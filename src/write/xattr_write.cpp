// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - writable extended attributes (RW-6D).
//
// Mutation strategy (see docs/XATTR.md):
//   * shortform, single-leaf and node/node edits are incremental where the
//     representation does not change; a single leaf is rebuilt canonically and
//     the DA node path performs Linux-style split/merge/borrow/root collapse;
//   * representation transitions (none/shortform/leaf/node) build the target
//     structure directly and free the obsolete blocks in the same transaction;
//   * remote values are written and flushed before the metadata transaction
//     commits.  Replacing a value always allocates new remote blocks first and
//     frees the old blocks only after the new mapping is part of the same
//     logged transaction, so a crash leaves either the old or the new value;
//   * every attr-owned block goes through BlockAllocator with
//     RmapOwner::Attr / RmapOwner::Bmbt(attr=true).
#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "xfs/alloc.h"
#include "xfs/bmap.h"
#include "xfs/crash.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/log.h"
#include "xfs/log_format.h"
#include "xfs/ondisk.h"
#include "xfs/parent.h"
#include "xfs/transaction.h"
#include "xfs/xattr.h"
#include "write/xattr_write_internal.h"

namespace xfs {
namespace {

namespace bt = ondisk::btree;
namespace di = ondisk::inode;
namespace lf = logfmt;

constexpr size_t kLeafMagicOff = 8;
constexpr size_t kLeafCrcOff = 12;
constexpr size_t kLeafLsnOff = 24;
constexpr size_t kLeafHeader = kAttrLeafHeaderSize;
constexpr size_t kNodeHeader = kAttrNodeHeaderSize;

size_t NodeMaxRecs(uint32_t block_size) {
  return (block_size - kNodeHeader) / kAttrNodeEntrySize;
}

size_t EntrySize(const XattrEntry& e) {
  if (e.remote) return AttrRemoteEntrySize(e.name.size());
  return AttrLocalEntrySize(e.name.size(), e.value.size());
}

size_t LeafBytes(const std::vector<XattrEntry>& entries) {
  size_t used = 0;
  for (const XattrEntry& e : entries) used += EntrySize(e);
  return kLeafHeader + entries.size() * kAttrLeafEntrySize + used;
}

bool LeafFits(const std::vector<XattrEntry>& entries, uint32_t block_size) {
  return LeafBytes(entries) <= block_size;
}

uint32_t LastHash(const std::vector<XattrEntry>& entries) {
  return entries.empty() ? 0 : entries.back().hash;
}

std::vector<XattrEntry> SortByHash(std::vector<XattrEntry> entries) {
  std::stable_sort(entries.begin(), entries.end(),
                   [](const XattrEntry& a, const XattrEntry& b) {
                     return a.hash < b.hash;
                   });
  return entries;
}

bool IsParentEntry(const XattrEntry& e) {
  return (e.flags & kAttrNspOnDiskMask) == kAttrParent;
}

// xfs_attr_match(): namespace + name, and for parent pointers the exact
// 12-byte xfs_parent_rec value.  Two hard links with the same name under
// different parents have different hashes; a hash collision is still
// disambiguated by the value.
bool SameAttrKey(const XattrEntry& a, const XattrEntry& b) {
  if ((a.flags & kAttrNspOnDiskMask) != (b.flags & kAttrNspOnDiskMask)) {
    return false;
  }
  if (a.name != b.name) return false;
  if (IsParentEntry(a) || IsParentEntry(b)) {
    return a.hash == b.hash && a.value == b.value;
  }
  return true;
}

// The hash that must be written into a leaf/node entry.
uint32_t EntryHash(const XattrEntry& e) {
  if (IsParentEntry(e) && !e.remote && e.value.size() == kParentRecSize) {
    return ParentPointerHash(e.name, GetBE64(e.value.data(), 0));
  }
  return AttrHashName(e.name);
}

Status NormalizeEntryHash(XattrEntry* e, uint64_t ino) {
  if (e->incomplete) return OkStatus();
  if (IsParentEntry(*e)) {
    if (e->remote || e->value.size() != kParentRecSize) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "parent pointer has a non-local or malformed value")
          .Ino(ino)
          .Metadata("parent pointer");
    }
    Status name_ok = ValidateParentPointerName(e->name, ino);
    if (!name_ok.ok()) return name_ok;
  }
  e->hash = EntryHash(*e);
  return OkStatus();
}

// xfs_attr_leaf_newentsize(): a value is local when the resulting local entry
// is strictly smaller than 3/4 of the block; otherwise it is remote.  A value
// above 64 KiB is rejected before this point.
bool ValueMustBeRemote(const std::string& name, size_t value_len,
                       uint32_t block_size) {
  if (value_len > kXattrMaxValueBytes) return true;
  return AttrLocalEntrySize(name.size(), value_len) >=
         AttrLocalEntryMax(block_size);
}

uint32_t DefaultAttrForkOffRaw(uint32_t inode_size) {
  const uint32_t litino = inode_size - di::kCoreSizeV3;
  if (inode_size == 256) {
    return (litino - (4 + 2 * 16)) >> 3;  // xfs_bmdr_space_calc(MINABTPTRS)
  }
  return (4 + 6 * 2 * 16) >> 3;  // xfs_bmap_compute_attr_offset
}

uint32_t Litino(const XfsGeometry& geo) {
  return geo.inode_size - di::kCoreSizeV3;
}

// xfs_attr_shortform_bytesfit().  `bytes` is the total shortform size
// including the 4-byte header.  Returns the raw di_forkoff value (8-byte
// units), or 0 when the requested layout cannot fit.
uint32_t ShortformForkOff(const XfsInode& inode, const XfsGeometry& geo,
                          uint32_t bytes) {
  const uint32_t litino = Litino(geo);
  if (bytes > litino) return 0;
  int64_t offset = static_cast<int64_t>(litino - bytes) >> 3;
  const uint32_t current_attr_size =
      inode.forkoff ? litino - static_cast<uint32_t>(inode.forkoff) * 8 : 0;

  if (inode.format == di::kFormatDev) {
    const uint32_t minforkoff = 1;  // roundup(sizeof(xfs_dev_t), 8) >> 3
    return offset >= static_cast<int64_t>(minforkoff) ? minforkoff : 0;
  }
  if (bytes <= current_attr_size) return inode.forkoff;

  int64_t dsize = 0;
  const uint32_t default_offset =
      (geo.inode_size == 256)
          ? litino - (4 + 2 * 16)
          : (4 + 6 * 2 * 16);
  switch (inode.format) {
    case di::kFormatExtents:
      dsize = static_cast<int64_t>(inode.nextents) * 16;
      if (inode.forkoff == 0 && dsize > default_offset) {
        dsize = 4 + 3 * 16;  // xfs_bmdr_space_calc(MINDBTPTRS)
      }
      break;
    case di::kFormatBtree: {
      if (inode.forkoff != 0) {
        if (offset < inode.forkoff) return 0;
        return inode.forkoff;
      }
      if (inode.data_fork.size() < 4) return 0;
      const uint16_t numrecs = GetBE16(inode.data_fork.data(), 2);
      dsize = 4 + static_cast<int64_t>(numrecs) * 16;
      break;
    }
    case di::kFormatLocal:
      dsize = static_cast<int64_t>(inode.size);
      break;
    default:
      dsize = 0;
      break;
  }
  int64_t minforkoff = std::max<int64_t>(dsize, 4 + 3 * 16);
  minforkoff = (minforkoff + 7) >> 3;
  const int64_t maxforkoff =
      (static_cast<int64_t>(litino) - (4 + 2 * 16)) >> 3;
  if (offset >= maxforkoff) return static_cast<uint32_t>(maxforkoff);
  if (offset >= minforkoff) return static_cast<uint32_t>(offset);
  return 0;
}

bool CanUseShortform(const std::vector<XattrEntry>& entries,
                     const XfsInode& inode, const XfsGeometry& geo,
                     uint32_t* forkoff_out) {
  uint64_t total = 4;
  for (const XattrEntry& e : entries) {
    if (e.incomplete) return false;
    if (e.remote) return false;
    if (e.name.size() >= 255 || e.value.size() >= 255) return false;
    total += 3 + e.name.size() + e.value.size();
    if (total > Litino(geo)) return false;
  }
  uint32_t raw = ShortformForkOff(inode, geo, static_cast<uint32_t>(total));
  if (raw == 0) return false;
  *forkoff_out = raw;
  return true;
}

std::vector<uint8_t> BuildShortform(const std::vector<XattrEntry>& entries) {
  size_t total = 4;
  for (const XattrEntry& e : entries) {
    total += 3 + e.name.size() + e.value.size();
  }
  std::vector<uint8_t> out(total, 0);
  PutBE16(out.data(), 0, static_cast<uint16_t>(total));
  out[2] = static_cast<uint8_t>(entries.size());
  out[3] = 0;
  size_t pos = 4;
  for (const XattrEntry& e : entries) {
    out[pos] = static_cast<uint8_t>(e.name.size());
    out[pos + 1] = static_cast<uint8_t>(e.value.size());
    out[pos + 2] = static_cast<uint8_t>(e.flags & kAttrNspOnDiskMask);
    std::memcpy(out.data() + pos + 3, e.name.data(), e.name.size());
    if (!e.value.empty()) {
      std::memcpy(out.data() + pos + 3 + e.name.size(), e.value.data(),
                  e.value.size());
    }
    pos += 3 + e.name.size() + e.value.size();
  }
  return out;
}

// Canonical leaf builder.  Entries must be in hash order.  The block header
// carries magic/blkno/uuid/owner; the transaction layer stamps LSN and CRC.
std::vector<uint8_t> BuildLeaf(const XfsGeometry& geo, uint64_t ino,
                               uint32_t forw, uint32_t back,
                               const std::vector<XattrEntry>& input) {
  std::vector<XattrEntry> entries = input;
  std::stable_sort(entries.begin(), entries.end(),
                   [](const XattrEntry& a, const XattrEntry& b) {
                     return a.hash < b.hash;
                   });
  std::vector<uint8_t> block(geo.block_size, 0);
  PutBE32(block.data(), 0, forw);
  PutBE32(block.data(), 4, back);
  PutBE16(block.data(), kLeafMagicOff, kAttrLeafMagicV5);
  PutBE16(block.data(), 10, 0);  // pad16
  // The common header's blkno/uuid/owner.  Physical blkno is filled below.
  std::memcpy(block.data() + 32, geo.meta_uuid.data(), 16);
  PutBE64(block.data(), 48, ino);

  uint32_t cursor = geo.block_size;
  size_t usedbytes = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    XattrEntry& e = entries[i];
    const size_t size = EntrySize(e);
    cursor -= static_cast<uint32_t>(size);
    uint8_t* rec = block.data() + cursor;
    if (e.remote) {
      PutBE32(rec, 0, e.remote_lbno);
      PutBE32(rec, 4, e.remote_value_len);
      rec[8] = static_cast<uint8_t>(e.name.size());
      if (!e.name.empty()) {
        std::memcpy(rec + 9, e.name.data(), e.name.size());
      }
    } else {
      PutBE16(rec, 0, static_cast<uint16_t>(e.value.size()));
      rec[2] = static_cast<uint8_t>(e.name.size());
      if (!e.name.empty()) {
        std::memcpy(rec + 3, e.name.data(), e.name.size());
      }
      if (!e.value.empty()) {
        std::memcpy(rec + 3 + e.name.size(), e.value.data(), e.value.size());
      }
    }
    usedbytes += size;
    uint8_t* ent = block.data() + kLeafHeader + i * kAttrLeafEntrySize;
    PutBE32(ent, 0, e.hash);
    PutBE16(ent, 4, static_cast<uint16_t>(cursor));
    uint8_t leaf_flags = e.flags & kAttrNspOnDiskMask;
    if (e.incomplete) leaf_flags |= kAttrIncomplete;
    if (!e.remote) leaf_flags |= kAttrLocal;
    ent[6] = leaf_flags;
    ent[7] = 0;
  }
  PutBE16(block.data(), 56, static_cast<uint16_t>(entries.size()));
  PutBE16(block.data(), 58, static_cast<uint16_t>(usedbytes));
  const uint16_t firstused =
      (cursor == 65536) ? 0 : static_cast<uint16_t>(cursor);
  PutBE16(block.data(), 60, firstused);
  block[62] = 0;  // holes
  const uint32_t entries_end =
      static_cast<uint32_t>(kLeafHeader +
                            entries.size() * kAttrLeafEntrySize);
  // The name/value area occupies [firstused, blocksize); the only free region
  // in a freshly rebuilt leaf is between the entry table and that area.
  // xfs_attr3_leaf_create initialises freemap[0] this way, and Linux's
  // insertion path uses it as the first-fit free region.
  const uint32_t free_size =
      cursor >= entries_end ? cursor - entries_end : 0;
  PutBE16(block.data(), 64, static_cast<uint16_t>(entries_end));
  PutBE16(block.data(), 66, static_cast<uint16_t>(free_size));
  for (size_t i = 1; i < kAttrLeafMapSize; ++i) {
    PutBE16(block.data(), 64 + i * 4, 0);
    PutBE16(block.data(), 64 + i * 4 + 2, 0);
  }
  return block;
}

struct NodeEditEntry {
  uint32_t hash = 0;
  uint32_t child = 0;
};

std::vector<uint8_t> BuildNode(const XfsGeometry& geo, uint64_t ino,
                               uint32_t forw, uint32_t back, uint16_t level,
                               const std::vector<NodeEditEntry>& entries) {
  std::vector<uint8_t> block(geo.block_size, 0);
  PutBE32(block.data(), 0, forw);
  PutBE32(block.data(), 4, back);
  PutBE16(block.data(), kLeafMagicOff, kAttrDaNodeMagicV5);
  PutBE16(block.data(), 10, 0);
  std::memcpy(block.data() + 32, geo.meta_uuid.data(), 16);
  PutBE64(block.data(), 48, ino);
  PutBE16(block.data(), 56, static_cast<uint16_t>(entries.size()));
  PutBE16(block.data(), 58, level);
  for (size_t i = 0; i < entries.size(); ++i) {
    uint8_t* p = block.data() + kNodeHeader + i * kAttrNodeEntrySize;
    PutBE32(p, 0, entries[i].hash);
    PutBE32(p, 4, entries[i].child);
  }
  return block;
}

// ---------------------------------------------------------------------------
// Attribute-fork block/extent manager
// ---------------------------------------------------------------------------
class AttrBlockMap {
 public:
  AttrBlockMap(BlockAllocator* alloc, DataForkEditor* editor,
               const XfsGeometry* geo, uint64_t ino)
      : alloc_(alloc), editor_(editor), geo_(geo), ino_(ino) {}

  bool Map(uint32_t logical, uint64_t* fsb) const {
    for (const XfsExtent& e : editor_->extents()) {
      if (logical >= e.startoff && logical < e.EndBlock()) {
        *fsb = e.startblock + (logical - e.startoff);
        return true;
      }
    }
    return false;
  }

  Status FindHole(uint32_t len, uint32_t* out) const {
    uint64_t cursor = 0;
    for (const XfsExtent& e : editor_->extents()) {
      if (e.EndBlock() <= cursor) continue;
      if (e.startoff <= cursor) {
        cursor = std::max<uint64_t>(cursor, e.EndBlock());
        continue;
      }
      if (e.startoff >= cursor + len) {
        *out = static_cast<uint32_t>(cursor);
        return OkStatus();
      }
      cursor = std::max<uint64_t>(cursor, e.EndBlock());
    }
    if (cursor + len > UINT32_MAX) {
      return XFS_ERR(XfsError::kNoSpace,
                     "attribute fork logical address space exhausted")
          .Ino(ino_)
          .Metadata("attr fork");
    }
    *out = static_cast<uint32_t>(cursor);
    return OkStatus();
  }

  Status AllocateRun(uint32_t len, uint32_t* logical_out,
                     uint64_t* fsb_out) {
    uint32_t cursor = 0;
    Status s = FindHole(len, &cursor);
    if (!s.ok()) return s;
    for (uint32_t i = 0; i < len; ++i) {
      const uint32_t logical = cursor + i;
      Allocation alloc;
      s = alloc_->AllocateOwned(geo_->InoAgno(ino_), 1,
                                RmapOwner::Attr(ino_, logical), &alloc);
      if (!s.ok()) return s;
      XfsExtent extent;
      extent.startoff = logical;
      extent.startblock = alloc.fsb;
      extent.blockcount = 1;
      extent.unwritten = false;
      s = editor_->InsertExtent(extent);
      if (!s.ok()) return s;
      editor_->AddNblocks(1);
      if (i == 0) *logical_out = logical;
      if (i == 0) *fsb_out = alloc.fsb;
    }
    return OkStatus();
  }

  Status EnsureRoot(uint64_t* fsb_out) {
    if (Map(0, fsb_out)) return OkStatus();
    Allocation alloc;
    Status s = alloc_->AllocateOwned(geo_->InoAgno(ino_), 1,
                                     RmapOwner::Attr(ino_, 0), &alloc);
    if (!s.ok()) return s;
    XfsExtent extent;
    extent.startoff = 0;
    extent.startblock = alloc.fsb;
    extent.blockcount = 1;
    extent.unwritten = false;
    s = editor_->InsertExtent(extent);
    if (!s.ok()) return s;
    editor_->AddNblocks(1);
    *fsb_out = alloc.fsb;
    return OkStatus();
  }

  Status FreeRange(uint32_t start, uint32_t len) {
    if (len == 0) return OkStatus();
    return editor_->RemoveRange(start, static_cast<uint64_t>(start) + len);
  }

  // Frees DA leaf/node blocks. They are logged as one XFS_LI_BUF item per
  // filesystem block (kBlftAttrLeaf/kBlftDaNode); a cancel must use exactly
  // that (blkno,len). Remote value blocks are direct-written and must use the
  // plain FreeRange below: cancelling them would be a format error.
  Status FreeDaRange(uint32_t start, uint32_t len) {
    if (len == 0) return OkStatus();
    for (uint32_t i = 0; i < len; ++i) {
      uint64_t fsb = 0;
      if (!Map(start + i, &fsb)) {
        return XFS_ERR(XfsError::kInternal,
                       "attr DA block being freed is not mapped")
            .Ino(ino_)
            .Actual(start + i)
            .Metadata("attr fork");
      }
      Status s = alloc_->InvalidateMetadata(fsb, 1, "attr DA block");
      if (!s.ok()) return s;
    }
    return FreeRange(start, len);
  }

  Status FreeAll() {
    uint32_t end = 0;
    for (const XfsExtent& e : editor_->extents()) {
      end = std::max<uint32_t>(end, static_cast<uint32_t>(e.EndBlock()));
    }
    return FreeRange(0, end);
  }

  DataForkEditor* editor() { return editor_; }

 private:
  BlockAllocator* alloc_;
  DataForkEditor* editor_;
  const XfsGeometry* geo_;
  uint64_t ino_;
};

// ---------------------------------------------------------------------------
// Working DA tree (incremental node-format edits)
// ---------------------------------------------------------------------------
struct WorkingLeaf {
  uint32_t logical = 0;
  uint32_t forw = 0;
  uint32_t back = 0;
  std::vector<XattrEntry> entries;
  bool dirty = false;
};

struct WorkingNode {
  uint32_t logical = 0;
  uint32_t forw = 0;
  uint32_t back = 0;
  uint16_t level = 0;
  std::vector<NodeEditEntry> entries;
  bool dirty = false;
};

class TreeEditor {
 public:
  TreeEditor(const XfsGeometry& geo, const XfsInode& inode,
             AttrBlockMap* blocks)
      : geo_(geo), ino_(inode.ino), blocks_(blocks) {}

  Status Load(const XattrFork& fork) {
    if (fork.format != XattrForkFormat::kLeaf &&
        fork.format != XattrForkFormat::kNode) {
      return XFS_ERR(XfsError::kInternal, "tree editor needs a DA tree")
          .Ino(ino_)
          .Metadata("attr fork");
    }
    root_level_ = fork.root_level;
    leaves_.clear();
    nodes_.clear();
    for (const XattrDaLeaf& info : fork.leaves) {
      WorkingLeaf leaf;
      leaf.logical = info.logical;
      leaf.forw = info.forw;
      leaf.back = info.back;
      const size_t first = info.first_entry;
      const size_t count = info.entry_count;
      if (first + count > fork.entries.size()) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "attr leaf entry index is out of range")
            .Ino(ino_)
            .Metadata("attr fork");
      }
      leaf.entries.assign(fork.entries.begin() + static_cast<ptrdiff_t>(first),
                          fork.entries.begin() +
                              static_cast<ptrdiff_t>(first + count));
      const auto inserted = leaves_.emplace(leaf.logical, std::move(leaf));
      (void)inserted;
    }
    for (const XattrDaNode& info : fork.nodes) {
      WorkingNode node;
      node.logical = info.logical;
      node.forw = info.forw;
      node.back = info.back;
      node.level = info.level;
      node.entries.reserve(info.entries.size());
      for (const XattrDaNodeEntry& e : info.entries) {
        node.entries.push_back({e.hash, e.child});
      }
      nodes_.emplace(node.logical, std::move(node));
    }
    return OkStatus();
  }

  Status Insert(XattrEntry entry) {
    // A replace keeps the old entry's position and only changes its value.
    uint32_t target = 0;
    bool replacing = false;
    for (auto& [logical, leaf] : leaves_) {
      for (const XattrEntry& e : leaf.entries) {
        if (SameAttrKey(e, entry)) {
          target = logical;
          replacing = true;
          break;
        }
      }
      if (replacing) break;
    }
    if (!replacing) {
      Status s = FindTargetLeaf(entry.hash, &target);
      if (!s.ok()) return s;
    }
    WorkingLeaf& leaf = leaves_.at(target);
    if (replacing) {
      for (XattrEntry& e : leaf.entries) {
        if (SameAttrKey(e, entry)) {
          e = entry;
          break;
        }
      }
    } else {
      auto pos = std::lower_bound(
          leaf.entries.begin(), leaf.entries.end(), entry.hash,
          [](const XattrEntry& a, uint32_t hash) { return a.hash < hash; });
      leaf.entries.insert(pos, entry);
    }
    leaf.dirty = true;
    if (LeafFits(leaf.entries, geo_.block_size)) {
      return UpdateKeys(target);
    }
    return SplitLeaf(target);
  }

  Status Remove(const XattrEntry& entry) {
    uint32_t target = 0;
    bool found = false;
    for (auto& [logical, leaf] : leaves_) {
      for (auto it = leaf.entries.begin(); it != leaf.entries.end(); ++it) {
        if (SameAttrKey(*it, entry)) {
          leaf.entries.erase(it);
          leaf.dirty = true;
          target = logical;
          found = true;
          break;
        }
      }
      if (found) break;
    }
    if (!found) {
      return XFS_ERR(XfsError::kNotFound, "xattr not found")
          .Ino(ino_)
          .Metadata("attr fork");
    }
    if (root_level_ == 0) return UpdateKeys(target);
    if (leaves_.at(target).entries.empty()) {
      return RemoveEmptyLeaf(target);
    }
    Status s = UpdateKeys(target);
    if (!s.ok()) return s;
    if (LeafBytes(leaves_.at(target).entries) <=
        geo_.block_size / 2) {
      s = RebalanceLeaf(target);
      if (!s.ok()) return s;
    }
    return CollapseRoot();
  }

  bool IsSingleLeaf() const { return root_level_ == 0 && leaves_.size() == 1; }
  const std::vector<XattrEntry>* RootLeafEntries() const {
    auto it = leaves_.find(0);
    if (it == leaves_.end()) return nullptr;
    return &it->second.entries;
  }

  Status Publish(XfsTransaction* txn) {
    for (auto& [logical, leaf] : leaves_) {
      if (!leaf.dirty) continue;
      uint64_t fsb = 0;
      if (!blocks_->Map(logical, &fsb)) {
        return XFS_ERR(XfsError::kCorruptInode, "attr leaf is not mapped")
            .Ino(ino_)
            .Metadata("attr leaf")
            .Actual(logical);
      }
      std::vector<uint8_t> block = BuildLeaf(
          geo_, ino_, leaf.forw, leaf.back, leaf.entries);
      PutBE64(block.data(), 16, geo_.FsbToByte(fsb) / 512);
      Status s = txn->AddMetadataBuffer(
          geo_.FsbToByte(fsb), std::move(block), lf::kBlftAttrLeaf,
          kLeafLsnOff, kLeafCrcOff, "attr leaf");
      if (!s.ok()) return s;
      leaf.dirty = false;
    }
    for (auto& [logical, node] : nodes_) {
      if (!node.dirty) continue;
      uint64_t fsb = 0;
      if (!blocks_->Map(logical, &fsb)) {
        return XFS_ERR(XfsError::kCorruptInode, "attr node is not mapped")
            .Ino(ino_)
            .Metadata("attr node")
            .Actual(logical);
      }
      std::vector<uint8_t> block =
          BuildNode(geo_, ino_, node.forw, node.back, node.level, node.entries);
      PutBE64(block.data(), 16, geo_.FsbToByte(fsb) / 512);
      Status s = txn->AddMetadataBuffer(
          geo_.FsbToByte(fsb), std::move(block), lf::kBlftDaNode, kLeafLsnOff,
          kLeafCrcOff, "attr node");
      if (!s.ok()) return s;
      node.dirty = false;
    }
    return OkStatus();
  }

 private:
  Status FindTargetLeaf(uint32_t hash, uint32_t* out) {
    if (root_level_ == 0) {
      *out = 0;
      return OkStatus();
    }
    uint32_t node = 0;
    for (int depth = 0; depth <= kAttrMaxTreeDepth; ++depth) {
      auto it = nodes_.find(node);
      if (it == nodes_.end()) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "attr DA node is missing while descending")
            .Ino(ino_)
            .Metadata("attr node")
            .Actual(node);
      }
      const auto& entries = it->second.entries;
      size_t index = 0;
      while (index < entries.size() && entries[index].hash < hash) ++index;
      if (index == entries.size()) index = entries.size() - 1;
      const uint32_t child = entries[index].child;
      if (leaves_.count(child)) {
        *out = child;
        return OkStatus();
      }
      node = child;
    }
    return XFS_ERR(XfsError::kCorruptBtree, "attr DA tree is too deep")
        .Ino(ino_)
        .Metadata("attr node");
  }

  bool ParentOf(uint32_t child, uint32_t* parent, size_t* index) {
    for (auto& [logical, node] : nodes_) {
      for (size_t i = 0; i < node.entries.size(); ++i) {
        if (node.entries[i].child == child) {
          *parent = logical;
          *index = i;
          return true;
        }
      }
    }
    return false;
  }

  uint32_t SubtreeLastHash(uint32_t logical) const {
    auto leaf = leaves_.find(logical);
    if (leaf != leaves_.end()) return LastHash(leaf->second.entries);
    auto node = nodes_.find(logical);
    if (node != nodes_.end() && !node->second.entries.empty()) {
      return node->second.entries.back().hash;
    }
    return 0;
  }

  Status UpdateKeys(uint32_t child) {
    uint32_t current = child;
    for (int depth = 0; depth <= kAttrMaxTreeDepth; ++depth) {
      uint32_t parent = 0;
      size_t index = 0;
      if (!ParentOf(current, &parent, &index)) return OkStatus();
      WorkingNode& node = nodes_.at(parent);
      node.entries[index].hash = SubtreeLastHash(current);
      node.dirty = true;
      current = parent;
    }
    return XFS_ERR(XfsError::kCorruptBtree, "attr DA parent chain is too deep")
        .Ino(ino_)
        .Metadata("attr node");
  }

  Status SplitLeaf(uint32_t logical) {
    if (root_level_ == 0) {
      return XFS_ERR(XfsError::kInternal,
                     "a single-leaf split must use the tree builder")
          .Ino(ino_)
          .Metadata("attr leaf");
    }
    uint32_t parent = 0;
    size_t parent_index = 0;
    if (!ParentOf(logical, &parent, &parent_index)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "attr leaf has no parent node")
          .Ino(ino_)
          .Metadata("attr leaf");
    }
    WorkingLeaf& leaf = leaves_.at(logical);
    const std::vector<XattrEntry> all = leaf.entries;
    size_t best = 0;
    size_t best_imbalance = SIZE_MAX;
    bool found = false;
    for (size_t i = 1; i < all.size(); ++i) {
      std::vector<XattrEntry> lower(all.begin(), all.begin() + i);
      std::vector<XattrEntry> upper(all.begin() + i, all.end());
      if (!LeafFits(lower, geo_.block_size) ||
          !LeafFits(upper, geo_.block_size)) {
        continue;
      }
      const size_t lb = LeafBytes(lower);
      const size_t ub = LeafBytes(upper);
      const size_t imbalance =
          lb > ub ? lb - ub : ub - lb;
      if (!found || imbalance < best_imbalance) {
        found = true;
        best = i;
        best_imbalance = imbalance;
      }
    }
    if (!found) {
      return XFS_ERR(XfsError::kNoSpace,
                     "attr leaf cannot be split into two valid leaves")
          .Ino(ino_)
          .Metadata("attr leaf");
    }
    WorkingLeaf& old = leaves_.at(logical);
    std::vector<XattrEntry> lower(all.begin(), all.begin() + best);
    std::vector<XattrEntry> upper(all.begin() + best, all.end());

    uint32_t new_logical = 0;
    uint64_t new_fsb = 0;
    Status s = blocks_->AllocateRun(1, &new_logical, &new_fsb);
    if (!s.ok()) return s;
    (void)new_fsb;
    WorkingLeaf new_leaf;
    new_leaf.logical = new_logical;
    new_leaf.forw = old.forw;
    new_leaf.back = old.logical;
    new_leaf.entries = std::move(upper);
    new_leaf.dirty = true;
    const uint32_t old_forw = old.forw;
    old.entries = std::move(lower);
    old.forw = new_logical;
    old.dirty = true;
    if (old_forw != 0) {
      auto next = leaves_.find(old_forw);
      if (next == leaves_.end()) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "attr leaf forward link is invalid")
            .Ino(ino_)
            .Metadata("attr leaf")
            .Actual(old_forw);
      }
      next->second.back = new_logical;
      next->second.dirty = true;
    }
    const uint32_t upper_hash = LastHash(new_leaf.entries);
    leaves_.emplace(new_logical, std::move(new_leaf));

    WorkingNode& pnode = nodes_.at(parent);
    NodeEditEntry inserted{upper_hash, new_logical};
    pnode.entries.insert(pnode.entries.begin() +
                             static_cast<ptrdiff_t>(parent_index + 1),
                         inserted);
    pnode.dirty = true;
    s = UpdateKeys(old.logical);
    if (!s.ok()) return s;
    s = UpdateKeys(new_logical);
    if (!s.ok()) return s;
    if (pnode.entries.size() > NodeMaxRecs(geo_.block_size)) {
      return SplitNode(parent);
    }
    return OkStatus();
  }

  Status SplitNode(uint32_t logical) {
    WorkingNode& node = nodes_.at(logical);
    const std::vector<NodeEditEntry> all = node.entries;
    // The root split below erases this map node; keep every scalar we need
    // afterwards in a local instead of reading through the dangling
    // reference.
    const uint16_t node_level = node.level;
    const size_t max = NodeMaxRecs(geo_.block_size);
    if (all.size() <= max) return OkStatus();
    uint32_t parent = 0;
    size_t parent_index = 0;
    const bool is_root = !ParentOf(logical, &parent, &parent_index);
    if (is_root && logical != 0) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "non-root attr node has no parent")
          .Ino(ino_)
          .Metadata("attr node")
          .Actual(logical);
    }
    const size_t split = all.size() / 2;
    std::vector<NodeEditEntry> lower(all.begin(), all.begin() + split);
    std::vector<NodeEditEntry> upper(all.begin() + split, all.end());
    if (lower.empty() || upper.empty()) {
      return XFS_ERR(XfsError::kInternal, "attr node split produced an empty half")
          .Ino(ino_)
          .Metadata("attr node");
    }

    if (is_root) {
      uint32_t lower_logical = 0, upper_logical = 0;
      uint64_t dummy = 0;
      Status s = blocks_->AllocateRun(1, &lower_logical, &dummy);
      if (!s.ok()) return s;
      s = blocks_->AllocateRun(1, &upper_logical, &dummy);
      if (!s.ok()) return s;
      WorkingNode lower_node;
      lower_node.logical = lower_logical;
      lower_node.level = node_level;
      lower_node.entries = std::move(lower);
      lower_node.forw = upper_logical;
      lower_node.back = 0;
      lower_node.dirty = true;
      WorkingNode upper_node;
      upper_node.logical = upper_logical;
      upper_node.level = node_level;
      upper_node.entries = std::move(upper);
      upper_node.forw = 0;
      upper_node.back = lower_logical;
      upper_node.dirty = true;
      const uint32_t lower_hash = lower_node.entries.back().hash;
      const uint32_t upper_hash = upper_node.entries.back().hash;
      nodes_.erase(0);
      nodes_.emplace(lower_logical, std::move(lower_node));
      nodes_.emplace(upper_logical, std::move(upper_node));
      WorkingNode root;
      root.logical = 0;
      root.level = static_cast<uint16_t>(node_level + 1);
      root.entries = {{lower_hash, lower_logical},
                      {upper_hash, upper_logical}};
      root.dirty = true;
      nodes_.emplace(0, std::move(root));
      root_level_ = static_cast<uint16_t>(root_level_ + 1);
      return OkStatus();
    }

    uint32_t new_logical = 0;
    uint64_t dummy = 0;
    Status s = blocks_->AllocateRun(1, &new_logical, &dummy);
    if (!s.ok()) return s;
    WorkingNode& old = nodes_.at(logical);
    WorkingNode upper_node;
    upper_node.logical = new_logical;
    upper_node.level = old.level;
    upper_node.entries = std::move(upper);
    upper_node.forw = old.forw;
    upper_node.back = old.logical;
    upper_node.dirty = true;
    const uint32_t old_forw = old.forw;
    old.entries = std::move(lower);
    old.forw = new_logical;
    old.dirty = true;
    if (old_forw != 0) {
      auto next = nodes_.find(old_forw);
      if (next == nodes_.end()) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "attr node forward link is invalid")
            .Ino(ino_)
            .Metadata("attr node")
            .Actual(old_forw);
      }
      next->second.back = new_logical;
      next->second.dirty = true;
    }
    const uint32_t upper_hash = upper_node.entries.back().hash;
    nodes_.emplace(new_logical, std::move(upper_node));

    WorkingNode& pnode = nodes_.at(parent);
    pnode.entries.insert(pnode.entries.begin() +
                             static_cast<ptrdiff_t>(parent_index + 1),
                         {upper_hash, new_logical});
    pnode.dirty = true;
    s = UpdateKeys(old.logical);
    if (!s.ok()) return s;
    s = UpdateKeys(new_logical);
    if (!s.ok()) return s;
    if (pnode.entries.size() > NodeMaxRecs(geo_.block_size)) {
      return SplitNode(parent);
    }
    return OkStatus();
  }

  Status RemoveEmptyLeaf(uint32_t logical) {
    uint32_t parent = 0;
    size_t index = 0;
    if (!ParentOf(logical, &parent, &index)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "attr leaf has no parent node")
          .Ino(ino_)
          .Metadata("attr leaf");
    }
    WorkingLeaf leaf = leaves_.at(logical);
    if (leaf.back != 0) {
      auto prev = leaves_.find(leaf.back);
      if (prev == leaves_.end()) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "attr leaf back link is invalid")
            .Ino(ino_)
            .Metadata("attr leaf");
      }
      prev->second.forw = leaf.forw;
      prev->second.dirty = true;
    }
    if (leaf.forw != 0) {
      auto next = leaves_.find(leaf.forw);
      if (next == leaves_.end()) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "attr leaf forward link is invalid")
            .Ino(ino_)
            .Metadata("attr leaf");
      }
      next->second.back = leaf.back;
      next->second.dirty = true;
    }
    leaves_.erase(logical);
    WorkingNode& pnode = nodes_.at(parent);
    pnode.entries.erase(pnode.entries.begin() + static_cast<ptrdiff_t>(index));
    pnode.dirty = true;
    Status s = blocks_->FreeDaRange(logical, 1);
    if (!s.ok()) return s;
    s = UpdateKeys(parent);
    if (!s.ok()) return s;
    if (parent == 0) {
      if (pnode.entries.empty()) {
        // The tree has no leaves: the caller will remove/empty the fork.
        nodes_.erase(0);
        root_level_ = 0;
        return OkStatus();
      }
      return CollapseRoot();
    }
    if (pnode.entries.size() <= NodeMaxRecs(geo_.block_size) / 2) {
      s = RebalanceNode(parent);
      if (!s.ok()) return s;
    }
    return CollapseRoot();
  }

  Status RebalanceLeaf(uint32_t logical) {
    if (root_level_ == 0) return OkStatus();
    WorkingLeaf& leaf = leaves_.at(logical);
    uint32_t candidates[2] = {leaf.back, leaf.forw};
    // Prefer the lower-numbered sibling, as xfs_attr3_leaf_toosmall does.
    std::sort(std::begin(candidates), std::end(candidates));
    for (uint32_t sibling : candidates) {
      if (sibling == 0) continue;
      auto sit = leaves_.find(sibling);
      if (sit == leaves_.end()) continue;
      const bool sibling_is_prev = (sibling == leaf.back);
      std::vector<XattrEntry> combined;
      WorkingLeaf copy = leaf;
      if (sibling_is_prev) {
        combined = sit->second.entries;
        combined.insert(combined.end(), copy.entries.begin(),
                        copy.entries.end());
      } else {
        combined = copy.entries;
        combined.insert(combined.end(), sit->second.entries.begin(),
                        sit->second.entries.end());
      }
      const size_t combined_bytes = LeafBytes(combined);
      if (combined_bytes > geo_.block_size - geo_.block_size / 4) {
        continue;  // cannot join with 25% free space
      }
      const uint32_t keep =
          std::min(copy.logical, sit->second.logical);
      const uint32_t drop =
          std::max(copy.logical, sit->second.logical);
      WorkingLeaf& keep_leaf = leaves_.at(keep);
      WorkingLeaf& drop_leaf = leaves_.at(drop);
      const uint32_t drop_back = drop_leaf.back;
      const uint32_t drop_forw = drop_leaf.forw;
      keep_leaf.entries = std::move(combined);
      keep_leaf.dirty = true;
      // Fix the sibling chain around the dropped block.  Exactly one of the
      // two adjacency cases holds because the candidate is a sibling.
      if (drop_back == keep) {
        keep_leaf.forw = drop_leaf.forw;
        if (drop_forw != 0) {
          auto n = leaves_.find(drop_forw);
          if (n != leaves_.end()) {
            n->second.back = keep;
            n->second.dirty = true;
          }
        }
      } else if (drop_forw == keep) {
        keep_leaf.back = drop_leaf.back;
        if (drop_back != 0) {
          auto p = leaves_.find(drop_back);
          if (p != leaves_.end()) {
            p->second.forw = keep;
            p->second.dirty = true;
          }
        }
      } else {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "merged attr leaves are not adjacent")
            .Ino(ino_)
            .Metadata("attr leaf");
      }
      // Remove the dropped child from its parent.
      uint32_t parent = 0;
      size_t index = 0;
      if (!ParentOf(drop, &parent, &index)) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "attr leaf has no parent node")
            .Ino(ino_)
            .Metadata("attr leaf");
      }
      WorkingNode& pnode = nodes_.at(parent);
      pnode.entries.erase(pnode.entries.begin() + static_cast<ptrdiff_t>(index));
      pnode.dirty = true;
      leaves_.erase(drop);
      Status s = blocks_->FreeDaRange(drop, 1);
      if (!s.ok()) return s;
      s = UpdateKeys(keep);
      if (!s.ok()) return s;
      if (parent == 0) {
        return CollapseRoot();
      }
      if (pnode.entries.size() <= NodeMaxRecs(geo_.block_size) / 2) {
        s = RebalanceNode(parent);
        if (!s.ok()) return s;
      }
      return CollapseRoot();
    }

    // No merge possible: rebalance the two leaves without freeing either.
    const uint32_t sibling =
        candidates[0] != 0 ? candidates[0] : candidates[1];
    if (sibling == 0) return OkStatus();
    auto sit = leaves_.find(sibling);
    if (sit == leaves_.end()) return OkStatus();
    WorkingLeaf& other = sit->second;
    std::vector<XattrEntry> combined;
    const bool other_is_prev = (sibling == leaf.back);
    if (other_is_prev) {
      combined = other.entries;
      combined.insert(combined.end(), leaf.entries.begin(), leaf.entries.end());
    } else {
      combined = leaf.entries;
      combined.insert(combined.end(), other.entries.begin(), other.entries.end());
    }
    size_t best = 0;
    size_t best_imbalance = SIZE_MAX;
    for (size_t i = 1; i < combined.size(); ++i) {
      std::vector<XattrEntry> lower(combined.begin(), combined.begin() + i);
      std::vector<XattrEntry> upper(combined.begin() + i, combined.end());
      if (!LeafFits(lower, geo_.block_size) ||
          !LeafFits(upper, geo_.block_size)) {
        continue;
      }
      const size_t lb = LeafBytes(lower);
      const size_t ub = LeafBytes(upper);
      const size_t imbalance = lb > ub ? lb - ub : ub - lb;
      if (imbalance < best_imbalance) {
        best_imbalance = imbalance;
        best = i;
      }
    }
    if (best == 0) return OkStatus();
    std::vector<XattrEntry> lower(combined.begin(), combined.begin() + best);
    std::vector<XattrEntry> upper(combined.begin() + best, combined.end());
    if (other_is_prev) {
      other.entries = std::move(lower);
      leaf.entries = std::move(upper);
    } else {
      leaf.entries = std::move(lower);
      other.entries = std::move(upper);
    }
    other.dirty = true;
    leaf.dirty = true;
    Status s = UpdateKeys(leaf.logical);
    if (!s.ok()) return s;
    return UpdateKeys(other.logical);
  }

  Status RebalanceNode(uint32_t logical) {
    if (root_level_ == 0 || logical == 0) return OkStatus();
    WorkingNode& node = nodes_.at(logical);
    const size_t max = NodeMaxRecs(geo_.block_size);
    if (node.entries.size() > max / 2) return OkStatus();
    uint32_t parent = 0;
    size_t parent_index = 0;
    if (!ParentOf(logical, &parent, &parent_index)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "attr node has no parent node")
          .Ino(ino_)
          .Metadata("attr node");
    }
    uint32_t candidates[2] = {node.back, node.forw};
    std::sort(std::begin(candidates), std::end(candidates));
    for (uint32_t sibling : candidates) {
      if (sibling == 0) continue;
      auto sit = nodes_.find(sibling);
      if (sit == nodes_.end()) continue;
      if (sit->second.level != node.level) continue;
      const bool sibling_is_prev = (sibling == node.back);
      std::vector<NodeEditEntry> combined;
      if (sibling_is_prev) {
        combined = sit->second.entries;
        combined.insert(combined.end(), node.entries.begin(),
                        node.entries.end());
      } else {
        combined = node.entries;
        combined.insert(combined.end(), sit->second.entries.begin(),
                        sit->second.entries.end());
      }
      if (combined.size() > max - max / 4) continue;
      const uint32_t keep = std::min(logical, sibling);
      const uint32_t drop = std::max(logical, sibling);
      WorkingNode& keep_node = nodes_.at(keep);
      WorkingNode& drop_node = nodes_.at(drop);
      const uint32_t drop_back = drop_node.back;
      const uint32_t drop_forw = drop_node.forw;
      keep_node.entries = std::move(combined);
      keep_node.dirty = true;
      if (drop_back == keep) {
        keep_node.forw = drop_node.forw;
        if (drop_forw != 0) {
          auto n = nodes_.find(drop_forw);
          if (n != nodes_.end()) {
            n->second.back = keep;
            n->second.dirty = true;
          }
        }
      } else if (drop_forw == keep) {
        keep_node.back = drop_node.back;
        if (drop_back != 0) {
          auto p = nodes_.find(drop_back);
          if (p != nodes_.end()) {
            p->second.forw = keep;
            p->second.dirty = true;
          }
        }
      } else {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "merged attr nodes are not adjacent")
            .Ino(ino_)
            .Metadata("attr node");
      }
      uint32_t drop_parent = 0;
      size_t drop_index = 0;
      if (!ParentOf(drop, &drop_parent, &drop_index)) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "attr node has no parent node")
            .Ino(ino_)
            .Metadata("attr node");
      }
      WorkingNode& dp = nodes_.at(drop_parent);
      dp.entries.erase(dp.entries.begin() + static_cast<ptrdiff_t>(drop_index));
      dp.dirty = true;
      nodes_.erase(drop);
      Status s = blocks_->FreeDaRange(drop, 1);
      if (!s.ok()) return s;
      s = UpdateKeys(keep);
      if (!s.ok()) return s;
      if (drop_parent == 0) return CollapseRoot();
      if (dp.entries.size() <= max / 2) {
        s = RebalanceNode(drop_parent);
        if (!s.ok()) return s;
      }
      return CollapseRoot();
    }

    // Rebalance without merging.
    const uint32_t sibling =
        candidates[0] != 0 ? candidates[0] : candidates[1];
    if (sibling == 0) return OkStatus();
    auto sit = nodes_.find(sibling);
    if (sit == nodes_.end()) return OkStatus();
    WorkingNode& other = sit->second;
    std::vector<NodeEditEntry> combined;
    const bool other_is_prev = (sibling == node.back);
    if (other_is_prev) {
      combined = other.entries;
      combined.insert(combined.end(), node.entries.begin(), node.entries.end());
    } else {
      combined = node.entries;
      combined.insert(combined.end(), other.entries.begin(), other.entries.end());
    }
    const size_t split = combined.size() / 2;
    if (split == 0 || split >= combined.size()) return OkStatus();
    std::vector<NodeEditEntry> lower(combined.begin(), combined.begin() + split);
    std::vector<NodeEditEntry> upper(combined.begin() + split, combined.end());
    if (other_is_prev) {
      other.entries = std::move(lower);
      node.entries = std::move(upper);
    } else {
      node.entries = std::move(lower);
      other.entries = std::move(upper);
    }
    other.dirty = true;
    node.dirty = true;
    Status s = UpdateKeys(node.logical);
    if (!s.ok()) return s;
    return UpdateKeys(other.logical);
  }

  Status CollapseRoot() {
    while (root_level_ > 0) {
      auto root_it = nodes_.find(0);
      if (root_it == nodes_.end() || root_it->second.entries.size() != 1) {
        return OkStatus();
      }
      const uint32_t child = root_it->second.entries.front().child;
      auto leaf_it = leaves_.find(child);
      if (leaf_it != leaves_.end()) {
        WorkingLeaf moved = leaf_it->second;
        if (moved.back != 0 || moved.forw != 0) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "single root leaf has siblings")
              .Ino(ino_)
              .Metadata("attr leaf");
        }
        moved.logical = 0;
        moved.dirty = true;
        leaves_.erase(child);
        nodes_.erase(0);
        leaves_[0] = std::move(moved);
        root_level_ = 0;
        return blocks_->FreeDaRange(child, 1);
      }
      auto node_it = nodes_.find(child);
      if (node_it == nodes_.end()) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "attr root child is missing")
            .Ino(ino_)
            .Metadata("attr node");
      }
      WorkingNode moved = node_it->second;
      nodes_.erase(child);
      const uint32_t moved_forw = moved.forw;
      const uint32_t moved_back = moved.back;
      moved.logical = 0;
      moved.dirty = true;
      moved.forw = 0;
      moved.back = 0;
      nodes_.erase(0);
      nodes_[0] = std::move(moved);
      root_level_ = nodes_[0].level;
      if (moved_forw != 0) {
        auto n = nodes_.find(moved_forw);
        if (n != nodes_.end()) n->second.back = 0;
      }
      if (moved_back != 0) {
        auto p = nodes_.find(moved_back);
        if (p != nodes_.end()) p->second.forw = 0;
      }
      Status s = blocks_->FreeDaRange(child, 1);
      if (!s.ok()) return s;
    }
    return OkStatus();
  }

  const XfsGeometry& geo_;
  uint64_t ino_;
  AttrBlockMap* blocks_;
  uint16_t root_level_ = 0;
  std::map<uint32_t, WorkingLeaf> leaves_;
  std::map<uint32_t, WorkingNode> nodes_;
};

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
class XattrWriter {
 public:
  XattrWriter(XfsTransaction* txn, BlockAllocator* alloc, BlockDevice* device,
              const XfsGeometry* geo, const XfsInode& inode,
              const XattrFork& fork, std::vector<uint8_t> raw,
              bool parent_pointers)
      : txn_(txn),
        alloc_(alloc),
        device_(device),
        geo_(geo),
        inode_(inode),
        fork_(fork),
        raw_(std::move(raw)),
        parent_pointers_(parent_pointers) {}

  ~XattrWriter() = default;

  // `changed_index` is the index in `entries` of a new or replaced attribute
  // whose remote value still has to be allocated; `removed_remote` is the old
  // remote range of a replaced/removed attribute.  `removed_entry` is the
  // exact old entry for a removal (parent pointers need the whole record, not
  // just namespace+name).  `entries` is the complete desired attribute set.
  Status Prepare(std::vector<XattrEntry> entries, int changed_index,
                 std::optional<std::pair<uint32_t, uint32_t>> removed_remote,
                 std::optional<XattrEntry> removed_entry,
                 bool preserve_shortform_empty, PreparedInode* out) {
    entries_ = std::move(entries);
    changed_index_ = changed_index;
    removed_remote_ = removed_remote;
    removed_entry_ = std::move(removed_entry);
    preserve_shortform_empty_ = preserve_shortform_empty;
    for (XattrEntry& entry : entries_) {
      Status normalized = NormalizeEntryHash(&entry, inode_.ino);
      if (!normalized.ok()) return normalized;
    }
    Status s = InitEditor();
    if (!s.ok()) return s;
    s = DecideTarget();
    if (!s.ok()) return s;
    if (target_ == Target::kNone || target_ == Target::kShortform) {
      s = BuildShortformTarget();
      if (!s.ok()) return s;
    } else if (target_ == Target::kSingleLeaf) {
      s = BuildSingleLeaf();
      if (!s.ok()) return s;
    } else if (target_ == Target::kTree) {
      s = BuildFullTree();
      if (!s.ok()) return s;
    } else if (target_ == Target::kIncrementalTree) {
      s = BuildIncrementalTree();
      if (!s.ok()) return s;
    }
    if (!s.ok()) return s;
    if (remote_written_) {
      MaybeCrash(CrashPoint::kBeforeDeviceFlush,
                 "xattr remote ino=" + std::to_string(inode_.ino));
      if (!device_->Flush()) {
        return XFS_ERR(XfsError::kIoError,
                       "failed to flush remote attr value: " +
                           device_->LastError())
            .Ino(inode_.ino)
            .Metadata("attr remote");
      }
      MaybeCrash(CrashPoint::kAfterDeviceFlush,
                 "xattr remote ino=" + std::to_string(inode_.ino));
    }
    return Finish(out);
  }

 private:
  enum class Target {
    kNone,
    kShortform,
    kSingleLeaf,
    kTree,
    kIncrementalTree,
  };

  Status InitEditor() {
    XfsInode synth;
    synth.ino = inode_.ino;
    synth.format = di::kFormatExtents;
    synth.nextents = 0;
    if (inode_.aformat == di::kFormatExtents ||
        inode_.aformat == di::kFormatBtree) {
      synth.format = inode_.aformat;
      synth.nextents = inode_.anextents;
      synth.data_fork = inode_.attr_fork;
    } else {
      synth.data_fork.assign(inode_.attr_fork.size(), 0);
    }
    editor_ = std::make_unique<DataForkEditor>(txn_, alloc_, device_, geo_, true);
    editor_->SetFreeRemovedBlocks(true);
    Status s = editor_->Load(synth);
    if (!s.ok()) return s;
    blocks_ = std::make_unique<AttrBlockMap>(alloc_, editor_.get(), geo_,
                                             inode_.ino);
    return OkStatus();
  }

  uint32_t ChooseLeafForkOff() const {
    // xfs_bmap_set_attrforkoff() falls back to xfs_default_attroffset() when
    // the attr fork is created for a non-shortform attribute.  bytesfit(4)
    // is not the right value here: it optimises for an almost empty attr
    // fork and would leave no room for the data fork's extents/root.
    if (inode_.forkoff != 0) return inode_.forkoff;
    uint32_t raw = DefaultAttrForkOffRaw(geo_->inode_size);
    if (inode_.format == di::kFormatLocal) {
      // Preserve an inline directory/symlink if the literal area can hold both
      // forks.  Linux converts the data fork when it cannot; RW-6D refuses
      // cleanly rather than truncating local data.
      raw = std::max<uint32_t>(raw,
                               static_cast<uint32_t>((inode_.size + 7) >> 3));
    } else if (inode_.format == di::kFormatDev) {
      raw = std::max<uint32_t>(raw, 1);
    }
    // At least the 36-byte bmdr root must remain available to the attr fork.
    const uint32_t max_raw = (Litino(*geo_) - 36) >> 3;
    if (raw > max_raw) return 0;
    return raw;
  }

  Status DecideTarget() {
    // Empty attribute set: Linux removes the fork unless the data fork is a
    // btree; in that case an empty shortform fork is retained so the data
    // branch root layout is not disturbed.
    if (entries_.empty()) {
      // xfs_attr_shortform_remove keeps an empty shortform fork when the data
      // fork is a btree, and unconditionally on parent-pointer filesystems
      // (`!xfs_has_parent(mp)` is part of the fork-removal condition).  The
      // empty fork keeps the data-fork layout stable until the inode is
      // reused or freed.
      if (inode_.format == di::kFormatBtree || parent_pointers_) {
        target_ = Target::kShortform;
        preserve_shortform_empty_ = true;
        uint32_t raw = inode_.forkoff;
        if (raw == 0) {
          raw = DefaultAttrForkOffRaw(geo_->inode_size);
        }
        target_forkoff_ = raw;
      } else {
        target_ = Target::kNone;
        target_forkoff_ = 0;
      }
      return OkStatus();
    }

    uint32_t sf_raw = 0;
    if (CanUseShortform(entries_, inode_, *geo_, &sf_raw)) {
      target_ = Target::kShortform;
      target_forkoff_ = sf_raw;
      return OkStatus();
    }

    const bool single_leaf = LeafFits(SortByHash(entries_), geo_->block_size);
    if (fork_.format == XattrForkFormat::kNode && !single_leaf) {
      target_ = Target::kIncrementalTree;
      target_forkoff_ = inode_.forkoff;
      return OkStatus();
    }
    if (fork_.format == XattrForkFormat::kLeaf ||
        fork_.format == XattrForkFormat::kNode) {
      target_forkoff_ = inode_.forkoff;
    } else {
      // shortform -> extents: xfs_bmap_forkoff_reset() may shrink the attr
      // fork to the default offset, never grow it.
      target_forkoff_ = std::max<uint32_t>(
          ChooseLeafForkOff(), DefaultAttrForkOffRaw(geo_->inode_size));
    }
    if (target_forkoff_ == 0) {
      return XFS_ERR(XfsError::kNoSpace,
                     "no literal area remains for an attribute fork")
          .Ino(inode_.ino)
          .Metadata("attr fork");
    }
    target_ = single_leaf ? Target::kSingleLeaf : Target::kTree;
    return OkStatus();
  }

  Status DropAllAttrBlocks() {
    Status s = FreeOldExcept({});
    if (!s.ok()) return s;
    // Removing every extent does not by itself release attr BMBT blocks; the
    // bmap editor's collapse path does that in the same transaction.
    std::vector<uint8_t> temp = raw_;
    uint32_t fields = 0;
    std::vector<uint8_t> region;
    s = editor_->Finalize(&temp, &fields, &region);
    if (!s.ok()) return s;
    return OkStatus();
  }

  Status BuildShortformTarget() {
    if (target_ == Target::kNone) {
      // Free every attr block (there should be no DA tree left).
      Status s = DropAllAttrBlocks();
      if (!s.ok()) return s;
      target_forkoff_ = 0;
      target_aformat_ = 0;
      target_anextents_ = 0;
      attr_image_.clear();
      attr_fields_ = 0;
      attr_region_.clear();
      return OkStatus();
    }
    Status s = DropAllAttrBlocks();
    if (!s.ok()) return s;
    target_aformat_ = di::kFormatLocal;
    target_anextents_ = 0;
    attr_image_ = BuildShortform(entries_);
    attr_fields_ = lf::kILogAData;
    attr_region_ = attr_image_;
    return OkStatus();
  }

  Status AllocateChangedRemote() {
    if (changed_index_ < 0 ||
        static_cast<size_t>(changed_index_) >= entries_.size()) {
      return OkStatus();
    }
    XattrEntry& e = entries_[changed_index_];
    if (!e.remote || e.remote_lbno != 0 || e.IsIncomplete()) return OkStatus();
    if (e.value.size() > kXattrMaxValueBytes) {
      return XFS_ERR(XfsError::kOutOfRange,
                     "xattr value exceeds the XFS limit")
          .Ino(inode_.ino)
          .Metadata("xattr")
          .Actual(e.value.size());
    }
    const uint32_t space = geo_->block_size - kAttrRemoteHeaderSize;
    const uint32_t blocks = e.value.empty()
                                ? 0
                                : static_cast<uint32_t>(
                                      (e.value.size() + space - 1) / space);
    if (blocks == 0) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "empty remote xattr value is not supported")
          .Ino(inode_.ino)
          .Metadata("xattr");
    }
    MaybeCrash(CrashPoint::kBeforeDataWrite,
               "xattr remote ino=" + std::to_string(inode_.ino));
    uint32_t logical = 0;
    uint64_t fsb = 0;
    Status s = blocks_->AllocateRun(blocks, &logical, &fsb);
    if (!s.ok()) return s;
    (void)fsb;
    e.remote_lbno = logical;
    e.remote_blocks = blocks;
    e.remote_value_len = static_cast<uint32_t>(e.value.size());
    e.remote = true;

    uint32_t offset = 0;
    uint32_t remaining = static_cast<uint32_t>(e.value.size());
    for (uint32_t i = 0; i < blocks; ++i) {
      uint64_t block_fsb = 0;
      if (!blocks_->Map(logical + i, &block_fsb)) {
        return XFS_ERR(XfsError::kInternal,
                       "freshly allocated remote attr block is not mapped")
            .Ino(inode_.ino)
            .Metadata("attr remote");
      }
      std::vector<uint8_t> block(geo_->block_size, 0);
      const uint32_t bytes = std::min(remaining, space);
      PutBE32(block.data(), 0, ondisk::kAttr3RmtMagic);
      PutBE32(block.data(), 4, offset);
      PutBE32(block.data(), 8, bytes);
      std::memcpy(block.data() + 16, geo_->meta_uuid.data(), 16);
      PutBE64(block.data(), 32, inode_.ino);
      PutBE64(block.data(), 40, geo_->FsbToByte(block_fsb) / 512);
      PutBE64(block.data(), 48, kAttrNullCommitLsn);
      if (bytes != 0) {
        std::memcpy(block.data() + kAttrRemoteHeaderSize,
                    e.value.data() + offset, bytes);
      }
      UpdateXfsChecksum(block.data(), block.size(), 12);
      if (!device_->Write(geo_->FsbToByte(block_fsb), block.data(),
                          block.size())) {
        return XFS_ERR(XfsError::kIoError,
                       "failed to write remote attr value")
            .Ino(inode_.ino)
            .Fsb(block_fsb)
            .Metadata("attr remote");
      }
      offset += bytes;
      remaining -= bytes;
    }
    remote_written_ = true;
    MaybeCrash(CrashPoint::kAfterDataWrite,
               "xattr remote ino=" + std::to_string(inode_.ino));
    return OkStatus();
  }

  Status BuildSingleLeaf() {
    // Reserve logical block 0 (the DA root) before any remote value gets a
    // logical offset; xfs_bmap_first_unused() would otherwise hand logical 0
    // to the value and the leaf would overwrite it.
    uint64_t root_fsb = 0;
    Status s = blocks_->EnsureRoot(&root_fsb);
    if (!s.ok()) return s;
    s = AllocateChangedRemote();
    if (!s.ok()) return s;
    std::vector<XattrEntry> sorted = SortByHash(entries_);
    // Free every old attr block except logical 0 and remote blocks retained by
    // unchanged entries.
    std::set<uint32_t> keep;
    keep.insert(0);
    for (const XattrEntry& e : sorted) {
      if (e.remote) {
        for (uint32_t i = 0; i < e.remote_blocks; ++i) {
          keep.insert(e.remote_lbno + i);
        }
      }
    }
    s = FreeOldExcept(keep);
    if (!s.ok()) return s;
    s = PublishLeaf(root_fsb, 0, 0, sorted);
    if (!s.ok()) return s;
    target_aformat_ = di::kFormatExtents;
    target_anextents_ = editor_->nextents();
    s = FinalizeAttrFork(&attr_image_, &attr_fields_, &attr_region_,
                         &target_aformat_, &target_anextents_);
    if (!s.ok()) return s;
    return OkStatus();
  }

  Status BuildFullTree() {
    // Reserve the root logical block before remote values or leaves.
    uint64_t root_fsb = 0;
    Status s = blocks_->EnsureRoot(&root_fsb);
    if (!s.ok()) return s;
    s = AllocateChangedRemote();
    if (!s.ok()) return s;
    std::vector<XattrEntry> sorted = SortByHash(entries_);
    const size_t capacity = geo_->block_size - kLeafHeader;
    std::vector<std::vector<XattrEntry>> leaf_groups;
    std::vector<XattrEntry> current;
    size_t current_bytes = kLeafHeader;
    for (XattrEntry& e : sorted) {
      const size_t bytes =
          EntrySize(e) + kAttrLeafEntrySize;
      if (EntrySize(e) + kLeafHeader > geo_->block_size) {
        return XFS_ERR(XfsError::kNoSpace,
                       "a single xattr entry does not fit a leaf block")
            .Ino(inode_.ino)
            .Metadata("attr leaf");
      }
      if (!current.empty() && current_bytes + bytes > capacity) {
        leaf_groups.push_back(std::move(current));
        current.clear();
        current_bytes = kLeafHeader;
      }
      current.push_back(std::move(e));
      current_bytes += bytes;
    }
    if (!current.empty()) leaf_groups.push_back(std::move(current));
    if (leaf_groups.empty()) {
      return XFS_ERR(XfsError::kInternal, "tree builder received no entries")
          .Ino(inode_.ino)
          .Metadata("attr fork");
    }

    struct ChildRef {
      uint32_t hash = 0;
      uint32_t logical = 0;
    };
    std::vector<ChildRef> children;
    std::set<uint32_t> keep;
    const size_t node_max = NodeMaxRecs(geo_->block_size);

    if (leaf_groups.size() == 1) {
      keep.insert(0);
      for (const XattrEntry& e : leaf_groups[0]) {
        if (e.remote) {
          for (uint32_t i = 0; i < e.remote_blocks; ++i) {
            keep.insert(e.remote_lbno + i);
          }
        }
      }
      s = FreeOldExcept(keep);
      if (!s.ok()) return s;
      s = PublishLeaf(root_fsb, 0, 0, leaf_groups[0]);
      if (!s.ok()) return s;
      target_leaf_count_ = 1;
    } else {
      keep.insert(0);

      // Leaves.  Logical assignment is arbitrary within the fork address
      // space; sibling links carry the hash order.
      uint32_t previous = 0;
      std::vector<WorkingLeaf> leaves;
      for (size_t i = 0; i < leaf_groups.size(); ++i) {
        uint32_t logical = 0;
        uint64_t fsb = 0;
        s = blocks_->AllocateRun(1, &logical, &fsb);
        if (!s.ok()) return s;
        (void)fsb;
        WorkingLeaf leaf;
        leaf.logical = logical;
        leaf.back = previous;
        leaf.entries = std::move(leaf_groups[i]);
        if (!leaves.empty()) leaves.back().forw = logical;
        previous = logical;
        leaves.push_back(std::move(leaf));
      }
      for (const WorkingLeaf& leaf : leaves) {
        keep.insert(leaf.logical);
        for (const XattrEntry& e : leaf.entries) {
          if (e.remote) {
            for (uint32_t i = 0; i < e.remote_blocks; ++i) {
              keep.insert(e.remote_lbno + i);
            }
          }
        }
      }
      for (WorkingLeaf& leaf : leaves) {
        uint64_t fsb = 0;
        if (!blocks_->Map(leaf.logical, &fsb)) {
          return XFS_ERR(XfsError::kInternal, "new attr leaf is not mapped")
              .Ino(inode_.ino)
              .Metadata("attr leaf");
        }
        s = PublishLeaf(fsb, leaf.forw, leaf.back,
                        leaf.entries);
        if (!s.ok()) return s;
        children.push_back({LastHash(leaf.entries), leaf.logical});
      }

      // Build intermediate node levels until the remaining children fit a
      // single root node at logical 0.
      uint16_t level = 1;
      while (children.size() > node_max) {
        const size_t groups =
            (children.size() + node_max - 1) / node_max;
        const size_t per_group =
            (children.size() + groups - 1) / groups;
        std::vector<ChildRef> next;
        uint32_t previous_node = 0;
        std::vector<WorkingNode> nodes;
        for (size_t g = 0; g < children.size(); g += per_group) {
          const size_t end = std::min(children.size(), g + per_group);
          uint32_t logical = 0;
          uint64_t fsb = 0;
          s = blocks_->AllocateRun(1, &logical, &fsb);
          if (!s.ok()) return s;
          (void)fsb;
          WorkingNode node;
          node.logical = logical;
          node.level = level;
          node.back = previous_node;
          if (!nodes.empty()) nodes.back().forw = logical;
          previous_node = logical;
          for (size_t i = g; i < end; ++i) {
            node.entries.push_back({children[i].hash, children[i].logical});
          }
          next.push_back({node.entries.back().hash, node.logical});
          nodes.push_back(std::move(node));
        }
        for (const WorkingNode& node : nodes) keep.insert(node.logical);
        for (WorkingNode& node : nodes) {
          uint64_t fsb = 0;
          if (!blocks_->Map(node.logical, &fsb)) {
            return XFS_ERR(XfsError::kInternal, "new attr node is not mapped")
                .Ino(inode_.ino)
                .Metadata("attr node");
          }
          s = PublishNode(fsb, node.forw, node.back, node.level,
                          node.entries);
          if (!s.ok()) return s;
        }
        children = std::move(next);
        ++level;
      }

      std::vector<NodeEditEntry> root_entries;
      for (const ChildRef& child : children) {
        root_entries.push_back({child.hash, child.logical});
      }
      s = PublishNode(root_fsb, 0, 0, level, root_entries);
      if (!s.ok()) return s;
      target_leaf_count_ = static_cast<uint32_t>(leaves.size());
    }

    // Free everything not part of the new layout, including old DA blocks and
    // remote values that were replaced.
    s = FreeOldExcept(keep);
    if (!s.ok()) return s;

    target_aformat_ = di::kFormatExtents;
    target_anextents_ = editor_->nextents();
    s = FinalizeAttrFork(&attr_image_, &attr_fields_, &attr_region_,
                         &target_aformat_, &target_anextents_);
    if (!s.ok()) return s;
    return OkStatus();
  }

  Status PublishLeaf(uint64_t fsb, uint32_t forw, uint32_t back,
                     const std::vector<XattrEntry>& entries) {
    std::vector<uint8_t> block =
        BuildLeaf(*geo_, inode_.ino, forw, back, entries);
    PutBE64(block.data(), 16, geo_->FsbToByte(fsb) / 512);
    return txn_->AddMetadataBuffer(geo_->FsbToByte(fsb), std::move(block),
                                   lf::kBlftAttrLeaf, kLeafLsnOff, kLeafCrcOff,
                                   "attr leaf");
  }

  Status PublishNode(uint64_t fsb, uint32_t forw, uint32_t back,
                     uint16_t level,
                     const std::vector<NodeEditEntry>& entries) {
    std::vector<uint8_t> block =
        BuildNode(*geo_, inode_.ino, forw, back, level, entries);
    PutBE64(block.data(), 16, geo_->FsbToByte(fsb) / 512);
    return txn_->AddMetadataBuffer(geo_->FsbToByte(fsb), std::move(block),
                                   lf::kBlftDaNode, kLeafLsnOff, kLeafCrcOff,
                                   "attr node");
  }

  Status BuildIncrementalTree() {
    Status s = AllocateChangedRemote();
    if (!s.ok()) return s;
    TreeEditor editor(*geo_, inode_, blocks_.get());
    s = editor.Load(fork_);
    if (!s.ok()) return s;
    if (removed_entry_.has_value()) {
      s = editor.Remove(*removed_entry_);
      if (!s.ok()) return s;
    }
    if (changed_index_ >= 0) {
      if (static_cast<size_t>(changed_index_) >= entries_.size()) {
        return XFS_ERR(XfsError::kInternal, "changed xattr index is invalid")
            .Ino(inode_.ino)
            .Metadata("xattr");
      }
      // The tree editor replaces the key if present, otherwise inserts.  A
      // rename can remove the old parent pointer and insert the new one in
      // the same increment (they may even share a hash slot).
      s = editor.Insert(entries_[changed_index_]);
      if (!s.ok()) return s;
    }
    if (removed_remote_.has_value()) {
      s = blocks_->FreeRange(removed_remote_->first,
                             removed_remote_->second);
      if (!s.ok()) return s;
    }
    s = editor.Publish(txn_);
    if (!s.ok()) return s;

    target_aformat_ = di::kFormatExtents;
    target_anextents_ = editor_->nextents();
    attr_image_.assign(
        static_cast<size_t>(Litino(*geo_) - target_forkoff_ * 8), 0);
    s = FinalizeAttrFork(&attr_image_, &attr_fields_, &attr_region_,
                         &target_aformat_, &target_anextents_);
    if (!s.ok()) return s;
    return OkStatus();
  }

  // Frees old attr extents that are not in `keep` (one logical block per set
  // element).  The root block 0 is always kept. A run may mix DA leaf/node
  // blocks with direct-written remote values; only the DA blocks have a
  // buffer log item to cancel, so split the range at that boundary.
  Status FreeOldExcept(const std::set<uint32_t>& keep) {
    std::set<uint32_t> da_logicals;
    for (const XattrDaLeaf& leaf : fork_.leaves) {
      da_logicals.insert(leaf.logical);
    }
    for (const XattrDaNode& node : fork_.nodes) {
      da_logicals.insert(node.logical);
    }
    for (const XfsExtent& e : fork_.extents) {
      uint32_t b = static_cast<uint32_t>(e.startoff);
      const uint32_t end = static_cast<uint32_t>(e.EndBlock());
      while (b < end) {
        if (keep.count(b)) {
          ++b;
          continue;
        }
        const bool is_da = da_logicals.count(b) != 0;
        uint32_t run = 1;
        while (b + run < end && !keep.count(b + run) &&
               (da_logicals.count(b + run) != 0) == is_da) {
          ++run;
        }
        Status s = is_da ? blocks_->FreeDaRange(b, run)
                         : blocks_->FreeRange(b, run);
        if (!s.ok()) return s;
        b += run;
      }
    }
    return OkStatus();
  }

  Status FreeOldExceptKeepAllPublished(const std::set<uint32_t>& keep) {
    return FreeOldExcept(keep);
  }

  Status FinalizeAttrFork(std::vector<uint8_t>* image, uint32_t* fields,
                          std::vector<uint8_t>* region, uint8_t* aformat,
                          uint32_t* anextents) {
    // A shortform fork does not go through the bmap editor.
    if (target_aformat_ == di::kFormatLocal) {
      *fields = lf::kILogAData;
      *region = attr_image_;
      return OkStatus();
    }
    const size_t fork_size =
        static_cast<size_t>(Litino(*geo_) - target_forkoff_ * 8);
    Status fork_status = editor_->SetForkBytes(
        static_cast<uint32_t>(fork_size));
    if (!fork_status.ok()) return fork_status;
    std::vector<uint8_t> temp = raw_;
    uint32_t data_fields = 0;
    std::vector<uint8_t> data_region;
    Status s = editor_->Finalize(&temp, &data_fields, &data_region);
    if (!s.ok()) return s;
    (void)data_fields;
    (void)data_region;
    *aformat = editor_->format();
    *anextents = editor_->nextents();
    image->assign(fork_size, 0);
    const size_t bytes = std::min<size_t>(fork_size,
                                          editor_->fork_bytes());
    std::memcpy(image->data(), temp.data() + di::kCoreSizeV3, bytes);
    uint32_t mapped = 0;
    if (*aformat == di::kFormatBtree) {
      mapped = lf::kILogABRoot;
      // DataForkEditor::Finalize logs the in-core btree root in data_region.
      *region = std::move(data_region);
    } else {
      mapped = lf::kILogAExt;
      if (!editor_->extents().empty()) {
        region->assign(editor_->extents().size() * 16, 0);
        std::memcpy(region->data(), temp.data() + di::kCoreSizeV3,
                    region->size());
      } else {
        region->clear();
      }
    }
    *fields = mapped;
    return OkStatus();
  }

  Status PrepareDataFork(PreparedInode* out) {
    const uint32_t litino = Litino(*geo_);
    const uint32_t old_size =
        inode_.forkoff ? inode_.forkoff * 8 : litino;
    const uint32_t new_size =
        target_forkoff_ ? target_forkoff_ * 8 : litino;
    bool need_editor = false;
    if (inode_.format == di::kFormatBtree && new_size != old_size) {
      // The on-disk bmdr key/pointer layout depends on the data fork size.
      need_editor = true;
    } else if (new_size < old_size) {
      uint64_t required = 0;
      switch (inode_.format) {
        case di::kFormatExtents:
          required = static_cast<uint64_t>(inode_.nextents) * 16;
          break;
        case di::kFormatLocal:
          required = inode_.size;
          break;
        case di::kFormatDev:
          required = 8;
          break;
        default:
          required = 0;
          break;
      }
      if (inode_.format == di::kFormatExtents &&
          required > new_size) {
        need_editor = true;
      } else if (inode_.format == di::kFormatLocal &&
                 required > new_size) {
        return XFS_ERR(XfsError::kNoSpace,
                       "not enough literal space for the local data fork")
            .Ino(inode_.ino)
            .Metadata("inode");
      }
    }
    if (!need_editor) return OkStatus();

    DataForkEditor data_editor(txn_, alloc_, device_, geo_, false);
    // Growing the data fork can make an existing btree unnecessary.  xfs_repair
    // rejects a btree whose extent count fits the literal area, so allow the
    // editor to collapse in that direction; shrinking still keeps the btree
    // (or moves its root) because the extents no longer fit.
    data_editor.SetAllowCollapse(new_size > old_size);
    Status s = data_editor.Load(inode_);
    if (!s.ok()) return s;
    // Load() takes fork_bytes_ from the inode being edited; resize only after
    // the tree/root has been parsed.
    s = data_editor.SetForkBytes(new_size);
    if (!s.ok()) return s;
    uint32_t fields = 0;
    std::vector<uint8_t> region;
    s = data_editor.Finalize(&out->image, &fields, &region);
    if (!s.ok()) return s;
    out->data_fields = fields;
    out->data_region = std::move(region);
    // Finalize already applied the bmbt/allocation delta to out->image.
    return OkStatus();
  }

  Status Finish(PreparedInode* out) {
    out->image = raw_;
    Status s = PrepareDataFork(out);
    if (!s.ok()) return s;

    // Place the attr fork image at the requested forkoff and clear the old
    // tail.  The data fork is already laid out at offset 176.
    uint8_t* core = out->image.data();
    core[di::kForkoff] = static_cast<uint8_t>(target_forkoff_);
    core[di::kAformat] = target_aformat_;
    SetInodeAttrExtentCount(core, target_anextents_);
    const uint32_t attr_size =
        target_forkoff_ ? litino_value() - target_forkoff_ * 8 : 0;
    if (attr_size != 0) {
      std::memset(core + di::kCoreSizeV3 + static_cast<size_t>(
                      target_forkoff_) * 8,
                  0, attr_size);
      if (!attr_image_.empty()) {
        std::memcpy(core + di::kCoreSizeV3 +
                        static_cast<size_t>(target_forkoff_) * 8,
                    attr_image_.data(),
                    std::min<size_t>(attr_image_.size(), attr_size));
      }
    } else if (target_aformat_ == 0) {
      // No attr fork: the literal area belongs entirely to the data fork.
    }
    if (editor_) {
      const int64_t delta = editor_->nblocks_delta();
      const uint64_t old_nblocks = GetBE64(core, di::kNblocks);
      const int64_t next = static_cast<int64_t>(old_nblocks) + delta;
      if (next < 0) {
        return XFS_ERR(XfsError::kInternal,
                       "attr fork block accounting underflow")
            .Ino(inode_.ino)
            .Metadata("inode");
      }
      PutBE64(core, di::kNblocks, static_cast<uint64_t>(next));
    }
    const XfsTimestamp now = CurrentTimestamp();
    s = StoreInodeTimestamp(core, di::kCtime, now);
    if (!s.ok()) return s;
    out->attr_fields = attr_fields_;
    out->attr_region = attr_region_;
    return OkStatus();
  }

  uint32_t litino_value() const { return Litino(*geo_); }

  Status BuildSingleLeafRemoteFree() { return OkStatus(); }

  XfsTransaction* txn_;
  BlockAllocator* alloc_;
  BlockDevice* device_;
  const XfsGeometry* geo_;
  const XfsInode& inode_;
  const XattrFork& fork_;
  std::vector<uint8_t> raw_;
  std::vector<XattrEntry> entries_;
  int changed_index_ = -1;
  std::optional<std::pair<uint32_t, uint32_t>> removed_remote_;
  std::optional<XattrEntry> removed_entry_;
  bool preserve_shortform_empty_ = false;
  bool parent_pointers_ = false;
  bool remote_written_ = false;

  std::unique_ptr<DataForkEditor> editor_;
  std::unique_ptr<AttrBlockMap> blocks_;
  Target target_ = Target::kNone;
  uint32_t target_forkoff_ = 0;
  uint8_t target_aformat_ = 0;
  uint32_t target_anextents_ = 0;
  uint32_t target_leaf_count_ = 0;
  std::vector<uint8_t> attr_image_;
  uint32_t attr_fields_ = 0;
  std::vector<uint8_t> attr_region_;
};

}  // namespace

Result<PreparedInode> PrepareXattrMutation(
    XfsTransaction& txn, BlockAllocator& alloc, BlockDevice& device,
    const XfsGeometry& geo, const XfsInode& inode, const XattrFork& fork,
    std::vector<uint8_t> raw, std::vector<XattrEntry> entries,
    int changed_index,
    std::optional<std::pair<uint32_t, uint32_t>> removed_remote,
    std::optional<XattrEntry> removed_entry, bool preserve_shortform_empty) {
  XattrWriter writer(&txn, &alloc, &device, &geo, inode, fork, std::move(raw),
                     geo.features.parent);
  PreparedInode prepared;
  Status s = writer.Prepare(std::move(entries), changed_index, removed_remote,
                            std::move(removed_entry), preserve_shortform_empty,
                            &prepared);
  if (!s.ok()) return s.error();
  return prepared;
}

Result<std::vector<XattrEntry>> XfsFilesystem::ListXattrs(
    uint64_t ino, bool include_private) const {
  std::shared_lock<xfs::conc::SharedMutex> barrier(write_barrier_);
  return ListXattrsLocked(ino, include_private);
}

Result<std::vector<XattrEntry>> XfsFilesystem::ListXattrsLocked(
    uint64_t ino, bool include_private) const {
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  return xfs::ListXattrs(*device_, geometry_, inode.value(), include_private);
}

Result<XattrEntry> XfsFilesystem::GetXattr(uint64_t ino,
                                           std::string_view name,
                                           bool include_private) const {
  std::shared_lock<xfs::conc::SharedMutex> barrier(write_barrier_);
  return GetXattrLocked(ino, name, include_private);
}

Result<XattrEntry> XfsFilesystem::GetXattrLocked(
    uint64_t ino, std::string_view name, bool include_private) const {
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  return xfs::GetXattr(*device_, geometry_, inode.value(), name,
                       include_private);
}

Result<XattrEntry> XfsFilesystem::SetXattr(uint64_t ino,
                                           std::string_view name,
                                           const void* value,
                                           size_t value_len,
                                           XattrSetMode mode) {
  // Serialize with namespace/data writes: xattr changes publish an inode
  // metadata transaction and must not interleave with a file-data mutation.
  XFS_CONC_SYNC("namespace.barrier.before", "op=namespace");
  std::unique_lock<xfs::conc::SharedMutex> namespace_barrier(write_barrier_);
  XFS_CONC_SYNC("namespace.barrier.after", "op=namespace");
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume,
                   "filesystem is mounted read-only")
        .Ino(ino)
        .Metadata("xattr");
  }
  if (value_len > kXattrMaxValueBytes) {
    return XFS_ERR(XfsError::kOutOfRange,
                   "xattr value exceeds the XFS size limit")
        .Ino(ino)
        .Metadata("xattr")
        .Actual(value_len);
  }
  std::string on_disk_name;
  auto ns = XattrNamespace::ParseName(name, &on_disk_name, false);
  if (!ns.ok()) return ns.error();

  std::lock_guard<xfs::conc::Mutex> lock(mutation_mutex_);
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  auto fork = ReadXattrFork(*device_, geometry_, inode.value());
  if (!fork.ok()) return fork.error();

  XattrEntry entry;
  entry.flags = ns.value().flags;
  entry.name = on_disk_name;
  entry.hash = AttrHashName(entry.name);
  entry.value.assign(static_cast<const uint8_t*>(value),
                     static_cast<const uint8_t*>(value) + value_len);
  entry.remote = ValueMustBeRemote(entry.name, value_len, geometry_.block_size);
  entry.remote_value_len =
      entry.remote ? static_cast<uint32_t>(value_len) : 0;
  entry.remote_lbno = 0;
  entry.remote_blocks = 0;
  entry.incomplete = false;

  std::vector<XattrEntry> entries = fork.value().entries;
  int changed_index = -1;
  std::optional<std::pair<uint32_t, uint32_t>> removed_remote;
  bool existing = false;
  for (size_t i = 0; i < entries.size(); ++i) {
    if ((entries[i].flags & kAttrNspOnDiskMask) == entry.flags &&
        entries[i].name == entry.name) {
      existing = true;
      if (mode == XattrSetMode::kCreate) {
        return XFS_ERR(XfsError::kAlreadyExists,
                       "xattr already exists")
            .Ino(ino)
            .Metadata("xattr");
      }
      if (entries[i].remote && entries[i].remote_blocks != 0) {
        removed_remote = std::make_pair(entries[i].remote_lbno,
                                        entries[i].remote_blocks);
      }
      entries[i] = entry;
      if (i > static_cast<size_t>(INT32_MAX)) {
        return XFS_ERR(XfsError::kInternal, "too many xattrs")
            .Ino(ino)
            .Metadata("xattr");
      }
      changed_index = static_cast<int>(i);
      break;
    }
  }
  if (!existing) {
    if (mode == XattrSetMode::kReplace) {
      return XFS_ERR(XfsError::kNotFound,
                     "xattr does not exist (XATTR_REPLACE)")
          .Ino(ino)
          .Metadata("xattr");
    }
    if (entries.size() > static_cast<size_t>(INT32_MAX)) {
      return XFS_ERR(XfsError::kInternal, "too many xattrs")
          .Ino(ino)
          .Metadata("xattr");
    }
    changed_index = static_cast<int>(entries.size());
    entries.push_back(entry);
  }

  auto raw = ReadRawInode(ino);
  if (!raw.ok()) return raw.error();
  auto txn = BeginTransaction();
  if (!txn.ok()) return txn.error();
  BlockAllocator alloc(&txn.value(), device_.get(), &geometry_);
  XattrWriter writer(&txn.value(), &alloc, device_.get(), &geometry_,
                     inode.value(), fork.value(), std::move(raw.value()),
                     geometry_.features.parent);
  PreparedInode prepared;
  Status s = writer.Prepare(std::move(entries), changed_index, removed_remote,
                            std::nullopt, false, &prepared);
  if (!s.ok()) return s.error();
  s = LogPreparedInode(txn.value(), ino, std::move(prepared.image),
                       prepared.data_fields, prepared.data_region,
                       prepared.attr_fields, prepared.attr_region);
  if (!s.ok()) return s.error();
  s = alloc.Commit();
  if (!s.ok()) return s.error();
  s = CommitTransaction(txn.value());
  if (!s.ok()) return s.error();
  return GetXattrLocked(ino, name, /*include_private=*/false);
}

Status XfsFilesystem::RemoveXattr(uint64_t ino, std::string_view name) {
  // Serialize with namespace/data writes: xattr changes publish an inode
  // metadata transaction and must not interleave with a file-data mutation.
  XFS_CONC_SYNC("namespace.barrier.before", "op=namespace");
  std::unique_lock<xfs::conc::SharedMutex> namespace_barrier(write_barrier_);
  XFS_CONC_SYNC("namespace.barrier.after", "op=namespace");
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume,
                   "filesystem is mounted read-only")
        .Ino(ino)
        .Metadata("xattr");
  }
  std::string on_disk_name;
  auto ns = XattrNamespace::ParseName(name, &on_disk_name, false);
  if (!ns.ok()) return ns.error();

  std::lock_guard<xfs::conc::Mutex> lock(mutation_mutex_);
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  auto fork = ReadXattrFork(*device_, geometry_, inode.value());
  if (!fork.ok()) return fork.error();

  std::vector<XattrEntry> entries = fork.value().entries;
  std::optional<std::pair<uint32_t, uint32_t>> removed_remote;
  std::optional<XattrEntry> removed_entry;
  bool found = false;
  for (auto it = entries.begin(); it != entries.end(); ++it) {
    if ((it->flags & kAttrNspOnDiskMask) == ns.value().flags &&
        it->name == on_disk_name) {
      if (it->remote && it->remote_blocks != 0) {
        removed_remote = std::make_pair(it->remote_lbno, it->remote_blocks);
      }
      removed_entry = *it;
      entries.erase(it);
      found = true;
      break;
    }
  }
  if (!found) {
    return XFS_ERR(XfsError::kNotFound, "xattr not found")
        .Ino(ino)
        .Metadata("xattr");
  }

  auto raw = ReadRawInode(ino);
  if (!raw.ok()) return raw.error();
  auto txn = BeginTransaction();
  if (!txn.ok()) return txn.error();
  BlockAllocator alloc(&txn.value(), device_.get(), &geometry_);
  XattrWriter writer(&txn.value(), &alloc, device_.get(), &geometry_,
                     inode.value(), fork.value(), std::move(raw.value()),
                     geometry_.features.parent);
  PreparedInode prepared;
  Status s = writer.Prepare(std::move(entries), -1, removed_remote,
                            std::move(removed_entry), false, &prepared);
  if (!s.ok()) return s.error();
  s = LogPreparedInode(txn.value(), ino, std::move(prepared.image),
                       prepared.data_fields, prepared.data_region,
                       prepared.attr_fields, prepared.attr_region);
  if (!s.ok()) return s.error();
  s = alloc.Commit();
  if (!s.ok()) return s.error();
  return CommitTransaction(txn.value());
}

}  // namespace xfs
