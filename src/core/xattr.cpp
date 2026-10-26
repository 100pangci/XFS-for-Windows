// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - typed extended attribute reader / verifier (RW-6D).
//
// The code follows the v5 format checks in
//   libxfs/xfs_attr_leaf.c    (xfs_attr3_leaf_verify)
//   libxfs/xfs_da_btree.c     (xfs_da3_node_verify)
//   libxfs/xfs_attr_remote.c  (xfs_attr3_rmt_verify)
// and the inode-fork dispatch in libxfs/xfs_attr.c.  Every block is checked
// before it is interpreted; no guessed layout or unchecked pointer is used.
#include "xfs/xattr.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

#include "xfs/endian.h"
#include "xfs/parent.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/verifier.h"

namespace xfs {
namespace {

namespace bt = ondisk::btree;
namespace di = ondisk::inode;

// xfs_da3_blkinfo offsets (shared by attr leaf and DA node).
constexpr size_t kBlkForw = 0;
constexpr size_t kBlkBack = 4;
constexpr size_t kBlkMagic = 8;
constexpr size_t kBlkCrc = 12;
constexpr size_t kBlkBlkno = 16;
constexpr size_t kBlkLsn = 24;
constexpr size_t kBlkUuid = 32;
constexpr size_t kBlkOwner = 48;

// xfs_attr3_leaf_hdr after the common 56-byte info.
constexpr size_t kLeafCount = 56;
constexpr size_t kLeafUsedBytes = 58;
constexpr size_t kLeafFirstUsed = 60;
constexpr size_t kLeafHoles = 62;
constexpr size_t kLeafFreemap = 64;

// xfs_da3_node_hdr after the common 56-byte info.
constexpr size_t kNodeCount = 56;
constexpr size_t kNodeLevel = 58;
constexpr size_t kNodeEntries = 64;

// xfs_attr3_rmt_hdr.
constexpr size_t kRmtMagic = 0;
constexpr size_t kRmtOffset = 4;
constexpr size_t kRmtBytes = 8;
constexpr size_t kRmtCrc = 12;
constexpr size_t kRmtUuid = 16;
constexpr size_t kRmtOwner = 32;
constexpr size_t kRmtBlkno = 40;
constexpr size_t kRmtLsn = 48;

uint32_t Rol32(uint32_t v, unsigned n) {
  n &= 31;
  return n == 0 ? v : (v << n) | (v >> (32 - n));
}

bool IsParentNamespace(uint8_t flags) {
  return (flags & kAttrNspOnDiskMask) == kAttrParent;
}

// The on-disk attribute hash.  Ordinary names use xfs_da_hashname; parent
// pointers additionally mix the parent inode number (xfs_parent_hashval).
// The value is part of the key, so a parent-pointer implementation must not
// treat a bare name as unique.
uint32_t ExpectedEntryHash(const XattrEntry& entry) {
  if (IsParentNamespace(entry.flags) && !entry.remote &&
      entry.value.size() == kParentRecSize) {
    return ParentPointerHash(entry.name, GetBE64(entry.value.data(), 0));
  }
  return AttrHashName(entry.name);
}

bool ParentValueKey(const XattrEntry& entry, std::string* key) {
  if (!IsParentNamespace(entry.flags)) {
    key->clear();
    return true;
  }
  if (entry.remote || entry.value.size() != kParentRecSize) return false;
  key->assign(reinterpret_cast<const char*>(entry.value.data()),
              entry.value.size());
  return true;
}

struct LeafHeader {
  uint32_t forw = 0;
  uint32_t back = 0;
  uint16_t count = 0;
  uint16_t usedbytes = 0;
  uint32_t firstused = 0;
  uint8_t holes = 0;
  struct Map {
    uint16_t base = 0;
    uint16_t size = 0;
  } freemap[kAttrLeafMapSize];
};

struct NodeEntry {
  uint32_t hash = 0;
  uint32_t before = 0;
};

struct NodeHeader {
  uint32_t forw = 0;
  uint32_t back = 0;
  uint16_t count = 0;
  uint16_t level = 0;
};

size_t EntriesEnd(size_t header_size, uint16_t count) {
  return header_size + static_cast<size_t>(count) * kAttrLeafEntrySize;
}

// Decodes firstused.  A zero on-disk value means the 64 KiB block size (the
// field is 16-bit); see xfs_attr3_leaf_firstused_from_disk().
Status DecodeFirstUsed(uint16_t raw, uint32_t block_size, uint32_t* out) {
  if (raw == 0) {
    if (block_size <= 65535) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "attr leaf firstused is the 64k sentinel on a smaller "
                     "block")
          .Metadata("attr leaf");
    }
    *out = block_size;
    return OkStatus();
  }
  *out = raw;
  return OkStatus();
}

Status DecodeLeafHeader(const uint8_t* block, uint32_t block_size,
                        LeafHeader* out) {
  if (GetBE16(block, kBlkMagic) != kAttrLeafMagicV5) {
    return XFS_ERR(XfsError::kBadMagic, "attr leaf magic mismatch")
        .Metadata("attr leaf")
        .Expected(kAttrLeafMagicV5)
        .Actual(GetBE16(block, kBlkMagic));
  }
  out->forw = GetBE32(block, kBlkForw);
  out->back = GetBE32(block, kBlkBack);
  out->count = GetBE16(block, kLeafCount);
  out->usedbytes = GetBE16(block, kLeafUsedBytes);
  Status s = DecodeFirstUsed(GetBE16(block, kLeafFirstUsed), block_size,
                             &out->firstused);
  if (!s.ok()) return s;
  out->holes = block[kLeafHoles];
  for (size_t i = 0; i < kAttrLeafMapSize; ++i) {
    out->freemap[i].base = GetBE16(block, kLeafFreemap + i * 4);
    out->freemap[i].size = GetBE16(block, kLeafFreemap + i * 4 + 2);
  }
  return OkStatus();
}

Status DecodeNodeHeader(const uint8_t* block, NodeHeader* out) {
  if (GetBE16(block, kBlkMagic) != kAttrDaNodeMagicV5) {
    return XFS_ERR(XfsError::kBadMagic, "attr node magic mismatch")
        .Metadata("attr node")
        .Expected(kAttrDaNodeMagicV5)
        .Actual(GetBE16(block, kBlkMagic));
  }
  out->forw = GetBE32(block, kBlkForw);
  out->back = GetBE32(block, kBlkBack);
  out->count = GetBE16(block, kNodeCount);
  out->level = GetBE16(block, kNodeLevel);
  return OkStatus();
}

// Common xfs_da3_blkinfo verification for leaf/node blocks.
Status VerifyDaHeader(const uint8_t* block, uint32_t block_size,
                      uint64_t fsb, uint64_t ino, const XfsGeometry& geo,
                      uint64_t sb_lsn, const char* what) {
  Status s = VerifyChecksum(block, block_size, kBlkCrc, what);
  if (!s.ok()) {
    auto err = s.error();
    err.ino = ino;
    err.fsb = fsb;
    return err;
  }
  s = VerifyUuidField(block, block_size, kBlkUuid, geo.meta_uuid, what);
  if (!s.ok()) {
    auto err = s.error();
    err.ino = ino;
    err.fsb = fsb;
    return err;
  }
  s = VerifyBlockNoField(block, block_size, kBlkBlkno, geo.FsbToByte(fsb) / 512,
                         what);
  if (!s.ok()) {
    auto err = s.error();
    err.ino = ino;
    err.fsb = fsb;
    return err;
  }
  s = VerifyOwnerField(block, block_size, kBlkOwner, 8, ino, what);
  if (!s.ok()) {
    auto err = s.error();
    err.ino = ino;
    err.fsb = fsb;
    return err;
  }
  s = VerifyLsnField(block, block_size, kBlkLsn, sb_lsn, what);
  if (!s.ok()) {
    auto err = s.error();
    err.ino = ino;
    err.fsb = fsb;
    return err;
  }
  return OkStatus();
}

// Attribute-fork extent loader.  The on-disk bmbt and extent-record formats are
// shared with the data fork; LoadDataForkExtents only needs a fork view, so a
// shallow synthetic inode is enough.  The bmbt header owner is the inode, not
// a data/attr discriminator, which matches Linux.
Status LoadAttrExtents(BlockDevice& device, const XfsGeometry& geo,
                       const XfsInode& inode, std::vector<XfsExtent>* out) {
  XfsInode synth;
  synth.ino = inode.ino;
  synth.format = inode.aformat;
  synth.nextents = inode.anextents;
  synth.data_fork = inode.attr_fork;
  Status s = LoadDataForkExtents(device, geo, synth, out, 1u << 24);
  if (!s.ok()) return s;
  if (out->size() != inode.anextents) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "attr extent count does not match the extent map")
        .Ino(inode.ino)
        .Metadata("inode")
        .Expected(inode.anextents)
        .Actual(out->size());
  }
  for (const XfsExtent& e : *out) {
    if (e.unwritten) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "attr fork contains an unwritten extent")
          .Ino(inode.ino)
          .Metadata("inode")
          .Fsb(e.startblock);
    }
  }
  return OkStatus();
}

bool MapLogical(const std::vector<XfsExtent>& extents, uint64_t logical,
                uint64_t* fsb) {
  for (const XfsExtent& e : extents) {
    if (logical >= e.startoff && logical < e.EndBlock()) {
      *fsb = e.startblock + (logical - e.startoff);
      return true;
    }
  }
  return false;
}

Status ReadBlock(BlockDevice& device, const XfsGeometry& geo, uint64_t fsb,
                 std::vector<uint8_t>* out) {
  if (!geo.FsbValid(fsb)) {
    return XFS_ERR(XfsError::kCorruptInode, "attr block is outside the fs")
        .Metadata("attr fork")
        .Fsb(fsb);
  }
  out->assign(geo.block_size, 0);
  if (!device.Read(geo.FsbToByte(fsb), out->data(), out->size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read attr block")
        .Metadata("attr fork")
        .Fsb(fsb)
        .Offset(geo.FsbToByte(fsb));
  }
  return OkStatus();
}

// Validates the leaf free-space map exactly as xfs_attr3_leaf_verify does.
Status VerifyFreeMap(const LeafHeader& hdr, uint32_t block_size) {
  const size_t entries_end = EntriesEnd(kAttrLeafHeaderSize, hdr.count);
  for (size_t i = 0; i < kAttrLeafMapSize; ++i) {
    const uint32_t base = hdr.freemap[i].base;
    const uint32_t size = hdr.freemap[i].size;
    if (base > block_size || size > block_size) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "attr leaf freemap entry out of range")
          .Metadata("attr leaf")
          .Actual(i);
    }
    if ((base & 3) || (size & 3)) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "attr leaf freemap entry is not 4-byte aligned")
          .Metadata("attr leaf")
          .Actual(i);
    }
    if (base + size < base || base + size > block_size) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "attr leaf freemap range overflows the block")
          .Metadata("attr leaf")
          .Actual(i);
    }
    if (size != 0 && base < entries_end) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "attr leaf freemap overlaps the entry table")
          .Metadata("attr leaf")
          .Actual(i);
    }
  }
  for (size_t a = 0; a < kAttrLeafMapSize; ++a) {
    for (size_t b = a + 1; b < kAttrLeafMapSize; ++b) {
      if (hdr.freemap[a].size == 0 || hdr.freemap[b].size == 0) continue;
      const uint32_t aend = hdr.freemap[a].base + hdr.freemap[a].size;
      const uint32_t bend = hdr.freemap[b].base + hdr.freemap[b].size;
      if (hdr.freemap[a].base < bend && aend > hdr.freemap[b].base) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "attr leaf freemap entries overlap")
            .Metadata("attr leaf");
      }
    }
  }
  return OkStatus();
}

Status ParseLeafEntries(const uint8_t* block, uint32_t block_size,
                        const LeafHeader& hdr, uint64_t ino,
                        std::vector<XattrEntry>* out) {
  const size_t entries_end = EntriesEnd(kAttrLeafHeaderSize, hdr.count);
  if (entries_end > hdr.firstused || hdr.firstused > block_size) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "attr leaf entry table overlaps firstused")
        .Ino(ino)
        .Metadata("attr leaf")
        .Expected(hdr.firstused)
        .Actual(entries_end);
  }
  if (hdr.count == 0) {
    if (hdr.usedbytes != 0) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "empty attr leaf has nonzero usedbytes")
          .Ino(ino)
          .Metadata("attr leaf");
    }
  }
  out->clear();
  out->reserve(hdr.count);
  uint32_t last_hash = 0;
  size_t used = 0;
  for (uint16_t i = 0; i < hdr.count; ++i) {
    const uint8_t* ent = block + kAttrLeafHeaderSize + i * kAttrLeafEntrySize;
    XattrEntry item;
    item.hash = GetBE32(ent, 0);
    const uint16_t nameidx = GetBE16(ent, 4);
    item.flags = ent[6];
    if (item.hash < last_hash) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "attr leaf hashes are not sorted")
          .Ino(ino)
          .Metadata("attr leaf")
          .Actual(i);
    }
    last_hash = item.hash;
    if ((item.flags & ~kAttrOnDiskMask) != 0) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "attr leaf entry has unknown flag bits")
          .Ino(ino)
          .Metadata("attr leaf")
          .Actual(item.flags);
    }
    uint8_t nsp = item.flags & kAttrNspOnDiskMask;
    if (nsp != 0 && nsp != kAttrRoot && nsp != kAttrSecure &&
        nsp != kAttrParent) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "attr leaf entry has an invalid namespace combination")
          .Ino(ino)
          .Metadata("attr leaf")
          .Actual(nsp);
    }
    if (nameidx < hdr.firstused || nameidx >= block_size) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "attr leaf name pointer is out of range")
          .Ino(ino)
          .Metadata("attr leaf")
          .Actual(nameidx);
    }
    if (item.IsLocal()) {
      if (static_cast<size_t>(nameidx) + 3 > block_size) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "attr local name header is truncated")
            .Ino(ino)
            .Metadata("attr leaf");
      }
      const uint16_t valuelen = GetBE16(block, nameidx);
      const uint8_t namelen = block[nameidx + 2];
      if (namelen == 0) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "attr local name is empty")
            .Ino(ino)
            .Metadata("attr leaf");
      }
      const size_t size = AttrLocalEntrySize(namelen, valuelen);
      if (static_cast<size_t>(nameidx) + size > block_size) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "attr local name/value exceeds the block")
            .Ino(ino)
            .Metadata("attr leaf")
            .Actual(nameidx);
      }
      item.name.assign(reinterpret_cast<const char*>(block + nameidx + 3),
                       namelen);
      item.value.assign(block + nameidx + 3 + namelen,
                        block + nameidx + 3 + namelen + valuelen);
      item.remote = false;
      used += size;
    } else {
      if (static_cast<size_t>(nameidx) + 9 > block_size) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "attr remote name header is truncated")
            .Ino(ino)
            .Metadata("attr leaf");
      }
      const uint32_t valueblk = GetBE32(block, nameidx);
      const uint32_t valuelen = GetBE32(block, nameidx + 4);
      const uint8_t namelen = block[nameidx + 8];
      if (namelen == 0) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "attr remote name is empty")
            .Ino(ino)
            .Metadata("attr leaf");
      }
      if (valuelen > kXattrMaxValueBytes) {
        return XFS_ERR(XfsError::kOutOfRange,
                       "attr remote value exceeds the XFS limit")
            .Ino(ino)
            .Metadata("attr leaf")
            .Actual(valuelen);
      }
      if (!item.IsIncomplete() && valueblk == 0) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "complete attr remote entry has no value block")
            .Ino(ino)
            .Metadata("attr leaf");
      }
      const size_t size = AttrRemoteEntrySize(namelen);
      if (static_cast<size_t>(nameidx) + size > block_size) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "attr remote name exceeds the block")
            .Ino(ino)
            .Metadata("attr leaf")
            .Actual(nameidx);
      }
      item.name.assign(reinterpret_cast<const char*>(block + nameidx + 9),
                       namelen);
      item.remote = true;
      item.remote_lbno = valueblk;
      item.remote_blocks =
          valuelen == 0 ? 0
                        : static_cast<uint32_t>(
                              (valuelen + (block_size - kAttrRemoteHeaderSize) -
                               1) /
                              (block_size - kAttrRemoteHeaderSize));
      item.remote_value_len = valuelen;
      // The value bytes are filled by ReadRemoteValue after all blocks have
      // been verified.
      item.value.clear();
      used += size;
    }
    item.incomplete = item.IsIncomplete();
    Status s = ValidateXattrName(item.name, item.flags, true);
    if (!s.ok()) {
      auto err = s.error();
      err.ino = ino;
      err.metadata = "attr leaf";
      return err;
    }
    out->push_back(std::move(item));
  }
  if (used != hdr.usedbytes) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "attr leaf usedbytes does not match its entries")
        .Ino(ino)
        .Metadata("attr leaf")
        .Expected(used)
        .Actual(hdr.usedbytes);
  }
  return OkStatus();
}

// Reads and verifies one remote value; `entry` is completed in place.
Status ReadRemoteValue(BlockDevice& device, const XfsGeometry& geo,
                       const XfsInode& inode, const std::vector<XfsExtent>& extents,
                       const XattrEntry& leaf_entry, uint64_t sb_lsn,
                       XattrEntry* entry) {
  if (leaf_entry.remote_blocks == 0) {
    if (leaf_entry.remote_value_len != 0) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "remote attr has a value length but no blocks")
          .Ino(inode.ino)
          .Metadata("attr remote")
          .Actual(leaf_entry.remote_value_len);
    }
    entry->value.clear();
    entry->remote = true;
    entry->remote_lbno = leaf_entry.remote_lbno;
    entry->remote_blocks = 0;
    entry->remote_value_len = 0;
    entry->name = leaf_entry.name;
    entry->flags = leaf_entry.flags;
    entry->hash = leaf_entry.hash;
    entry->incomplete = leaf_entry.incomplete;
    return OkStatus();
  }
  const uint32_t space = geo.block_size - kAttrRemoteHeaderSize;
  std::vector<uint8_t> value;
  uint32_t offset = 0;
  uint64_t logical = leaf_entry.remote_lbno;
  for (uint32_t i = 0; i < leaf_entry.remote_blocks; ++i) {
    uint64_t fsb = 0;
    if (!MapLogical(extents, logical, &fsb)) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "remote attr value block is not mapped")
          .Ino(inode.ino)
          .Metadata("attr remote")
          .Actual(logical);
    }
    std::vector<uint8_t> block;
    Status s = ReadBlock(device, geo, fsb, &block);
    if (!s.ok()) {
      auto err = s.error();
      err.ino = inode.ino;
      return err;
    }
    if (GetBE32(block.data(), kRmtMagic) != ondisk::kAttr3RmtMagic) {
      return XFS_ERR(XfsError::kBadMagic, "remote attr block magic mismatch")
          .Ino(inode.ino)
          .Fsb(fsb)
          .Metadata("attr remote")
          .Expected(ondisk::kAttr3RmtMagic)
          .Actual(GetBE32(block.data(), kRmtMagic));
    }
    s = VerifyChecksum(block.data(), block.size(), kRmtCrc, "attr remote");
    if (!s.ok()) {
      auto err = s.error();
      err.ino = inode.ino;
      err.fsb = fsb;
      return err;
    }
    s = VerifyUuidField(block.data(), block.size(), kRmtUuid, geo.meta_uuid,
                        "attr remote");
    if (!s.ok()) {
      auto err = s.error();
      err.ino = inode.ino;
      err.fsb = fsb;
      return err;
    }
    s = VerifyBlockNoField(block.data(), block.size(), kRmtBlkno,
                           geo.FsbToByte(fsb) / 512, "attr remote");
    if (!s.ok()) {
      auto err = s.error();
      err.ino = inode.ino;
      err.fsb = fsb;
      return err;
    }
    s = VerifyOwnerField(block.data(), block.size(), kRmtOwner, 8, inode.ino,
                         "attr remote");
    if (!s.ok()) {
      auto err = s.error();
      err.ino = inode.ino;
      err.fsb = fsb;
      return err;
    }
    const uint32_t hdr_offset = GetBE32(block.data(), kRmtOffset);
    const uint32_t bytes = GetBE32(block.data(), kRmtBytes);
    if (hdr_offset != offset) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "remote attr block offset is not contiguous")
          .Ino(inode.ino)
          .Fsb(fsb)
          .Metadata("attr remote")
          .Expected(offset)
          .Actual(hdr_offset);
    }
    if (bytes > space || offset + bytes > kXattrMaxValueBytes) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "remote attr block byte count is out of range")
          .Ino(inode.ino)
          .Fsb(fsb)
          .Metadata("attr remote")
          .Actual(bytes);
    }
    const uint64_t lsn = GetBE64(block.data(), kRmtLsn);
    if (lsn != kAttrNullCommitLsn) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "remote attr block has a non-null commit LSN")
          .Ino(inode.ino)
          .Fsb(fsb)
          .Metadata("attr remote")
          .Actual(lsn);
    }
    if (i + 1 < leaf_entry.remote_blocks && bytes != space) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "non-final remote attr block is not full")
          .Ino(inode.ino)
          .Fsb(fsb)
          .Metadata("attr remote")
          .Expected(space)
          .Actual(bytes);
    }
    value.insert(value.end(), block.begin() + kAttrRemoteHeaderSize,
                 block.begin() + kAttrRemoteHeaderSize + bytes);
    offset += bytes;
    ++logical;
  }
  if (offset != leaf_entry.remote_value_len) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "remote attr value length does not match its leaf entry")
        .Ino(inode.ino)
        .Metadata("attr remote")
        .Expected(leaf_entry.remote_value_len)
        .Actual(offset);
  }
  entry->value = std::move(value);
  entry->remote = true;
  entry->remote_lbno = leaf_entry.remote_lbno;
  entry->remote_blocks = leaf_entry.remote_blocks;
  entry->remote_value_len = leaf_entry.remote_value_len;
  entry->name = leaf_entry.name;
  entry->flags = leaf_entry.flags;
  entry->hash = leaf_entry.hash;
  entry->incomplete = leaf_entry.incomplete;
  return OkStatus();
}

// Verifies the leaf free map and returns its decoded header + entries.
Status ReadLeafBlock(BlockDevice& device, const XfsGeometry& geo,
                     const XfsInode& inode, const std::vector<XfsExtent>& extents,
                     uint32_t logical, uint64_t sb_lsn,
                     std::vector<XattrEntry>* entries, uint32_t* forw,
                     uint32_t* back, uint64_t* fsb_out) {
  uint64_t fsb = 0;
  if (!MapLogical(extents, logical, &fsb)) {
    return XFS_ERR(XfsError::kCorruptInode, "attr leaf is not mapped")
        .Ino(inode.ino)
        .Metadata("attr fork")
        .Actual(logical);
  }
  std::vector<uint8_t> block;
  Status s = ReadBlock(device, geo, fsb, &block);
  if (!s.ok()) return s;
  LeafHeader hdr;
  s = DecodeLeafHeader(block.data(), geo.block_size, &hdr);
  if (!s.ok()) {
    auto err = s.error();
    err.ino = inode.ino;
    err.fsb = fsb;
    return err;
  }
  s = VerifyDaHeader(block.data(), geo.block_size, fsb, inode.ino, geo,
                     sb_lsn, "attr leaf");
  if (!s.ok()) return s;
  s = VerifyFreeMap(hdr, geo.block_size);
  if (!s.ok()) {
    auto err = s.error();
    err.ino = inode.ino;
    err.fsb = fsb;
    return err;
  }
  s = ParseLeafEntries(block.data(), geo.block_size, hdr, inode.ino, entries);
  if (!s.ok()) return s;
  *forw = hdr.forw;
  *back = hdr.back;
  *fsb_out = fsb;
  return OkStatus();
}

struct CollectedLeaf {
  uint32_t logical = 0;
  uint32_t forw = 0;
  uint32_t back = 0;
  uint64_t fsb = 0;
  std::vector<XattrEntry> entries;
};

Status CollectTree(BlockDevice& device, const XfsGeometry& geo,
                   const XfsInode& inode, const std::vector<XfsExtent>& extents,
                   uint64_t sb_lsn, uint16_t* root_level,
                   std::vector<CollectedLeaf>* leaves,
                   std::vector<XattrDaNode>* node_infos,
                   uint64_t* tree_blocks) {
  // Read the root at logical block 0 and dispatch on its magic.
  uint64_t root_fsb = 0;
  if (!MapLogical(extents, 0, &root_fsb)) {
    return XFS_ERR(XfsError::kCorruptInode, "attr tree root is not mapped")
        .Ino(inode.ino)
        .Metadata("attr fork");
  }
  std::vector<uint8_t> root;
  Status s = ReadBlock(device, geo, root_fsb, &root);
  if (!s.ok()) return s;
  const uint16_t root_magic = GetBE16(root.data(), kBlkMagic);
  if (root_magic == kAttrLeafMagicV5) {
    CollectedLeaf leaf;
    s = ReadLeafBlock(device, geo, inode, extents, 0, sb_lsn, &leaf.entries,
                      &leaf.forw, &leaf.back, &leaf.fsb);
    if (!s.ok()) return s;
    leaf.logical = 0;
    leaves->push_back(std::move(leaf));
    *root_level = 0;
    *tree_blocks = 1;
    return OkStatus();

  }
  if (root_magic != kAttrDaNodeMagicV5) {
    return XFS_ERR(XfsError::kBadMagic,
                   "attr tree root is neither a leaf nor a node")
        .Ino(inode.ino)
        .Fsb(root_fsb)
        .Metadata("attr fork")
        .Actual(root_magic);
  }

  struct NodeBlock {
    uint32_t logical = 0;
    uint64_t fsb = 0;
    NodeHeader hdr;
    std::vector<NodeEntry> entries;
  };
  std::map<uint32_t, NodeBlock> nodes;
  std::set<uint32_t> visited;
  std::map<uint16_t, std::vector<uint32_t>> level_order;
  struct Frame {
    uint32_t logical = 0;
    uint16_t level = 0;
  };
  std::vector<Frame> stack;
  stack.push_back({0, 0});  // 0 is filled after reading the root

  auto read_node = [&](uint32_t logical, uint16_t expected_level,
                       NodeBlock* out) -> Status {
    if (visited.count(logical)) {
      return XFS_ERR(XfsError::kCorruptBtree, "attr DA tree has a cycle")
          .Ino(inode.ino)
          .Metadata("attr node")
          .Actual(logical);
    }
    visited.insert(logical);
    uint64_t fsb = 0;
    if (!MapLogical(extents, logical, &fsb)) {
      return XFS_ERR(XfsError::kCorruptInode, "attr node is not mapped")
          .Ino(inode.ino)
          .Metadata("attr node")
          .Actual(logical);
    }
    std::vector<uint8_t> block;
    Status st = ReadBlock(device, geo, fsb, &block);
    if (!st.ok()) return st;
    NodeHeader hdr;
    st = DecodeNodeHeader(block.data(), &hdr);
    if (!st.ok()) {
      auto err = st.error();
      err.ino = inode.ino;
      err.fsb = fsb;
      return err;
    }
    st = VerifyDaHeader(block.data(), geo.block_size, fsb, inode.ino, geo,
                        sb_lsn, "attr node");
    if (!st.ok()) return st;
    if (hdr.level == 0 || hdr.level > kAttrMaxTreeDepth) {
      return XFS_ERR(XfsError::kCorruptBtree, "attr node level is out of range")
          .Ino(inode.ino)
          .Fsb(fsb)
          .Metadata("attr node")
          .Actual(hdr.level);
    }
    if (expected_level != 0 && hdr.level != expected_level) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "attr node level does not match its parent")
          .Ino(inode.ino)
          .Fsb(fsb)
          .Metadata("attr node")
          .Expected(expected_level)
          .Actual(hdr.level);
    }
    const size_t entries_end = static_cast<size_t>(kNodeEntries) +
                               static_cast<size_t>(hdr.count) * kAttrNodeEntrySize;
    if (hdr.count == 0 || entries_end > geo.block_size) {
      return XFS_ERR(XfsError::kCorruptBtree, "attr node count is out of range")
          .Ino(inode.ino)
          .Fsb(fsb)
          .Metadata("attr node")
          .Actual(hdr.count);
    }
    out->logical = logical;
    out->fsb = fsb;
    out->hdr = hdr;
    out->entries.clear();
    out->entries.reserve(hdr.count);
    uint32_t last_hash = 0;
    for (uint16_t i = 0; i < hdr.count; ++i) {
      const uint8_t* p = block.data() + kNodeEntries + i * kAttrNodeEntrySize;
      NodeEntry e;
      e.hash = GetBE32(p, 0);
      e.before = GetBE32(p, 4);
      if (i > 0 && e.hash < last_hash) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "attr node keys are not sorted")
            .Ino(inode.ino)
            .Fsb(fsb)
            .Metadata("attr node")
            .Actual(i);
      }
      last_hash = e.hash;
      if (e.before == 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "attr node has a null child pointer")
            .Ino(inode.ino)
            .Fsb(fsb)
            .Metadata("attr node");
      }
      out->entries.push_back(e);
    }
    level_order[hdr.level].push_back(logical);
    return OkStatus();
  };

  NodeBlock root_node;
  s = read_node(0, 0, &root_node);
  if (!s.ok()) return s;
  const uint16_t root_level_v = root_node.hdr.level;
  // Re-read the root with the correct expected level: read_node(0,0) was only
  // a header probe, so validate that the stored level is consistent.
  if (root_node.hdr.level != GetBE16(root.data(), kNodeLevel)) {
    return XFS_ERR(XfsError::kInternal, "attr node root header mismatch")
        .Ino(inode.ino)
        .Metadata("attr node");
  }
  visited.clear();
  nodes.clear();
  level_order.clear();
  s = read_node(0, root_level_v, &root_node);
  if (!s.ok()) return s;
  nodes.emplace(0, root_node);
  if (root_node.hdr.forw != 0 || root_node.hdr.back != 0) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "attr DA tree root has sibling links")
        .Ino(inode.ino)
        .Metadata("attr node");
  }

  // DFS in key order; children are visited left to right.
  struct Work {
    uint32_t logical;
    uint16_t level;
  };
  std::vector<Work> work;
  for (auto it = root_node.entries.rbegin(); it != root_node.entries.rend();
       ++it) {
    work.push_back({it->before, static_cast<uint16_t>(root_level_v - 1)});
  }
  while (!work.empty()) {
    Work w = work.back();
    work.pop_back();
    if (w.level == 0) {
      CollectedLeaf leaf;
      s = ReadLeafBlock(device, geo, inode, extents, w.logical, sb_lsn,
                        &leaf.entries, &leaf.forw, &leaf.back, &leaf.fsb);
      if (!s.ok()) return s;
      leaf.logical = w.logical;
      leaves->push_back(std::move(leaf));
      continue;
    }
    NodeBlock node;
    s = read_node(w.logical, w.level, &node);
    if (!s.ok()) return s;
    nodes.emplace(w.logical, node);
    for (auto it = node.entries.rbegin(); it != node.entries.rend(); ++it) {
      work.push_back({it->before, static_cast<uint16_t>(w.level - 1)});
    }
  }

  // Validate node keys against the last hash of their child subtree.
  for (const auto& [logical, node] : nodes) {
    for (const NodeEntry& e : node.entries) {
      uint32_t child_last = 0;
      auto leaf_it = std::find_if(
          leaves->begin(), leaves->end(),
          [&](const CollectedLeaf& l) { return l.logical == e.before; });
      if (leaf_it != leaves->end()) {
        if (!leaf_it->entries.empty()) {
          child_last = leaf_it->entries.back().hash;
        }
      } else {
        auto node_it = nodes.find(e.before);
        if (node_it == nodes.end()) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "attr node child is unreachable")
              .Ino(inode.ino)
              .Metadata("attr node")
              .Actual(e.before);
        }
        if (!node_it->second.entries.empty()) {
          child_last = node_it->second.entries.back().hash;
        }
      }
      if (child_last != e.hash) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "attr node key does not match its child")
            .Ino(inode.ino)
            .Metadata("attr node")
            .Expected(e.hash)
            .Actual(child_last);
      }
    }
  }

  // Leaves must be linked in exactly traversal order.
  for (size_t i = 0; i < leaves->size(); ++i) {
    const uint32_t want_back = i == 0 ? 0 : (*leaves)[i - 1].logical;
    const uint32_t want_forw =
        i + 1 == leaves->size() ? 0 : (*leaves)[i + 1].logical;
    if ((*leaves)[i].back != want_back || (*leaves)[i].forw != want_forw) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "attr leaf sibling chain is inconsistent")
          .Ino(inode.ino)
          .Metadata("attr leaf")
          .Actual((*leaves)[i].logical);
    }
  }

  // Nodes at each level must also form a doubly linked chain in key order.
  for (const auto& [level, order] : level_order) {
    for (size_t i = 0; i < order.size(); ++i) {
      const NodeBlock& n = nodes.at(order[i]);
      const uint32_t want_back = i == 0 ? 0 : order[i - 1];
      const uint32_t want_forw = i + 1 == order.size() ? 0 : order[i + 1];
      if (n.hdr.back != want_back || n.hdr.forw != want_forw) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "attr node sibling chain is inconsistent")
            .Ino(inode.ino)
            .Metadata("attr node")
            .Actual(order[i]);
      }
    }
  }

  if (node_infos != nullptr) {
    node_infos->clear();
    for (const auto& [logical, node] : nodes) {
      XattrDaNode info;
      info.logical = logical;
      info.forw = node.hdr.forw;
      info.back = node.hdr.back;
      info.level = node.hdr.level;
      for (const NodeEntry& e : node.entries) {
        info.entries.push_back({e.hash, e.before});
      }
      node_infos->push_back(std::move(info));
    }
  }
  *root_level = root_level_v;
  *tree_blocks = nodes.size() + leaves->size();
  return OkStatus();
}

Status ParseShortform(const XfsInode& inode, std::vector<XattrEntry>* out) {
  const std::vector<uint8_t>& fork = inode.attr_fork;
  if (fork.size() < 4) {
    return XFS_ERR(XfsError::kCorruptInode, "shortform attr fork is too small")
        .Ino(inode.ino)
        .Metadata("attr shortform");
  }
  const uint16_t totsize = GetBE16(fork.data(), 0);
  const uint8_t count = fork[2];
  if (totsize < 4 || totsize > fork.size()) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "shortform attr totsize is out of range")
        .Ino(inode.ino)
        .Metadata("attr shortform")
        .Expected(fork.size())
        .Actual(totsize);
  }
  size_t pos = 4;
  out->clear();
  out->reserve(count);
  for (uint8_t i = 0; i < count; ++i) {
    if (pos + 3 > totsize) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "shortform attr entry header is truncated")
          .Ino(inode.ino)
          .Metadata("attr shortform")
          .Actual(i);
    }
    const uint8_t namelen = fork[pos];
    const uint8_t valuelen = fork[pos + 1];
    const uint8_t flags = fork[pos + 2];
    if (namelen == 0) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "shortform attr name is empty")
          .Ino(inode.ino)
          .Metadata("attr shortform");
    }
    if ((flags & ~kAttrNspOnDiskMask) != 0) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "shortform attr has LOCAL/INCOMPLETE/unknown flags")
          .Ino(inode.ino)
          .Metadata("attr shortform")
          .Actual(flags);
    }
    const uint8_t nsp = flags & kAttrNspOnDiskMask;
    if (nsp != 0 && nsp != kAttrRoot && nsp != kAttrSecure &&
        nsp != kAttrParent) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "shortform attr namespace is invalid")
          .Ino(inode.ino)
          .Metadata("attr shortform")
          .Actual(nsp);
    }
    const size_t size = 3 + static_cast<size_t>(namelen) + valuelen;
    if (pos + size > totsize) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "shortform attr entry exceeds totsize")
          .Ino(inode.ino)
          .Metadata("attr shortform")
          .Actual(i);
    }
    XattrEntry e;
    e.flags = flags;
    e.name.assign(reinterpret_cast<const char*>(fork.data() + pos + 3),
                  namelen);
    e.value.assign(fork.begin() + static_cast<ptrdiff_t>(pos + 3 + namelen),
                   fork.begin() + static_cast<ptrdiff_t>(pos + size));
    e.hash = ExpectedEntryHash(e);
    e.remote = false;
    e.incomplete = false;
    Status s = ValidateXattrName(e.name, e.flags, false);
    if (!s.ok()) {
      auto err = s.error();
      err.ino = inode.ino;
      err.metadata = "attr shortform";
      return err;
    }
    out->push_back(std::move(e));
    pos += size;
  }
  if (pos != totsize) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "shortform attr entries do not exactly fill totsize")
        .Ino(inode.ino)
        .Metadata("attr shortform")
        .Expected(totsize)
        .Actual(pos);
  }
  return OkStatus();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public namespace / hash / validation helpers
// ---------------------------------------------------------------------------

uint32_t AttrHashName(const std::string& name) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(name.data());
  size_t namelen = name.size();
  uint32_t hash = 0;
  for (; namelen >= 4; namelen -= 4, p += 4) {
    hash = (p[0] << 21) ^ (p[1] << 14) ^ (p[2] << 7) ^ p[3] ^
           Rol32(hash, 7 * 4);
  }
  switch (namelen) {
    case 3:
      return (p[0] << 14) ^ (p[1] << 7) ^ p[2] ^ Rol32(hash, 21);
    case 2:
      return (p[0] << 7) ^ p[1] ^ Rol32(hash, 14);
    case 1:
      return p[0] ^ Rol32(hash, 7);
    default:
      return hash;
  }
}

size_t AttrLocalEntrySize(size_t name_len, size_t value_len) {
  const size_t size = 3 + name_len + value_len;
  return (size + 3) & ~size_t{3};
}

size_t AttrRemoteEntrySize(size_t name_len) {
  const size_t size = 11 + name_len;
  return (size + 3) & ~size_t{3};
}

size_t AttrLocalEntryMax(size_t block_size) {
  return (block_size >> 1) + (block_size >> 2);
}

XattrNamespace XattrNamespace::FromFlags(uint8_t flags) {
  XattrNamespace ns;
  if (flags & kAttrRoot) {
    ns.kind = XattrNamespaceKind::kTrusted;
    ns.flags = kAttrRoot;
  } else if (flags & kAttrSecure) {
    ns.kind = XattrNamespaceKind::kSecurity;
    ns.flags = kAttrSecure;
  } else if (flags & kAttrParent) {
    ns.kind = XattrNamespaceKind::kParent;
    ns.flags = kAttrParent;
  } else {
    ns.kind = XattrNamespaceKind::kUser;
    ns.flags = 0;
  }
  return ns;
}

Result<XattrNamespace> XattrNamespace::ParseName(std::string_view name,
                                                 std::string* on_disk_name,
                                                 bool allow_parent) {
  // VFS XATTR_NAME_MAX is 255 including the namespace prefix.
  if (name.size() > 255) {
    return XFS_ERR(XfsError::kNameTooLong, "xattr name exceeds XATTR_NAME_MAX")
        .Metadata("xattr")
        .Actual(name.size());
  }
  XattrNamespace ns;
  constexpr std::string_view kUser = "user.";
  constexpr std::string_view kTrusted = "trusted.";
  constexpr std::string_view kSecurity = "security.";
  constexpr std::string_view kSystem = "system.";
  if (name.substr(0, kUser.size()) == kUser) {
    ns.kind = XattrNamespaceKind::kUser;
    ns.flags = 0;
    *on_disk_name = std::string(name.substr(kUser.size()));
  } else if (name.substr(0, kTrusted.size()) == kTrusted) {
    ns.kind = XattrNamespaceKind::kTrusted;
    ns.flags = kAttrRoot;
    *on_disk_name = std::string(name.substr(kTrusted.size()));
  } else if (name.substr(0, kSecurity.size()) == kSecurity) {
    ns.kind = XattrNamespaceKind::kSecurity;
    ns.flags = kAttrSecure;
    *on_disk_name = std::string(name.substr(kSecurity.size()));
  } else if (name == "system.posix_acl_access") {
    ns.kind = XattrNamespaceKind::kSystemAcl;
    ns.flags = kAttrRoot;
    *on_disk_name = "SGI_ACL_FILE";
  } else if (name == "system.posix_acl_default") {
    ns.kind = XattrNamespaceKind::kSystemAcl;
    ns.flags = kAttrRoot;
    *on_disk_name = "SGI_ACL_DEFAULT";
  } else if (name.substr(0, kSystem.size()) == kSystem) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "unsupported system xattr namespace")
        .Metadata("xattr")
        .Actual(static_cast<uint64_t>(name.size()));
  } else if (name.substr(0, 7) == "parent.") {
    if (!allow_parent) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "parent-pointer xattrs are private and read-only")
          .Metadata("xattr");
    }
    ns.kind = XattrNamespaceKind::kParent;
    ns.flags = kAttrParent;
    *on_disk_name = std::string(name.substr(7));
  } else {
    // Raw native names (no VFS prefix) are accepted only through the explicit
    // namespace API, which supplies flags and the on-disk suffix directly.
    return XFS_ERR(XfsError::kInvalidArgument,
                   "xattr name must start with user., trusted., security., "
                   "system.posix_acl_*, or parent.")
        .Metadata("xattr");
  }
  Status s = ValidateXattrName(*on_disk_name, ns.flags);
  if (!s.ok()) return s.error();
  return ns;
}

const char* XattrNamespace::Prefix() const {
  switch (kind) {
    case XattrNamespaceKind::kUser: return "user.";
    case XattrNamespaceKind::kTrusted: return "trusted.";
    case XattrNamespaceKind::kSecurity: return "security.";
    case XattrNamespaceKind::kSystemAcl: return "system.";
    case XattrNamespaceKind::kParent: return "parent.";
    default: return "";
  }
}

std::string XattrNamespace::DisplayName(std::string_view on_disk_name) const {
  if (kind == XattrNamespaceKind::kTrusted &&
      on_disk_name == "SGI_ACL_FILE") {
    return "system.posix_acl_access";
  }
  if (kind == XattrNamespaceKind::kTrusted &&
      on_disk_name == "SGI_ACL_DEFAULT") {
    return "system.posix_acl_default";
  }
  return std::string(Prefix()) + std::string(on_disk_name);
}

std::string XattrEntry::DisplayName(bool include_private) const {
  if (incomplete) return std::string();
  XattrNamespace ns = Namespace();
  if (ns.IsPrivate() && !include_private) return std::string();
  return ns.DisplayName(name);
}

Status ValidateXattrName(std::string_view on_disk_name, uint8_t flags,
                         bool allow_incomplete) {
  uint8_t masked = flags & kAttrNspOnDiskMask;
  if (masked != 0 && masked != kAttrRoot && masked != kAttrSecure &&
      masked != kAttrParent) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "xattr has more than one namespace flag")
        .Metadata("xattr")
        .Actual(flags);
  }
  if (!allow_incomplete && (flags & kAttrIncomplete)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "xattr entry is marked incomplete")
        .Metadata("xattr")
        .Actual(flags);
  }
  if (on_disk_name.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument, "xattr name is empty")
        .Metadata("xattr");
  }
  if (on_disk_name.size() > kXattrMaxNameBytes) {
    return XFS_ERR(XfsError::kNameTooLong,
                   "xattr name is longer than MAXNAMELEN - 1")
        .Metadata("xattr")
        .Actual(on_disk_name.size());
  }
  if (on_disk_name.find('\0') != std::string_view::npos) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "xattr name contains a NUL byte")
        .Metadata("xattr");
  }
  if (masked == kAttrParent) {
    // Parent pointer names are directory-entry names, so they must also obey
    // xfs_dir2_namecheck and can never be a dot entry.  The 12-byte value is
    // validated by DecodeParentPointer once the namespace is known.
    Status name_ok = ValidateParentPointerName(on_disk_name);
    if (!name_ok.ok()) return name_ok;
  }
  return OkStatus();
}

Result<XattrFork> ReadXattrFork(BlockDevice& device, const XfsGeometry& geo,
                                const XfsInode& inode, uint64_t sb_lsn) {
  XattrFork fork;
  fork.forkoff = inode.forkoff;
  fork.aformat = inode.aformat;
  fork.anextents = inode.anextents;
  if (!inode.HasAttrFork()) {
    // Linux's xfs_inode_util initialises a new inode with
    // aformat=EXTENTS/anextents=0 even when di_forkoff is zero (a reserved
    // empty attribute fork).  Only a non-zero extent count or a format that
    // implies real fork data is corruption.
    if (inode.anextents != 0 ||
        (inode.aformat != 0 &&
         inode.aformat != di::kFormatExtents)) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "inode has attr metadata without an attr fork")
          .Ino(inode.ino)
          .Metadata("inode")
          .Actual(inode.aformat);
    }
    fork.format = XattrForkFormat::kNone;
    return fork;
  }
  if (inode.aformat == di::kFormatLocal) {
    if (inode.anextents != 0) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "shortform attr fork has extent counts")
          .Ino(inode.ino)
          .Metadata("inode")
          .Actual(inode.anextents);
    }
    fork.format = XattrForkFormat::kShortform;
    Status s = ParseShortform(inode, &fork.entries);
    if (!s.ok()) return s.error();
  } else if (inode.aformat == di::kFormatExtents ||
             inode.aformat == di::kFormatBtree) {
    Status s = LoadAttrExtents(device, geo, inode, &fork.extents);
    if (!s.ok()) return s.error();
    if (fork.extents.empty()) {
      fork.format = XattrForkFormat::kExtentsEmpty;
      return fork;
    }
    uint16_t root_level = 0;
    std::vector<CollectedLeaf> leaves;
    s = CollectTree(device, geo, inode, fork.extents, sb_lsn, &root_level,
                    &leaves, &fork.nodes, &fork.tree_blocks);
    if (!s.ok()) return s.error();
    fork.root_level = root_level;
    fork.leaf_count = static_cast<uint32_t>(leaves.size());
    fork.format = root_level == 0 ? XattrForkFormat::kLeaf
                                  : XattrForkFormat::kNode;
    // DA tree blocks and remote values share the attr-fork logical address
    // space.  Overlaps would make one overwrite the other.
    std::set<uint32_t> used_logical;
    for (const CollectedLeaf& leaf : leaves) {
      used_logical.insert(leaf.logical);
    }
    for (const XattrDaNode& node : fork.nodes) {
      used_logical.insert(node.logical);
    }
    // Read remote values and merge entries in leaf traversal order.
    for (CollectedLeaf& leaf : leaves) {
      XattrDaLeaf info;
      info.logical = leaf.logical;
      info.forw = leaf.forw;
      info.back = leaf.back;
      info.first_entry = static_cast<uint32_t>(fork.entries.size());
      info.entry_count = static_cast<uint32_t>(leaf.entries.size());
      for (XattrEntry& leaf_entry : leaf.entries) {
        if (!leaf_entry.remote) {
          fork.entries.push_back(std::move(leaf_entry));
          continue;
        }
        if (!leaf_entry.IsIncomplete()) {
          for (uint32_t i = 0; i < leaf_entry.remote_blocks; ++i) {
            if (!used_logical.insert(leaf_entry.remote_lbno + i).second) {
              return XFS_ERR(XfsError::kCorruptInode,
                             "remote attr value overlaps another attr block")
                  .Ino(inode.ino)
                  .Metadata("attr remote")
                  .Actual(leaf_entry.remote_lbno + i);
            }
          }
        }
        XattrEntry complete;
        s = ReadRemoteValue(device, geo, inode, fork.extents, leaf_entry,
                            sb_lsn, &complete);
        if (!s.ok()) return s.error();
        fork.remote_blocks += complete.remote_blocks;
        fork.entries.push_back(std::move(complete));
      }
      fork.leaves.push_back(info);
    }
  } else {
    return XFS_ERR(XfsError::kCorruptInode, "invalid attr fork format")
        .Ino(inode.ino)
        .Metadata("inode")
        .Actual(inode.aformat);
  }

  // Validate the hash of every complete entry.  For ordinary attributes the
  // hash is derived from the name; parent pointers mix in the parent inode
  // number, so the same name under two parents is not a duplicate key.
  for (const XattrEntry& e : fork.entries) {
    if (e.incomplete) continue;
    if (IsParentNamespace(e.flags)) {
      auto decoded = DecodeParentPointer(e, geo, inode.ino);
      if (!decoded.ok()) {
        auto error = decoded.error();
        error.ino = inode.ino;
        error.metadata = "attr fork";
        return error;
      }
    }
    const uint32_t expected = ExpectedEntryHash(e);
    if (e.hash != expected) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "attr entry hash does not match its key")
          .Ino(inode.ino)
          .Expected(expected)
          .Actual(e.hash)
          .Metadata("attr fork");
    }
  }

  // Leaf entries are globally hash-ordered.  Per-leaf ordering is checked by
  // the leaf parser; the cross-leaf check catches a broken DA tree that would
  // make lookup-by-hash miss an entry.
  if (fork.format == XattrForkFormat::kLeaf ||
      fork.format == XattrForkFormat::kNode) {
    uint32_t last_hash = 0;
    for (const XattrEntry& e : fork.entries) {
      if (e.hash < last_hash) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "attr entries are not in global hash order")
            .Ino(inode.ino)
            .Metadata("attr fork");
      }
      last_hash = e.hash;
    }
  }

  // The attribute key is (full 32-bit hash, name).  Parent pointers also
  // compare the parent record value, because the hash mixes the parent inode:
  // a same-name hard link under another parent is a distinct key, while an
  // exact duplicate is corruption.  A logged replace temporarily has a
  // complete old entry and an INCOMPLETE new entry; only completed entries
  // must be unique.
  std::set<std::tuple<uint8_t, uint32_t, std::string, std::string>> keys;
  for (const XattrEntry& e : fork.entries) {
    if (e.incomplete) continue;
    std::string value_key;
    if (!ParentValueKey(e, &value_key)) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "parent pointer has a non-local or malformed value")
          .Ino(inode.ino)
          .Metadata("attr fork");
    }
    // Namespace is part of the key: user.foo and trusted.foo share a name and
    // hash but are different attributes.  The parent value is part of the
    // key too, so same-name hard links under different parents are allowed.
    const auto key = std::make_tuple(
        static_cast<uint8_t>(e.flags & kAttrNspOnDiskMask), e.hash, e.name,
        value_key);
    if (!keys.insert(key).second) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "duplicate attribute key in fork")
          .Ino(inode.ino)
          .Metadata("attr fork");
    }
  }

  // A single leaf at logical 0 has no separate DA node.  The tree walker
  // already verified the leaf chain; a non-leaf root must have a leaf chain
  // whose order matches key order.
  return fork;
}

Result<std::vector<XattrEntry>> ListXattrs(BlockDevice& device,
                                          const XfsGeometry& geo,
                                          const XfsInode& inode,
                                          bool include_private) {
  auto fork = ReadXattrFork(device, geo, inode);
  if (!fork.ok()) return fork.error();
  std::vector<XattrEntry> out;
  for (XattrEntry& e : fork.value().entries) {
    if (e.incomplete) continue;
    XattrNamespace ns = XattrNamespace::FromFlags(e.flags);
    if (ns.IsPrivate() && !include_private) continue;
    out.push_back(std::move(e));
  }
  return out;
}

Result<XattrEntry> GetXattr(BlockDevice& device, const XfsGeometry& geo,
                            const XfsInode& inode, std::string_view name,
                            bool include_private) {
  std::string on_disk;
  auto ns = XattrNamespace::ParseName(name, &on_disk, include_private);
  if (!ns.ok()) return ns.error();
  auto fork = ReadXattrFork(device, geo, inode);
  if (!fork.ok()) return fork.error();
  for (const XattrEntry& e : fork.value().entries) {
    if (e.incomplete) continue;
    if ((e.flags & kAttrNspOnDiskMask) == ns.value().flags &&
        e.name == on_disk) {
      return e;
    }
  }
  return XFS_ERR(XfsError::kNotFound, "xattr not found")
      .Ino(inode.ino)
      .Metadata("xattr");
}

Result<uint64_t> AttrLogicalToPhysical(const XattrFork& fork,
                                       const XfsGeometry& geo,
                                       uint64_t logical_block) {
  uint64_t fsb = 0;
  if (!MapLogical(fork.extents, logical_block, &fsb)) {
    return XFS_ERR(XfsError::kNotFound,
                   "attr logical block is not mapped")
        .Metadata("attr fork")
        .Actual(logical_block);
  }
  if (!geo.FsbValid(fsb)) {
    return XFS_ERR(XfsError::kCorruptExtent,
                   "attr logical block maps outside the filesystem")
        .Metadata("attr fork")
        .Fsb(fsb);
  }
  return fsb;
}

}  // namespace xfs
