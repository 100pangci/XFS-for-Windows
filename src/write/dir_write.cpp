// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - directory mutation (RW-3).
//
// Layouts and rules follow fs/xfs/libxfs/xfs_dir2_{sf,block,leaf,node,data}.c,
// xfs_da_btree.c and xfsprogs repair/dir2.c. See xfs/dir_write.h for the
// invariants this file maintains.
//
// Implementation notes:
//   - shortform and block directories keep their entries as a list with their
//     data-space offsets and rebuild the image from scratch on every change
//     (the result is what the kernel's add/remove/compact loops produce, with
//     zero stale entries);
//   - leaf/node data blocks keep an image and use the kernel's use-free and
//     make-free primitives; bestfree is recomputed after every change
//     (xfs_dir2_data_freescan semantics);
//   - the da btree is rebuilt from the leaf chain after every structural
//     change: keys are the last hash of each child (xfs_da3_fixhashpath),
//     which is exactly what lookup and repair expect;
//   - data blocks are grown in the lowest hole of the data space
//     (xfs_bmap_first_unused) and di_size follows xfs_dir2_grow_inode /
//     xfs_dir2_shrink_inode.
#include "xfs/dir_write.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

#include "xfs/alloc.h"
#include "xfs/bmap.h"
#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/log_format.h"
#include "xfs/ondisk.h"

namespace xfs {
namespace {

namespace d = ondisk::dir;
namespace di = ondisk::inode;
namespace lf = logfmt;

constexpr uint16_t kNullDataOff = 0xffff;
constexpr uint64_t kNullFsb = ~0ull;

// dir3 block header (data / block / free share the xfs_dir3_blk_hdr layout).
constexpr size_t kBlkMagic = 0;      // be32
constexpr size_t kBlkCrc = 4;        // le32
constexpr size_t kBlkBlkno = 8;      // be64
constexpr size_t kBlkLsn = 16;       // be64
constexpr size_t kBlkUuid = 24;      // 16 bytes
constexpr size_t kBlkOwner = 40;     // be64
constexpr size_t kBlkBestFree = 48;  // 3 x (be16 offset, be16 length)
// da3 blkinfo (leaf / node).
constexpr size_t kInfoForw = 0;    // be32
constexpr size_t kInfoBack = 4;    // be32
constexpr size_t kInfoMagic = 8;   // be16
constexpr size_t kInfoCrc = 12;    // le32
constexpr size_t kInfoBlkno = 16;  // be64
constexpr size_t kInfoLsn = 24;    // be64
constexpr size_t kInfoUuid = 32;   // 16 bytes
constexpr size_t kInfoOwner = 48;  // be64
constexpr size_t kLeafCount = 56;  // be16
constexpr size_t kLeafStale = 58;  // be16
constexpr size_t kNodeCount = 56;  // be16
constexpr size_t kNodeLevel = 58;  // be16
constexpr size_t kEnts = 64;       // first leaf / node / bests entry
// xfs_dir3_free_hdr
constexpr size_t kFreeFirstdb = 48;  // be32
constexpr size_t kFreeNvalid = 52;   // be32
constexpr size_t kFreeNused = 56;    // be32
constexpr size_t kFreeBests = 64;    // be16[]

constexpr size_t kBlockTail = 8;  // be32 count + be32 stale

uint32_t Rol32(uint32_t v, int bits) {
  bits &= 31;
  return (v << bits) | (v >> (32 - bits));
}

uint32_t DirHash(std::string_view name) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(name.data());
  const size_t len = name.size();
  uint32_t hash = 0;
  size_t i = 0;
  for (; i + 4 <= len; i += 4) {
    hash = (static_cast<uint32_t>(p[i]) << 21) ^
           (static_cast<uint32_t>(p[i + 1]) << 14) ^
           (static_cast<uint32_t>(p[i + 2]) << 7) ^
           static_cast<uint32_t>(p[i + 3]) ^ Rol32(hash, 7 * 4);
  }
  switch (len - i) {
    case 3:
      return (static_cast<uint32_t>(p[i]) << 14) ^
             (static_cast<uint32_t>(p[i + 1]) << 7) ^
             static_cast<uint32_t>(p[i + 2]) ^ Rol32(hash, 7 * 3);
    case 2:
      return (static_cast<uint32_t>(p[i]) << 7) ^
             static_cast<uint32_t>(p[i + 1]) ^ Rol32(hash, 7 * 2);
    case 1:
      return static_cast<uint32_t>(p[i]) ^ Rol32(hash, 7);
    default:
      return hash;
  }
}

Status ValidateDirName(std::string_view name, uint64_t ino) {
  if (name.empty() || name.size() > 255) {
    return XFS_ERR(XfsError::kNameTooLong, "invalid directory name length")
        .Ino(ino)
        .Actual(name.size());
  }
  if (name == "." || name == "..") {
    return XFS_ERR(XfsError::kAlreadyExists,
                   "'.' and '..' are implicit directory entries")
        .Ino(ino);
  }
  if (name.find('/') != std::string_view::npos ||
      name.find('\0') != std::string_view::npos) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "directory name contains '/' or NUL")
        .Ino(ino);
  }
  return OkStatus();
}

}  // namespace

// ---------------------------------------------------------------------------
// Constructor / geometry / capability check
// ---------------------------------------------------------------------------

DirectoryEditor::DirectoryEditor(XfsTransaction* txn, BlockAllocator* alloc,
                                 BlockDevice* device, const XfsGeometry* geo,
                                 DataForkEditor* fork)
    : txn_(txn), alloc_(alloc), device_(device), geo_(geo), fork_(fork) {
  block_log_ = geo->block_log;
  dir_block_size_ = geo->dir_block_size;
  leaf_max_ents_ = (dir_block_size_ - kEnts) / 8;
  node_max_ents_ = (dir_block_size_ - kEnts) / 8;
  free_max_bests_ = (dir_block_size_ - kEnts) / 2;
  root_dablk_ = static_cast<uint32_t>(d::kLeafOffset >> block_log_);
}

DirectoryEditor::~DirectoryEditor() = default;

Status DirectoryEditor::CheckMutable(const XfsInode& inode) {
  if (!inode.IsDir()) {
    return XFS_ERR(XfsError::kNotDirectory, "inode is not a directory")
        .Ino(inode.ino);
  }
  if (inode.format != di::kFormatLocal &&
      inode.format != di::kFormatExtents &&
      inode.format != di::kFormatBtree) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "directory data fork format is not supported for mutation")
        .Ino(inode.ino)
        .Metadata("inode")
        .Actual(inode.format);
  }
  // The directory writer computes offsets in the full literal area; a
  // directory with an attribute fork would shift its data fork. Attribute
  // mutation is out of scope for RW-3, so refuse instead of guessing.
  if (inode.forkoff != 0 || inode.anextents != 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "directories with attribute forks are not mutable yet")
        .Ino(inode.ino)
        .Metadata("inode");
  }
  return OkStatus();
}

Status DirectoryEditor::ReadDirBlock(uint64_t db,
                                     std::vector<uint8_t>* buf) const {
  const uint64_t fsb = FsbForFileBlock(db);
  if (fsb == kNullFsb) {
    return XFS_ERR(XfsError::kCorruptDirectory, "directory block not mapped")
        .Ino(ino_)
        .DirBlock(db);
  }
  buf->assign(dir_block_size_, 0);
  if (!device_->Read(geo_->FsbToByte(fsb), buf->data(), buf->size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read directory block")
        .Ino(ino_)
        .DirBlock(db);
  }
  return OkStatus();
}

uint64_t DirectoryEditor::FsbForFileBlock(uint64_t file_block) const {
  for (const XfsExtent& e : fork_->extents()) {
    if (e.startoff <= file_block && file_block < e.EndBlock()) {
      return e.startblock + (file_block - e.startoff);
    }
  }
  return kNullFsb;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

Status DirectoryEditor::Load(const XfsInode& inode) {
  ino_ = inode.ino;
  Status s = CheckMutable(inode);
  if (!s.ok()) return s;
  dirty_ = false;
  placed_.clear();
  data_blocks_.clear();
  data_dirty_.clear();
  bests_.clear();
  leaf_blocks_.clear();
  node_blocks_.clear();
  free_blocks_.clear();
  block_dirty_ = false;
  data_size_ = inode.size;

  if (inode.format == di::kFormatLocal) return LoadShortform(inode);
  std::vector<uint8_t> buf;
  s = ReadDirBlock(0, &buf);
  if (!s.ok()) return s;
  const uint32_t magic = GetBE32(buf.data(), kBlkMagic);
  if (magic == ondisk::kDir3BlockMagic) return LoadBlock();
  if (magic != ondisk::kDir3DataMagic) {
    return XFS_ERR(XfsError::kCorruptDirectory,
                   "unexpected directory block magic")
        .Ino(ino_)
        .DirBlock(0)
        .Expected(ondisk::kDir3DataMagic)
        .Actual(magic);
  }
  std::vector<uint8_t> root;
  s = ReadDirBlock(root_dablk_, &root);
  if (!s.ok()) return s;
  if (GetBE16(root.data(), kInfoMagic) == ondisk::kDir3Leaf1Magic) {
    return LoadLeaf();
  }
  return LoadNode();
}

Status DirectoryEditor::LoadShortform(const XfsInode& inode) {
  fmt_ = XfsDirFormat::kShortform;
  const std::vector<uint8_t>& fork = inode.data_fork;
  if (fork.size() < 6) {
    return XFS_ERR(XfsError::kCorruptDirectory, "shortform directory too short")
        .Ino(ino_);
  }
  const uint8_t count = fork[d::kSfHdrCount];
  const uint8_t i8count = fork[d::kSfHdrI8Count];
  const bool i8 = i8count != 0;
  const size_t hdr = i8 ? d::kSfHdrSizeFixed : 6;
  sf_parent_ = i8 ? GetBE64(fork.data(), d::kSfHdrParent)
                  : GetBE32(fork.data(), d::kSfHdrParent);
  size_t off = hdr;
  uint32_t last_offset = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (off + 3 > fork.size()) {
      return XFS_ERR(XfsError::kCorruptDirectory, "shortform entry truncated")
          .Ino(ino_);
    }
    PlacedEntry p;
    const uint8_t namelen = fork[off + d::kSfEntNamelen];
    const size_t entsize =
        3 + namelen + (geo_->features.ftype ? 1 : 0) + (i8 ? 8 : 4);
    if (off + entsize > fork.size()) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "shortform entry extends past the fork")
          .Ino(ino_);
    }
    p.offset = GetBE16(fork.data(), off + d::kSfEntOffset);
    p.e.name.assign(reinterpret_cast<const char*>(fork.data() + off + 3),
                    namelen);
    const size_t ftype_off = off + 3 + namelen;
    if (geo_->features.ftype) p.e.ftype = fork[ftype_off];
    const size_t inum_off = ftype_off + (geo_->features.ftype ? 1 : 0);
    p.e.ino = i8 ? GetBE64(fork.data(), inum_off)
                 : GetBE32(fork.data(), inum_off);
    if (p.offset < last_offset) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "shortform offsets out of order")
          .Ino(ino_);
    }
    last_offset = p.offset;
    placed_.push_back(std::move(p));
    off += entsize;
  }
  return OkStatus();
}

Status DirectoryEditor::LoadBlock() {
  fmt_ = XfsDirFormat::kBlock;
  std::vector<uint8_t> buf;
  Status s = ReadDirBlock(0, &buf);
  if (!s.ok()) return s;
  const uint32_t count = GetBE32(buf.data(), dir_block_size_ - 8);
  if (count > (dir_block_size_ - kEnts) / 8) {
    return XFS_ERR(XfsError::kCorruptDirectory,
                   "block directory leaf count out of range")
        .Ino(ino_)
        .Actual(count);
  }
  const uint32_t leaf_start = dir_block_size_ - kBlockTail - count * 8;
  uint32_t cur = d::kData3HeaderSize;
  while (cur < leaf_start) {
    const uint16_t tag = GetBE16(buf.data(), cur);
    if (tag == d::kDataFreeTag) {
      const uint32_t len = GetBE16(buf.data(), cur + 2);
      if (len < 8 || (len & 7) != 0 || cur + len > leaf_start) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "invalid free record in block directory")
            .Ino(ino_)
            .Actual(cur);
      }
      cur += len;
      continue;
    }
    const uint8_t namelen = GetU8(buf.data(), cur + 8);
    const size_t entsize = DataEntrySize(namelen);
    if (cur + entsize > leaf_start) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "block directory entry out of range")
          .Ino(ino_);
    }
    PlacedEntry p;
    p.offset = cur;
    p.e.name.assign(reinterpret_cast<const char*>(buf.data() + cur + 9),
                    namelen);
    p.e.ino = GetBE64(buf.data(), cur);
    if (geo_->features.ftype) p.e.ftype = GetU8(buf.data(), cur + 9 + namelen);
    if (p.e.name != "." && p.e.name != "..") placed_.push_back(std::move(p));
    cur += static_cast<uint32_t>(entsize);
  }
  // ".." supplies the parent inode for rebuilds.
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t addr = GetBE32(buf.data(), leaf_start + i * 8 + 4);
    if (addr == 0) continue;
    const uint32_t off = (addr & ((1u << (block_log_ - 3)) - 1)) << 3;
    if (off + 11 > dir_block_size_) continue;
    const uint8_t namelen = GetU8(buf.data(), off + 8);
    if (namelen == 2 && buf[off + 9] == '.' && buf[off + 10] == '.') {
      sf_parent_ = GetBE64(buf.data(), off);
    }
  }
  return OkStatus();
}

Status DirectoryEditor::LoadLeaf() {
  fmt_ = XfsDirFormat::kLeaf;
  std::vector<uint8_t> buf;
  Status s = ReadDirBlock(root_dablk_, &buf);
  if (!s.ok()) return s;
  LeafBlock leaf;
  leaf.dablk = root_dablk_;
  leaf.forw = GetBE32(buf.data(), kInfoForw);
  leaf.back = GetBE32(buf.data(), kInfoBack);
  const uint32_t count = GetBE16(buf.data(), kLeafCount);
  if (count > leaf_max_ents_) {
    return XFS_ERR(XfsError::kCorruptDirectory,
                   "leaf entry count out of range")
        .Ino(ino_)
        .Actual(count);
  }
  for (uint32_t i = 0; i < count; ++i) {
    leaf.ents.push_back({GetBE32(buf.data(), kEnts + i * 8),
                         GetBE32(buf.data(), kEnts + i * 8 + 4)});
  }
  const uint32_t bestcount = GetBE32(buf.data(), dir_block_size_ - 4);
  const uint32_t bests_off = dir_block_size_ - 4 - bestcount * 2;
  if (bests_off < kEnts + count * 8) {
    return XFS_ERR(XfsError::kCorruptDirectory,
                   "leaf bests table overlaps the entries")
        .Ino(ino_)
        .Actual(bestcount);
  }
  if ((static_cast<uint64_t>(bestcount) << block_log_) != data_size_) {
    return XFS_ERR(XfsError::kCorruptDirectory,
                   "leaf bestcount does not match the directory size")
        .Ino(ino_)
        .Expected(data_size_ >> block_log_)
        .Actual(bestcount);
  }
  for (uint32_t db = 0; db < bestcount; ++db) {
    const uint16_t best = GetBE16(buf.data(), bests_off + db * 2);
    if (FsbForFileBlock(db) == kNullFsb) {
      if (best != kNullDataOff) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "leaf bests entry references a missing data block")
            .Ino(ino_)
            .DirBlock(db);
      }
      continue;
    }
    std::vector<uint8_t> block;
    s = ReadDirBlock(db, &block);
    if (!s.ok()) return s;
    if (GetBE32(block.data(), kBlkMagic) != ondisk::kDir3DataMagic) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "data block magic mismatch")
          .Ino(ino_)
          .DirBlock(db);
    }
    bests_[db] = best;
    data_blocks_[db] = std::move(block);
  }
  leaf_blocks_[root_dablk_] = std::move(leaf);
  return OkStatus();
}

Status DirectoryEditor::LoadNode() {
  fmt_ = XfsDirFormat::kNode;
  std::vector<std::pair<uint32_t, uint16_t>> stack;  // dablk, expected level
  stack.push_back({root_dablk_, 0});
  std::set<uint32_t> seen;
  while (!stack.empty()) {
    const auto item = stack.back();
    stack.pop_back();
    const uint32_t dablk = item.first;
    if (!seen.insert(dablk).second) {
      return XFS_ERR(XfsError::kCorruptDirectory, "da btree cycle")
          .Ino(ino_)
          .Actual(dablk);
    }
    std::vector<uint8_t> buf;
    Status s = ReadDirBlock(dablk, &buf);
    if (!s.ok()) return s;
    const uint16_t magic = GetBE16(buf.data(), kInfoMagic);
    if (magic == ondisk::kDir3LeafnMagic) {
      LeafBlock leaf;
      leaf.dablk = dablk;
      leaf.forw = GetBE32(buf.data(), kInfoForw);
      leaf.back = GetBE32(buf.data(), kInfoBack);
      const uint32_t count = GetBE16(buf.data(), kLeafCount);
      if (count > leaf_max_ents_) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "leafn entry count out of range")
            .Ino(ino_)
            .Actual(count);
      }
      for (uint32_t i = 0; i < count; ++i) {
        leaf.ents.push_back({GetBE32(buf.data(), kEnts + i * 8),
                             GetBE32(buf.data(), kEnts + i * 8 + 4)});
      }
      leaf_blocks_[dablk] = std::move(leaf);
      continue;
    }
    if (magic != ondisk::kDaNode3Magic) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "unexpected da btree block magic")
          .Ino(ino_)
          .Actual(magic);
    }
    NodeBlock node;
    node.dablk = dablk;
    node.level = GetBE16(buf.data(), kNodeLevel);
    node.forw = GetBE32(buf.data(), kInfoForw);
    node.back = GetBE32(buf.data(), kInfoBack);
    const uint32_t count = GetBE16(buf.data(), kNodeCount);
    if (node.level == 0 || node.level >= 8 || count == 0 ||
        count > node_max_ents_) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "da btree node count/level out of range")
          .Ino(ino_)
          .Actual(count);
    }
    for (uint32_t i = 0; i < count; ++i) {
      node.ents.push_back({GetBE32(buf.data(), kEnts + i * 8),
                           GetBE32(buf.data(), kEnts + i * 8 + 4)});
    }
    for (auto it = node.ents.rbegin(); it != node.ents.rend(); ++it) {
      stack.push_back({it->before, static_cast<uint16_t>(node.level - 1)});
    }
    node_blocks_[dablk] = std::move(node);
  }
  if (node_blocks_.empty() && leaf_blocks_.size() != 1) {
    return XFS_ERR(XfsError::kCorruptDirectory,
                   "leafn root without a node tree is not alone")
        .Ino(ino_);
  }
  // Free blocks and their bests tables.
  const uint64_t free_first = d::kFreeOffset >> block_log_;
  for (const XfsExtent& e : fork_->extents()) {
    const uint64_t b0 = std::max<uint64_t>(e.startoff, free_first);
    const uint64_t b1 = e.EndBlock();
    for (uint64_t fb = b0; fb < b1; ++fb) {
      std::vector<uint8_t> buf;
      Status s = ReadDirBlock(fb, &buf);
      if (!s.ok()) return s;
      if (GetBE32(buf.data(), kBlkMagic) != ondisk::kDir3FreeMagic) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "free block magic mismatch")
            .Ino(ino_)
            .DirBlock(fb);
      }
      FreeBlock fb_block;
      fb_block.dablk = static_cast<uint32_t>(fb);
      const uint32_t firstdb = GetBE32(buf.data(), kFreeFirstdb);
      const uint32_t nvalid = GetBE32(buf.data(), kFreeNvalid);
      if (nvalid > free_max_bests_ ||
          (fb != free_first + firstdb / free_max_bests_)) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "free block range mismatch")
            .Ino(ino_)
            .Actual(firstdb);
      }
      for (uint32_t i = 0; i < nvalid; ++i) {
        fb_block.bests.push_back(GetBE16(buf.data(), kEnts + i * 2));
      }
      free_blocks_[fb_block.dablk] = std::move(fb_block);
    }
  }
  // Data blocks and their bests.
  const uint32_t bestcount = static_cast<uint32_t>(data_size_ >> block_log_);
  for (uint32_t db = 0; db < bestcount; ++db) {
    uint16_t best = kNullDataOff;
    const uint32_t fdb = db / free_max_bests_;
    const uint32_t findex = db % free_max_bests_;
    auto fit = free_blocks_.find(static_cast<uint32_t>(free_first + fdb));
    if (fit != free_blocks_.end() && findex < fit->second.bests.size()) {
      best = fit->second.bests[findex];
    }
    if (FsbForFileBlock(db) == kNullFsb) {
      if (best != kNullDataOff) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "free block references a missing data block")
            .Ino(ino_)
            .DirBlock(db);
      }
      continue;
    }
    std::vector<uint8_t> block;
    Status s = ReadDirBlock(db, &block);
    if (!s.ok()) return s;
    data_blocks_[db] = std::move(block);
    bests_[db] = best == kNullDataOff ? DataBestFree(data_blocks_[db]) : best;
  }
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Shortform / block helpers
// ---------------------------------------------------------------------------

bool DirectoryEditor::SfWide(uint64_t extra_ino) const {
  if (sf_parent_ > 0xffffffffull || extra_ino > 0xffffffffull) return true;
  for (const PlacedEntry& p : placed_) {
    if (p.e.ino > 0xffffffffull) return true;
  }
  return false;
}

size_t DirectoryEditor::SfHeaderSizeFor(bool wide) const {
  return wide ? d::kSfHdrSizeFixed : 6;
}

size_t DirectoryEditor::SfEntrySizeFor(size_t namelen, bool wide) const {
  return 3 + namelen + (geo_->features.ftype ? 1 : 0) + (wide ? 8 : 4);
}

size_t DirectoryEditor::DataEntrySize(size_t namelen) const {
  return d::DataEntrySize(namelen, geo_->features.ftype);
}

uint32_t DirectoryEditor::PlaceEntry(size_t entsize, uint32_t first_offset,
                                     bool shortform, bool wide) const {
  (void)shortform;
  (void)wide;
  // The stored offset is the entry's *data-space* position: it always advances
  // by the data-entry size (xfs_dir2_sf_addname_pick), even while the
  // directory is stored shortform. That is what makes the later conversion to
  // the block format a pure copy.
  uint32_t cursor = first_offset;
  for (const PlacedEntry& p : placed_) {
    if (cursor + entsize <= p.offset) return cursor;
    cursor = std::max<uint32_t>(
        cursor, p.offset + static_cast<uint32_t>(DataEntrySize(p.e.name.size())));
  }
  return cursor;
}

Status DirectoryEditor::InsertPlaced(const DirEntryWrite& entry,
                                     uint32_t first_offset, bool shortform) {
  const bool wide = SfWide(entry.ino);
  const size_t entsize =
      shortform ? SfEntrySizeFor(entry.name.size(), wide)
                : DataEntrySize(entry.name.size());
  const uint32_t offset = PlaceEntry(entsize, first_offset, shortform, wide);
  if (offset > 0xffff) {
    return XFS_ERR(XfsError::kNoSpace, "directory offset out of range")
        .Ino(ino_)
        .Metadata("directory");
  }
  size_t index = placed_.size();
  for (size_t i = 0; i < placed_.size(); ++i) {
    if (placed_[i].offset > offset) {
      index = i;
      break;
    }
  }
  uint32_t end = 0;
  for (const PlacedEntry& p : placed_) {
    end = std::max(end, p.offset + static_cast<uint32_t>(
                                   DataEntrySize(p.e.name.size())));
  }
  end = std::max(end, offset + static_cast<uint32_t>(
                                  DataEntrySize(entry.name.size())));
  if (shortform) {
    size_t image = SfHeaderSizeFor(wide);
    for (size_t i = 0; i < placed_.size(); ++i) {
      if (i == index) image += SfEntrySizeFor(entry.name.size(), wide);
      image += SfEntrySizeFor(placed_[i].e.name.size(), wide);
    }
    if (index == placed_.size()) {
      image += SfEntrySizeFor(entry.name.size(), wide);
    }
    const size_t capacity =
        static_cast<size_t>(geo_->inode_size) - di::kCoreSizeV3;
    if (image > capacity) {
      return XFS_ERR(XfsError::kNoSpace, "does not fit shortform")
          .Ino(ino_)
          .Metadata("directory");
    }
    // xfs_dir2_sf_verify also requires the directory to stay convertible to
    // the block format inside one block.
    if (static_cast<size_t>(end) + (placed_.size() + 1 + 2) * 8 + 8 >
        dir_block_size_) {
      return XFS_ERR(XfsError::kNoSpace, "shortform offsets do not fit")
          .Ino(ino_)
          .Metadata("directory");
    }
    placed_.insert(placed_.begin() + static_cast<ptrdiff_t>(index),
                   PlacedEntry{entry, offset});
    data_size_ = image;
    dirty_ = true;
    return OkStatus();
  }
  if (static_cast<size_t>(end) + (placed_.size() + 1 + 2) * 8 + 8 >
      dir_block_size_) {
    return XFS_ERR(XfsError::kNoSpace, "block directory is full")
        .Ino(ino_)
        .Metadata("directory");
  }
  placed_.insert(placed_.begin() + static_cast<ptrdiff_t>(index),
                 PlacedEntry{entry, offset});
  block_dirty_ = true;
  dirty_ = true;
  return OkStatus();
}

Status DirectoryEditor::RemovePlaced(std::string_view name,
                                     XfsDirEntry* removed) {
  for (size_t i = 0; i < placed_.size(); ++i) {
    if (placed_[i].e.name.size() != name.size() ||
        std::memcmp(placed_[i].e.name.data(), name.data(), name.size()) != 0) {
      continue;
    }
    removed->name = placed_[i].e.name;
    removed->ino = placed_[i].e.ino;
    removed->ftype = placed_[i].e.ftype;
    placed_.erase(placed_.begin() + static_cast<ptrdiff_t>(i));
    if (fmt_ == XfsDirFormat::kShortform) {
      const bool wide = SfWide(0);
      size_t image = SfHeaderSizeFor(wide);
      for (const PlacedEntry& p : placed_) {
        image += SfEntrySizeFor(p.e.name.size(), wide);
      }
      data_size_ = image;
    } else {
      block_dirty_ = true;
    }
    dirty_ = true;
    return OkStatus();
  }
  return XFS_ERR(XfsError::kNotFound, "directory entry not found").Ino(ino_);
}

namespace {

// One record in the ordered data area of the shortform/block image builder.
struct ImageRec {
  uint32_t offset;
  std::string name;
  uint64_t ino;
  uint8_t ftype;
};

void LayOutDataArea(const std::vector<ImageRec>& input, uint32_t end,
                    uint8_t* out) {
  std::vector<ImageRec> recs = input;
  std::sort(recs.begin(), recs.end(),
            [](const ImageRec& a, const ImageRec& b) {
              return a.offset < b.offset;
            });
  uint32_t cursor = ondisk::dir::kData3HeaderSize;
  for (const ImageRec& r : recs) {
    if (r.offset > cursor) {
      const uint32_t len = r.offset - cursor;
      PutBE16(out, cursor, ondisk::dir::kDataFreeTag);
      PutBE16(out, cursor + 2, static_cast<uint16_t>(len));
      PutBE16(out, r.offset - 2, static_cast<uint16_t>(cursor));
    }
    const size_t entsize =
        ondisk::dir::DataEntrySize(r.name.size(), true);
    uint8_t* b = out + r.offset;
    PutBE64(b, 0, r.ino);
    b[8] = static_cast<uint8_t>(r.name.size());
    std::memcpy(b + 9, r.name.data(), r.name.size());
    b[9 + r.name.size()] = r.ftype;
    PutBE16(b, entsize - 2, static_cast<uint16_t>(r.offset));
    cursor = r.offset + static_cast<uint32_t>(entsize);
  }
  if (cursor < end) {
    const uint32_t len = end - cursor;
    PutBE16(out, cursor, ondisk::dir::kDataFreeTag);
    PutBE16(out, cursor + 2, static_cast<uint16_t>(len));
    PutBE16(out, end - 2, static_cast<uint16_t>(cursor));
  }
}

}  // namespace

void DirectoryEditor::BuildShortformImage(std::vector<uint8_t>* image) const {
  const bool wide = SfWide(0);
  image->assign(SfHeaderSizeFor(wide), 0);
  (*image)[d::kSfHdrCount] = static_cast<uint8_t>(placed_.size());
  uint32_t i8count = sf_parent_ > 0xffffffffull ? 1 : 0;
  for (const PlacedEntry& p : placed_) {
    if (p.e.ino > 0xffffffffull) ++i8count;
  }
  (*image)[d::kSfHdrI8Count] = static_cast<uint8_t>(i8count);
  if (wide) {
    PutBE64(image->data(), d::kSfHdrParent, sf_parent_);
  } else {
    PutBE32(image->data(), d::kSfHdrParent, static_cast<uint32_t>(sf_parent_));
  }
  for (const PlacedEntry& p : placed_) {
    const size_t off = image->size();
    const size_t entsize = SfEntrySizeFor(p.e.name.size(), wide);
    image->resize(off + entsize, 0);
    uint8_t* b = image->data() + off;
    b[d::kSfEntNamelen] = static_cast<uint8_t>(p.e.name.size());
    PutBE16(b, d::kSfEntOffset, static_cast<uint16_t>(p.offset));
    std::memcpy(b + d::kSfEntName, p.e.name.data(), p.e.name.size());
    size_t pos = d::kSfEntName + p.e.name.size();
    if (geo_->features.ftype) b[pos++] = p.e.ftype;
    if (wide) {
      PutBE64(b, pos, p.e.ino);
    } else {
      PutBE32(b, pos, static_cast<uint32_t>(p.e.ino));
    }
  }
}

void DirectoryEditor::BuildBlockImage(std::vector<uint8_t>* image) const {
  image->assign(dir_block_size_, 0);
  PutBE32(image->data(), kBlkMagic, ondisk::kDir3BlockMagic);
  const uint32_t count = static_cast<uint32_t>(placed_.size()) + 2;
  const uint32_t leaf_start = dir_block_size_ - kBlockTail - count * 8;
  std::vector<ImageRec> recs;
  recs.push_back({d::kData3HeaderSize, ".", ino_, d::kFtDir});
  recs.push_back({d::kData3HeaderSize + static_cast<uint32_t>(
                                         DataEntrySize(1)),
                  "..", sf_parent_, d::kFtDir});
  for (const PlacedEntry& p : placed_) {
    recs.push_back({p.offset, p.e.name, p.e.ino, p.e.ftype});
  }
  // Lay out only the data area; the free record at the end must stop at the
  // leaf array.
  {
    std::vector<ImageRec> sorted = recs;
    std::sort(sorted.begin(), sorted.end(),
              [](const ImageRec& a, const ImageRec& b) {
                return a.offset < b.offset;
              });
    uint32_t cursor = d::kData3HeaderSize;
    for (const ImageRec& r : sorted) {
      if (r.offset > cursor) {
        const uint32_t len = r.offset - cursor;
        PutBE16(image->data(), cursor, d::kDataFreeTag);
        PutBE16(image->data(), cursor + 2, static_cast<uint16_t>(len));
        PutBE16(image->data(), r.offset - 2, static_cast<uint16_t>(cursor));
      }
      const size_t entsize = DataEntrySize(r.name.size());
      uint8_t* b = image->data() + r.offset;
      PutBE64(b, 0, r.ino);
      b[8] = static_cast<uint8_t>(r.name.size());
      std::memcpy(b + 9, r.name.data(), r.name.size());
      if (geo_->features.ftype) b[9 + r.name.size()] = r.ftype;
      PutBE16(b, entsize - 2, static_cast<uint16_t>(r.offset));
      cursor = r.offset + static_cast<uint32_t>(entsize);
    }
    if (cursor < leaf_start) {
      const uint32_t len = leaf_start - cursor;
      PutBE16(image->data(), cursor, d::kDataFreeTag);
      PutBE16(image->data(), cursor + 2, static_cast<uint16_t>(len));
      PutBE16(image->data(), leaf_start - 2, static_cast<uint16_t>(cursor));
    }
  }
  struct LeafOut {
    uint32_t hash;
    uint32_t addr;
  };
  std::vector<LeafOut> ents;
  for (const ImageRec& r : recs) ents.push_back({DirHash(r.name), r.offset >> 3});
  std::sort(ents.begin(), ents.end(),
            [](const LeafOut& a, const LeafOut& b) { return a.hash < b.hash; });
  for (size_t i = 0; i < ents.size(); ++i) {
    PutBE32(image->data(), leaf_start + static_cast<uint32_t>(i) * 8,
            ents[i].hash);
    PutBE32(image->data(), leaf_start + static_cast<uint32_t>(i) * 8 + 4,
            ents[i].addr);
  }
  PutBE32(image->data(), dir_block_size_ - 8, count);
  PutBE32(image->data(), dir_block_size_ - 4, 0);
  DataScanBestfree(image, leaf_start);
}

void DirectoryEditor::BuildDataBlockFromPlaced(std::vector<uint8_t>* image,
                                               uint64_t fsb) const {
  image->assign(dir_block_size_, 0);
  uint8_t* b = image->data();
  PutBE32(b, kBlkMagic, ondisk::kDir3DataMagic);
  PutBE64(b, kBlkBlkno, geo_->FsbToByte(fsb) / 512);
  std::memcpy(b + kBlkUuid, geo_->meta_uuid.data(), 16);
  PutBE64(b, kBlkOwner, ino_);
  std::vector<ImageRec> recs;
  recs.push_back({d::kData3HeaderSize, ".", ino_, d::kFtDir});
  recs.push_back({d::kData3HeaderSize + static_cast<uint32_t>(
                                         DataEntrySize(1)),
                  "..", sf_parent_, d::kFtDir});
  for (const PlacedEntry& p : placed_) {
    recs.push_back({p.offset, p.e.name, p.e.ino, p.e.ftype});
  }
  // Build the data area exactly as BuildBlockImage, but with the full block
  // as the end.
  std::vector<ImageRec> sorted = recs;
  std::sort(sorted.begin(), sorted.end(),
            [](const ImageRec& a, const ImageRec& b) {
              return a.offset < b.offset;
            });
  uint32_t cursor = d::kData3HeaderSize;
  for (const ImageRec& r : sorted) {
    if (r.offset > cursor) {
      const uint32_t len = r.offset - cursor;
      PutBE16(b, cursor, d::kDataFreeTag);
      PutBE16(b, cursor + 2, static_cast<uint16_t>(len));
      PutBE16(b, r.offset - 2, static_cast<uint16_t>(cursor));
    }
    const size_t entsize = DataEntrySize(r.name.size());
    uint8_t* e = b + r.offset;
    PutBE64(e, 0, r.ino);
    e[8] = static_cast<uint8_t>(r.name.size());
    std::memcpy(e + 9, r.name.data(), r.name.size());
    if (geo_->features.ftype) e[9 + r.name.size()] = r.ftype;
    PutBE16(e, entsize - 2, static_cast<uint16_t>(r.offset));
    cursor = r.offset + static_cast<uint32_t>(entsize);
  }
  if (cursor < dir_block_size_) {
    const uint32_t len = dir_block_size_ - cursor;
    PutBE16(b, cursor, d::kDataFreeTag);
    PutBE16(b, cursor + 2, static_cast<uint16_t>(len));
    PutBE16(b, dir_block_size_ - 2, static_cast<uint16_t>(cursor));
  }
  DataScanBestfree(image, dir_block_size_);
}

void DirectoryEditor::BuildLeafFromPlaced(LeafBlock* leaf) const {
  struct HashEnt {
    uint32_t hash;
    uint32_t addr;
  };
  std::vector<HashEnt> ents;
  ents.push_back({DirHash("."), d::kData3HeaderSize >> 3});
  ents.push_back({DirHash(".."),
                  static_cast<uint32_t>((d::kData3HeaderSize +
                                         DataEntrySize(1)) >> 3)});
  for (const PlacedEntry& p : placed_) {
    ents.push_back({DirHash(p.e.name), p.offset >> 3});
  }
  std::sort(ents.begin(), ents.end(),
            [](const HashEnt& a, const HashEnt& b) { return a.hash < b.hash; });
  leaf->ents.clear();
  for (const HashEnt& e : ents) leaf->ents.push_back({e.hash, e.addr});
}

// ---------------------------------------------------------------------------
// Data block primitives
// ---------------------------------------------------------------------------

uint32_t DirectoryEditor::DataEnd(const std::vector<uint8_t>& buf) const {
  const uint32_t magic = GetBE32(buf.data(), kBlkMagic);
  if (magic == ondisk::kDir3BlockMagic) {
    const uint32_t count = GetBE32(buf.data(), dir_block_size_ - 8);
    return dir_block_size_ - kBlockTail - count * 8;
  }
  return dir_block_size_;
}

void DirectoryEditor::InitDataBlock(std::vector<uint8_t>* buf,
                                    uint64_t blkno_daddr,
                                    uint64_t owner) const {
  buf->assign(dir_block_size_, 0);
  uint8_t* b = buf->data();
  PutBE32(b, kBlkMagic, ondisk::kDir3DataMagic);
  PutBE64(b, kBlkBlkno, blkno_daddr);
  std::memcpy(b + kBlkUuid, geo_->meta_uuid.data(), 16);
  PutBE64(b, kBlkOwner, owner);
  const uint32_t body = dir_block_size_ - d::kData3HeaderSize;
  PutBE16(b, kBlkBestFree + 0, d::kData3HeaderSize);
  PutBE16(b, kBlkBestFree + 2, static_cast<uint16_t>(body));
  PutBE16(b, d::kData3HeaderSize, d::kDataFreeTag);
  PutBE16(b, d::kData3HeaderSize + 2, static_cast<uint16_t>(body));
  PutBE16(b, d::kData3HeaderSize + body - 2,
          static_cast<uint16_t>(d::kData3HeaderSize));
}

void DirectoryEditor::InitLeafBlock(std::vector<uint8_t>* buf,
                                    uint64_t blkno_daddr, uint64_t owner,
                                    uint16_t magic) const {
  buf->assign(dir_block_size_, 0);
  uint8_t* b = buf->data();
  PutBE16(b, kInfoMagic, magic);
  PutBE64(b, kInfoBlkno, blkno_daddr);
  std::memcpy(b + kInfoUuid, geo_->meta_uuid.data(), 16);
  PutBE64(b, kInfoOwner, owner);
}

void DirectoryEditor::InitFreeBlock(std::vector<uint8_t>* buf,
                                    uint64_t blkno_daddr,
                                    uint64_t owner) const {
  buf->assign(dir_block_size_, 0);
  uint8_t* b = buf->data();
  PutBE32(b, kBlkMagic, ondisk::kDir3FreeMagic);
  PutBE64(b, kBlkBlkno, blkno_daddr);
  std::memcpy(b + kBlkUuid, geo_->meta_uuid.data(), 16);
  PutBE64(b, kBlkOwner, owner);
}

void DirectoryEditor::InitNodeBlock(std::vector<uint8_t>* buf,
                                    uint64_t blkno_daddr, uint64_t owner,
                                    uint16_t level) const {
  buf->assign(dir_block_size_, 0);
  uint8_t* b = buf->data();
  PutBE16(b, kInfoMagic, ondisk::kDaNode3Magic);
  PutBE64(b, kInfoBlkno, blkno_daddr);
  std::memcpy(b + kInfoUuid, geo_->meta_uuid.data(), 16);
  PutBE64(b, kInfoOwner, owner);
  PutBE16(b, kNodeLevel, level);
}

Status DirectoryEditor::DataUseFree(std::vector<uint8_t>* buf, uint32_t end,
                                    uint32_t off, uint32_t len) {
  uint8_t* b = buf->data();
  uint32_t cur = d::kData3HeaderSize;
  while (cur < end) {
    const uint16_t tag = GetBE16(b, cur);
    if (tag == d::kDataFreeTag) {
      const uint32_t length = GetBE16(b, cur + 2);
      if (cur <= off && off + len <= cur + length) {
        const uint32_t front = off - cur;
        const uint32_t back = cur + length - (off + len);
        if (front == 0 && back == 0) return OkStatus();
        if (front == 0) {
          PutBE16(b, off + len, d::kDataFreeTag);
          PutBE16(b, off + len + 2, static_cast<uint16_t>(back));
          PutBE16(b, off + len + back - 2, static_cast<uint16_t>(off + len));
          return OkStatus();
        }
        if (back == 0) {
          PutBE16(b, cur + 2, static_cast<uint16_t>(front));
          PutBE16(b, cur + front - 2, static_cast<uint16_t>(cur));
          return OkStatus();
        }
        PutBE16(b, cur + 2, static_cast<uint16_t>(front));
        PutBE16(b, cur + front - 2, static_cast<uint16_t>(cur));
        PutBE16(b, off + len, d::kDataFreeTag);
        PutBE16(b, off + len + 2, static_cast<uint16_t>(back));
        PutBE16(b, off + len + back - 2, static_cast<uint16_t>(off + len));
        return OkStatus();
      }
      cur += length;
      continue;
    }
    const uint8_t namelen = GetU8(b, cur + 8);
    cur += static_cast<uint32_t>(DataEntrySize(namelen));
  }
  return XFS_ERR(XfsError::kCorruptDirectory,
                 "no free space record covers the new entry")
      .Ino(ino_)
      .Actual(off);
}

Status DirectoryEditor::DataMakeFree(std::vector<uint8_t>* buf, uint32_t end,
                                     uint32_t off, uint32_t len) {
  uint8_t* b = buf->data();
  bool prev_free = false;
  uint32_t prev_off = 0;
  if (off > d::kData3HeaderSize) {
    const uint16_t prev_tag = GetBE16(b, off - 2);
    if (prev_tag < off && prev_tag >= d::kData3HeaderSize &&
        GetBE16(b, prev_tag) == d::kDataFreeTag) {
      prev_free = true;
      prev_off = prev_tag;
    }
  }
  bool post_free = off + len < end && GetBE16(b, off + len) == d::kDataFreeTag;
  if (prev_free && post_free) {
    const uint32_t prev_len = GetBE16(b, prev_off + 2);
    const uint32_t post_len = GetBE16(b, off + len + 2);
    const uint32_t total = prev_len + len + post_len;
    PutBE16(b, prev_off + 2, static_cast<uint16_t>(total));
    PutBE16(b, prev_off + total - 2, static_cast<uint16_t>(prev_off));
  } else if (prev_free) {
    const uint32_t prev_len = GetBE16(b, prev_off + 2);
    const uint32_t total = prev_len + len;
    PutBE16(b, prev_off + 2, static_cast<uint16_t>(total));
    PutBE16(b, prev_off + total - 2, static_cast<uint16_t>(prev_off));
  } else if (post_free) {
    const uint32_t post_len = GetBE16(b, off + len + 2);
    PutBE16(b, off, d::kDataFreeTag);
    PutBE16(b, off + 2, static_cast<uint16_t>(len + post_len));
    PutBE16(b, off + len + post_len - 2, static_cast<uint16_t>(off));
  } else {
    PutBE16(b, off, d::kDataFreeTag);
    PutBE16(b, off + 2, static_cast<uint16_t>(len));
    PutBE16(b, off + len - 2, static_cast<uint16_t>(off));
  }
  return OkStatus();
}

void DirectoryEditor::DataScanBestfree(std::vector<uint8_t>* buf,
                                       uint32_t end) const {
  uint8_t* b = buf->data();
  uint16_t slots[3][2] = {{0, 0}, {0, 0}, {0, 0}};
  uint32_t cur = d::kData3HeaderSize;
  while (cur < end) {
    const uint16_t tag = GetBE16(b, cur);
    if (tag == d::kDataFreeTag) {
      const uint16_t length = GetBE16(b, cur + 2);
      if (length > slots[0][1]) {
        slots[2][0] = slots[1][0];
        slots[2][1] = slots[1][1];
        slots[1][0] = slots[0][0];
        slots[1][1] = slots[0][1];
        slots[0][0] = static_cast<uint16_t>(cur);
        slots[0][1] = length;
      } else if (length > slots[1][1]) {
        slots[2][0] = slots[1][0];
        slots[2][1] = slots[1][1];
        slots[1][0] = static_cast<uint16_t>(cur);
        slots[1][1] = length;
      } else if (length > slots[2][1]) {
        slots[2][0] = static_cast<uint16_t>(cur);
        slots[2][1] = length;
      }
      cur += length;
      continue;
    }
    const uint8_t namelen = GetU8(b, cur + 8);
    cur += static_cast<uint32_t>(DataEntrySize(namelen));
  }
  PutBE16(b, kBlkBestFree + 0, slots[0][0]);
  PutBE16(b, kBlkBestFree + 2, slots[0][1]);
  PutBE16(b, kBlkBestFree + 4, slots[1][0]);
  PutBE16(b, kBlkBestFree + 6, slots[1][1]);
  PutBE16(b, kBlkBestFree + 8, slots[2][0]);
  PutBE16(b, kBlkBestFree + 10, slots[2][1]);
}

uint16_t DirectoryEditor::DataBestFree(const std::vector<uint8_t>& buf) const {
  return GetBE16(buf.data(), kBlkBestFree + 2);
}

Status DirectoryEditor::DataInsertEntry(std::vector<uint8_t>* buf, uint32_t end,
                                        const DirEntryWrite& entry,
                                        uint32_t* off_out) {
  const size_t entsize = DataEntrySize(entry.name.size());
  uint8_t* b = buf->data();
  uint32_t cur = d::kData3HeaderSize;
  bool found = false;
  while (cur < end) {
    const uint16_t tag = GetBE16(b, cur);
    if (tag == d::kDataFreeTag) {
      const uint32_t length = GetBE16(b, cur + 2);
      if (length >= entsize) {
        found = true;
        break;
      }
      cur += length;
      continue;
    }
    const uint8_t namelen = GetU8(b, cur + 8);
    cur += static_cast<uint32_t>(DataEntrySize(namelen));
  }
  if (!found) {
    return XFS_ERR(XfsError::kNoSpace, "no space in the data block")
        .Ino(ino_)
        .Metadata("directory");
  }
  Status s = DataUseFree(buf, end, cur, static_cast<uint32_t>(entsize));
  if (!s.ok()) return s;
  PutBE64(b, cur, entry.ino);
  b[cur + 8] = static_cast<uint8_t>(entry.name.size());
  std::memcpy(b + cur + 9, entry.name.data(), entry.name.size());
  if (geo_->features.ftype) b[cur + 9 + entry.name.size()] = entry.ftype;
  PutBE16(b, cur + entsize - 2, static_cast<uint16_t>(cur));
  DataScanBestfree(buf, end);
  *off_out = cur;
  return OkStatus();
}

Status DirectoryEditor::DataRemoveEntry(std::vector<uint8_t>* buf, uint32_t end,
                                        uint32_t off, bool* empty_out) {
  uint8_t* b = buf->data();
  if (off + 9 > end) {
    return XFS_ERR(XfsError::kCorruptDirectory, "entry offset out of range")
        .Ino(ino_)
        .Actual(off);
  }
  const uint8_t namelen = GetU8(b, off + 8);
  const size_t entsize = DataEntrySize(namelen);
  if (off + entsize > end) {
    return XFS_ERR(XfsError::kCorruptDirectory, "entry extends past the area")
        .Ino(ino_)
        .Actual(off);
  }
  Status s = DataMakeFree(buf, end, off, static_cast<uint32_t>(entsize));
  if (!s.ok()) return s;
  DataScanBestfree(buf, end);
  *empty_out = DataBestFree(*buf) == dir_block_size_ - d::kData3HeaderSize;
  return OkStatus();
}

uint16_t DirectoryEditor::BestsFor(uint32_t db) const {
  auto it = bests_.find(db);
  return it == bests_.end() ? kNullDataOff : it->second;
}

// ---------------------------------------------------------------------------
// Data block growth / shrink (xfs_dir2_grow_inode, xfs_dir2_shrink_inode)
// ---------------------------------------------------------------------------

Status DirectoryEditor::GrowDataBlock(uint32_t* db_out) {
  uint32_t db = 0;
  while (data_blocks_.count(db)) ++db;
  const uint32_t bestcount = static_cast<uint32_t>(data_size_ >> block_log_);
  const bool at_end = db >= bestcount;
  auto alloc = AllocDirBlock(db);
  if (!alloc.ok()) return alloc.error();
  std::vector<uint8_t> image;
  InitDataBlock(&image, geo_->FsbToByte(alloc.value()) / 512, ino_);
  data_blocks_[db] = std::move(image);
  bests_[db] = static_cast<uint16_t>(dir_block_size_ - d::kData3HeaderSize);
  data_dirty_.insert(db);
  if (at_end) data_size_ = static_cast<uint64_t>(db + 1) << block_log_;
  dirty_ = true;
  *db_out = db;
  return OkStatus();
}

Status DirectoryEditor::DropDataBlockIfEmpty(uint32_t db, bool empty) {
  if (!empty) {
    auto it = data_blocks_.find(db);
    if (it != data_blocks_.end()) bests_[db] = DataBestFree(it->second);
    return OkStatus();
  }
  if (db == 0) {
    auto it = data_blocks_.find(db);
    if (it != data_blocks_.end()) bests_[db] = DataBestFree(it->second);
    return OkStatus();
  }
  const uint32_t bestcount = static_cast<uint32_t>(data_size_ >> block_log_);
  if (db + 1 != bestcount) {
    bests_[db] = static_cast<uint16_t>(dir_block_size_ - d::kData3HeaderSize);
    return OkStatus();
  }
  Status s = FreeDirBlock(db);
  if (!s.ok()) return s;
  data_blocks_.erase(db);
  bests_.erase(db);
  data_dirty_.erase(db);
  uint32_t last = 0;
  for (const auto& entry : data_blocks_) last = std::max(last, entry.first);
  data_size_ = static_cast<uint64_t>(last + 1) << block_log_;
  dirty_ = true;
  return OkStatus();
}

Result<uint32_t> DirectoryEditor::PickDataBlock(uint32_t hash, size_t need) {
  LeafBlock* leaf = (fmt_ == XfsDirFormat::kLeaf)
                        ? &leaf_blocks_[root_dablk_]
                        : LeafForHash(hash);
  if (leaf != nullptr) {
    size_t index = 0;
    while (index < leaf->ents.size() && leaf->ents[index].hash < hash) ++index;
    for (; index < leaf->ents.size() && leaf->ents[index].hash == hash;
         ++index) {
      const uint32_t addr = leaf->ents[index].addr;
      if (addr == 0) continue;
      const uint32_t db = addr >> (block_log_ - 3);
      if (data_blocks_.count(db) && BestsFor(db) >= need) return db;
    }
  }
  const uint32_t bestcount = static_cast<uint32_t>(data_size_ >> block_log_);
  for (uint32_t db = 0; db < bestcount; ++db) {
    if (!data_blocks_.count(db)) continue;
    if (BestsFor(db) >= need) return db;
  }
  return XFS_ERR(XfsError::kNotFound, "every directory data block is full")
      .Ino(ino_)
      .Metadata("directory");
}

// ---------------------------------------------------------------------------
// Block allocation through the fork editor
// ---------------------------------------------------------------------------

Result<uint64_t> DirectoryEditor::AllocDirBlock(uint64_t file_block) {
  Allocation allocation;
  Status s = alloc_->Allocate(geo_->InoAgno(ino_), 1, &allocation);
  if (!s.ok()) return s.error();
  XfsExtent extent;
  extent.startoff = file_block;
  extent.startblock = allocation.fsb;
  extent.blockcount = 1;
  extent.unwritten = false;
  s = fork_->InsertExtent(extent);
  if (!s.ok()) return s.error();
  fork_->AddNblocks(1);
  return allocation.fsb;
}

Status DirectoryEditor::FreeDirBlock(uint64_t file_block) {
  fork_->SetFreeRemovedBlocks(true);
  return fork_->RemoveRange(file_block, file_block + 1);
}

// ---------------------------------------------------------------------------
// Leaf format
// ---------------------------------------------------------------------------

void DirectoryEditor::CompactLeaf(LeafBlock* leaf) {
  std::vector<LeafEnt> live;
  live.reserve(leaf->ents.size());
  for (const LeafEnt& e : leaf->ents) {
    if (e.addr != 0) live.push_back(e);
  }
  leaf->ents = std::move(live);
  leaf->dirty = true;
}

Status DirectoryEditor::AddLeafEntry(LeafBlock* leaf, size_t index,
                                     uint32_t hash, uint32_t addr) {
  if (leaf->ents.size() >= leaf_max_ents_) {
    CompactLeaf(leaf);
    index = 0;
    while (index < leaf->ents.size() && leaf->ents[index].hash <= hash) {
      ++index;
    }
    if (leaf->ents.size() >= leaf_max_ents_) {
      return XFS_ERR(XfsError::kNoSpace, "leaf block is full")
          .Ino(ino_)
          .Metadata("directory");
    }
  }
  leaf->ents.insert(leaf->ents.begin() + static_cast<ptrdiff_t>(index),
                    LeafEnt{hash, addr});
  leaf->dirty = true;
  return OkStatus();
}

Status DirectoryEditor::InsertLeaf(const DirEntryWrite& entry) {
  const uint32_t hash = DirHash(entry.name);
  const size_t need = DataEntrySize(entry.name.size());
  LeafBlock* leaf = &leaf_blocks_[root_dablk_];

  uint32_t db = 0;
  auto picked = PickDataBlock(hash, need);
  if (picked.ok()) {
    db = picked.value();
  } else {
    Status s = GrowDataBlock(&db);
    if (!s.ok()) return s;
  }
  // Reserve a leaf slot before changing the data block.
  if (leaf->ents.size() >= leaf_max_ents_) {
    CompactLeaf(leaf);
    if (leaf->ents.size() >= leaf_max_ents_) {
      return XFS_ERR(XfsError::kNoSpace, "leaf1 block is full")
          .Ino(ino_)
          .Metadata("directory");
    }
  }
  // The leaf1 block holds the entries *and* the bests array plus its tail:
  // when the two meet, convert to the node format (xfs_dir2_leaf_addname's
  // "needbytes" test).
  const size_t bestcount = static_cast<size_t>(data_size_ >> block_log_);
  if (kEnts + (leaf->ents.size() + 1) * 8 + bestcount * 2 + 4 >
      dir_block_size_) {
    return XFS_ERR(XfsError::kNoSpace, "leaf1 entries and bests are full")
        .Ino(ino_)
        .Metadata("directory");
  }
  size_t index = 0;
  while (index < leaf->ents.size() && leaf->ents[index].hash <= hash) ++index;
  const uint32_t end = DataEnd(data_blocks_[db]);
  uint32_t off = 0;
  Status s = DataInsertEntry(&data_blocks_[db], end, entry, &off);
  if (!s.ok()) return s;
  data_dirty_.insert(db);
  bests_[db] = DataBestFree(data_blocks_[db]);
  const uint32_t addr =
      (db << (block_log_ - 3)) | (off >> 3);
  s = AddLeafEntry(leaf, index, hash, addr);
  if (!s.ok()) return s;
  dirty_ = true;
  return OkStatus();
}

Status DirectoryEditor::LeafToNode() {
  auto leaf_it = leaf_blocks_.find(root_dablk_);
  if (leaf_it == leaf_blocks_.end()) {
    return XFS_ERR(XfsError::kInternal, "leaf1 block missing")
        .Ino(ino_)
        .Metadata("directory");
  }
  fmt_ = XfsDirFormat::kNode;
  Status s = RebuildFreeBlocks();
  if (!s.ok()) return s;
  leaf_it->second.dirty = true;
  dirty_ = true;
  return OkStatus();
}

Status DirectoryEditor::RemoveLeaf(std::string_view name,
                                   XfsDirEntry* removed) {
  const uint32_t hash = DirHash(name);
  LeafBlock* leaf = &leaf_blocks_[root_dablk_];
  size_t index = 0;
  while (index < leaf->ents.size() && leaf->ents[index].hash < hash) ++index;
  for (; index < leaf->ents.size() && leaf->ents[index].hash == hash;
       ++index) {
    const uint32_t addr = leaf->ents[index].addr;
    if (addr == 0) continue;
    const uint32_t db = addr >> (block_log_ - 3);
    const uint32_t off = (addr & ((1u << (block_log_ - 3)) - 1)) << 3;
    auto block_it = data_blocks_.find(db);
    if (block_it == data_blocks_.end()) continue;
    const uint8_t namelen = GetU8(block_it->second.data(), off + 8);
    if (namelen != name.size() ||
        std::memcmp(block_it->second.data() + off + 9, name.data(),
                    name.size()) != 0) {
      continue;
    }
    removed->name = std::string(name);
    removed->ino = GetBE64(block_it->second.data(), off);
    removed->ftype = geo_->features.ftype
                         ? GetU8(block_it->second.data(), off + 9 + namelen)
                         : 0;
    bool empty = false;
    Status s = DataRemoveEntry(&block_it->second,
                               DataEnd(block_it->second), off, &empty);
    if (!s.ok()) return s;
    data_dirty_.insert(db);
    leaf->ents[index].addr = 0;
    leaf->dirty = true;
    s = DropDataBlockIfEmpty(db, empty);
    if (!s.ok()) return s;
    if (data_blocks_.count(db)) bests_[db] = DataBestFree(data_blocks_[db]);
    dirty_ = true;
    return OkStatus();
  }
  return XFS_ERR(XfsError::kNotFound, "directory entry not found").Ino(ino_);
}

// ---------------------------------------------------------------------------
// Node format
// ---------------------------------------------------------------------------

DirectoryEditor::LeafBlock* DirectoryEditor::LeafForHash(uint32_t hash) {
  if (node_blocks_.empty()) {
    auto it = leaf_blocks_.find(root_dablk_);
    return it == leaf_blocks_.end() ? nullptr : &it->second;
  }
  auto root_it = node_blocks_.find(root_dablk_);
  if (root_it == node_blocks_.end()) return nullptr;
  NodeBlock* node = &root_it->second;
  for (int depth = 0; depth < 8; ++depth) {
    if (node->ents.empty()) return nullptr;
    size_t chosen = node->ents.size() - 1;
    for (size_t i = 0; i < node->ents.size(); ++i) {
      if (node->ents[i].hash >= hash) {
        chosen = i;
        break;
      }
    }
    const uint32_t child = node->ents[chosen].before;
    if (node->level == 1) {
      auto it = leaf_blocks_.find(child);
      return it == leaf_blocks_.end() ? nullptr : &it->second;
    }
    auto it = node_blocks_.find(child);
    if (it == node_blocks_.end()) return nullptr;
    node = &it->second;
  }
  return nullptr;
}

Status DirectoryEditor::SplitLeaf(LeafBlock* leaf, std::vector<LeafEnt> merged) {
  const size_t mid = merged.size() / 2;
  std::vector<LeafEnt> left(merged.begin(), merged.begin() + mid);
  std::vector<LeafEnt> right(merged.begin() + mid, merged.end());
  if (left.empty() || right.empty()) {
    return XFS_ERR(XfsError::kInternal, "degenerate leaf split")
        .Ino(ino_)
        .Metadata("directory");
  }
  const bool root_leaf = node_blocks_.empty() && leaf->dablk == root_dablk_;
  if (root_leaf) {
    // xfs_da3_root_split: the root leaf moves to a fresh block and the root
    // block becomes a node with two children.
    uint32_t left_dablk = root_dablk_ + 1;
    while (leaf_blocks_.count(left_dablk) || left_dablk == root_dablk_) {
      ++left_dablk;
    }
    uint32_t right_dablk = left_dablk + 1;
    while (leaf_blocks_.count(right_dablk) || right_dablk == root_dablk_ ||
           right_dablk == left_dablk) {
      ++right_dablk;
    }
    auto alloc = AllocDirBlock(left_dablk);
    if (!alloc.ok()) return alloc.error();
    alloc = AllocDirBlock(right_dablk);
    if (!alloc.ok()) return alloc.error();
    LeafBlock l;
    l.dablk = left_dablk;
    l.back = 0;
    l.forw = right_dablk;
    l.ents = std::move(left);
    l.dirty = true;
    LeafBlock r;
    r.dablk = right_dablk;
    r.back = left_dablk;
    r.forw = 0;
    r.ents = std::move(right);
    r.dirty = true;
    leaf_blocks_.erase(root_dablk_);
    leaf_blocks_[left_dablk] = std::move(l);
    leaf_blocks_[right_dablk] = std::move(r);
    NodeBlock root;
    root.dablk = root_dablk_;
    root.level = 1;
    root.ents = {{leaf_blocks_[left_dablk].ents.back().hash, left_dablk},
                 {leaf_blocks_[right_dablk].ents.back().hash, right_dablk}};
    root.dirty = true;
    node_blocks_[root_dablk_] = std::move(root);
    return RebuildNodeTree();
  }
  uint32_t new_dablk = root_dablk_ + 1;
  while (leaf_blocks_.count(new_dablk) || node_blocks_.count(new_dablk) ||
         new_dablk == root_dablk_) {
    ++new_dablk;
  }
  auto alloc = AllocDirBlock(new_dablk);
  if (!alloc.ok()) return alloc.error();
  LeafBlock r;
  r.dablk = new_dablk;
  r.back = leaf->dablk;
  r.forw = leaf->forw;
  r.ents = std::move(right);
  r.dirty = true;
  leaf->ents = std::move(left);
  leaf->forw = new_dablk;
  leaf->dirty = true;
  if (r.forw != 0) {
    auto it = leaf_blocks_.find(r.forw);
    if (it == leaf_blocks_.end()) {
      return XFS_ERR(XfsError::kCorruptDirectory, "leaf chain broken")
          .Ino(ino_)
          .Actual(r.forw);
    }
    it->second.back = new_dablk;
    it->second.dirty = true;
  }
  leaf_blocks_[new_dablk] = std::move(r);
  return RebuildNodeTree();
}

Status DirectoryEditor::InsertNode(const DirEntryWrite& entry) {
  const uint32_t hash = DirHash(entry.name);
  const size_t need = DataEntrySize(entry.name.size());
  LeafBlock* leaf = LeafForHash(hash);
  if (leaf == nullptr) {
    return XFS_ERR(XfsError::kCorruptDirectory, "no leaf for the hash")
        .Ino(ino_)
        .Metadata("directory");
  }
  uint32_t db = 0;
  auto picked = PickDataBlock(hash, need);
  if (picked.ok()) {
    db = picked.value();
  } else {
    Status s = GrowDataBlock(&db);
    if (!s.ok()) return s;
  }
  const uint32_t end = DataEnd(data_blocks_[db]);
  uint32_t off = 0;
  Status s = DataInsertEntry(&data_blocks_[db], end, entry, &off);
  if (!s.ok()) return s;
  data_dirty_.insert(db);
  bests_[db] = DataBestFree(data_blocks_[db]);
  const uint32_t addr = (db << (block_log_ - 3)) | (off >> 3);

  bool needs_split = false;
  if (leaf->ents.size() >= leaf_max_ents_) {
    CompactLeaf(leaf);
    if (leaf->ents.size() >= leaf_max_ents_) needs_split = true;
  }
  if (needs_split) {
    std::vector<LeafEnt> merged = leaf->ents;
    size_t pos = 0;
    while (pos < merged.size() && merged[pos].hash <= hash) ++pos;
    merged.insert(merged.begin() + static_cast<ptrdiff_t>(pos),
                  LeafEnt{hash, addr});
    s = SplitLeaf(leaf, std::move(merged));
    if (!s.ok()) return s;
  } else {
    size_t index = 0;
    while (index < leaf->ents.size() && leaf->ents[index].hash <= hash) {
      ++index;
    }
    s = AddLeafEntry(leaf, index, hash, addr);
    if (!s.ok()) return s;
    s = RebuildNodeTree();
    if (!s.ok()) return s;
  }
  s = RebuildFreeBlocks();
  if (!s.ok()) return s;
  dirty_ = true;
  return OkStatus();
}

Status DirectoryEditor::RemoveNode(std::string_view name,
                                   XfsDirEntry* removed) {
  const uint32_t hash = DirHash(name);
  LeafBlock* leaf = LeafForHash(hash);
  if (leaf == nullptr) {
    return XFS_ERR(XfsError::kNotFound, "directory entry not found").Ino(ino_);
  }
  bool found = false;
  for (;;) {
    size_t index = 0;
    while (index < leaf->ents.size() && leaf->ents[index].hash < hash) ++index;
    for (; index < leaf->ents.size() && leaf->ents[index].hash == hash;
         ++index) {
      const uint32_t addr = leaf->ents[index].addr;
      if (addr == 0) continue;
      const uint32_t db = addr >> (block_log_ - 3);
      const uint32_t off = (addr & ((1u << (block_log_ - 3)) - 1)) << 3;
      auto block_it = data_blocks_.find(db);
      if (block_it == data_blocks_.end()) continue;
      const uint8_t namelen = GetU8(block_it->second.data(), off + 8);
      if (namelen != name.size() ||
          std::memcmp(block_it->second.data() + off + 9, name.data(),
                      name.size()) != 0) {
        continue;
      }
      removed->name = std::string(name);
      removed->ino = GetBE64(block_it->second.data(), off);
      removed->ftype = geo_->features.ftype
                           ? GetU8(block_it->second.data(), off + 9 + namelen)
                           : 0;
      bool empty = false;
      Status s = DataRemoveEntry(&block_it->second,
                                 DataEnd(block_it->second), off, &empty);
      if (!s.ok()) return s;
      data_dirty_.insert(db);
      leaf->ents[index].addr = 0;
      leaf->dirty = true;
      s = DropDataBlockIfEmpty(db, empty);
      if (!s.ok()) return s;
      if (data_blocks_.count(db)) bests_[db] = DataBestFree(data_blocks_[db]);
      found = true;
      break;
    }
    if (found) break;
    if (leaf->ents.empty() || leaf->ents.back().hash != hash ||
        leaf->forw == 0) {
      return XFS_ERR(XfsError::kNotFound, "directory entry not found")
          .Ino(ino_);
    }
    auto next = leaf_blocks_.find(leaf->forw);
    if (next == leaf_blocks_.end()) {
      return XFS_ERR(XfsError::kCorruptDirectory, "leaf chain broken")
          .Ino(ino_)
          .Actual(leaf->forw);
    }
    leaf = &next->second;
  }

  CompactLeaf(leaf);
  if (leaf->ents.empty() && leaf->dablk != root_dablk_ &&
      leaf_blocks_.size() > 1) {
    const uint32_t dablk = leaf->dablk;
    const uint32_t forw = leaf->forw;
    const uint32_t back = leaf->back;
    auto prev = leaf_blocks_.find(back);
    if (prev != leaf_blocks_.end()) {
      prev->second.forw = forw;
      prev->second.dirty = true;
    }
    if (forw != 0) {
      auto next = leaf_blocks_.find(forw);
      if (next != leaf_blocks_.end()) {
        next->second.back = back;
        next->second.dirty = true;
      }
    }
    leaf_blocks_.erase(dablk);
    Status s = FreeDirBlock(dablk);
    if (!s.ok()) return s;
  }
  Status s = RebuildNodeTree();
  if (!s.ok()) return s;
  s = RebuildFreeBlocks();
  if (!s.ok()) return s;
  dirty_ = true;
  return OkStatus();
}

Status DirectoryEditor::RebuildNodeTree() {
  if (leaf_blocks_.empty()) {
    return XFS_ERR(XfsError::kInternal, "directory has no leaf blocks")
        .Ino(ino_)
        .Metadata("directory");
  }
  uint32_t first = 0;
  size_t roots = 0;
  for (const auto& entry : leaf_blocks_) {
    if (entry.second.back == 0) {
      first = entry.first;
      ++roots;
    }
  }
  if (roots != 1) {
    return XFS_ERR(XfsError::kCorruptDirectory,
                   "leaf chain has no unique first block")
        .Ino(ino_)
        .Actual(roots);
  }
  std::vector<uint32_t> order;
  uint32_t cur = first;
  while (cur != 0) {
    order.push_back(cur);
    auto it = leaf_blocks_.find(cur);
    if (it == leaf_blocks_.end()) {
      return XFS_ERR(XfsError::kCorruptDirectory, "leaf chain broken")
          .Ino(ino_)
          .Actual(cur);
    }
    cur = it->second.forw;
    if (order.size() > leaf_blocks_.size()) {
      return XFS_ERR(XfsError::kCorruptDirectory, "leaf chain is cyclic")
          .Ino(ino_);
    }
  }
  if (order.size() != leaf_blocks_.size()) {
    return XFS_ERR(XfsError::kCorruptDirectory,
                   "leaf chain does not reach every leaf")
        .Ino(ino_);
  }
  if (leaf_blocks_.size() == 1) {
    if (node_blocks_.empty()) return OkStatus();  // root-leaf state
    return NodeToLeaf1();
  }
  // Existing node blocks (except the root) are reused in order.
  std::vector<uint32_t> pool;
  for (const auto& entry : node_blocks_) {
    if (entry.first != root_dablk_) pool.push_back(entry.first);
  }
  std::sort(pool.begin(), pool.end());
  size_t pool_index = 0;
  std::set<uint32_t> used(order.begin(), order.end());
  std::vector<NodeBlock> pending;
  std::vector<NodeEnt> level_ents;
  for (uint32_t dablk : order) {
    const LeafBlock& leaf = leaf_blocks_[dablk];
    if (leaf.ents.empty()) {
      return XFS_ERR(XfsError::kInternal, "empty leaf in a node directory")
          .Ino(ino_)
          .Actual(dablk);
    }
    level_ents.push_back({leaf.ents.back().hash, dablk});
  }
  uint16_t level = 1;
  while (level_ents.size() > node_max_ents_) {
    const size_t groups =
        (level_ents.size() + node_max_ents_ - 1) / node_max_ents_;
    const size_t per = (level_ents.size() + groups - 1) / groups;
    std::vector<NodeEnt> next;
    for (size_t start = 0; start < level_ents.size(); start += per) {
      const size_t end = std::min(start + per, level_ents.size());
      NodeBlock node;
      node.level = level;
      node.ents.assign(level_ents.begin() + static_cast<ptrdiff_t>(start),
                       level_ents.begin() + static_cast<ptrdiff_t>(end));
      if (pool_index < pool.size()) {
        node.dablk = pool[pool_index++];
      } else {
        uint32_t dablk = root_dablk_ + 1;
        while (used.count(dablk) || node_blocks_.count(dablk)) ++dablk;
        auto alloc = AllocDirBlock(dablk);
        if (!alloc.ok()) return alloc.error();
        node.dablk = dablk;
      }
      used.insert(node.dablk);
      next.push_back({node.ents.back().hash, node.dablk});
      node.dirty = true;
      pending.push_back(std::move(node));
    }
    level_ents = std::move(next);
    ++level;
  }
  for (size_t i = pool_index; i < pool.size(); ++i) {
    Status s = FreeDirBlock(pool[i]);
    if (!s.ok()) return s;
  }
  NodeBlock root;
  root.dablk = root_dablk_;
  root.level = level;
  root.ents = std::move(level_ents);
  root.dirty = true;
  // forw/back within each level, root 0/0 like the kernel.
  std::map<uint32_t, NodeBlock> all;
  for (NodeBlock& node : pending) all[node.dablk] = std::move(node);
  all[root_dablk_] = std::move(root);
  for (uint16_t lvl = 1; lvl < level + 1; ++lvl) {
    std::vector<uint32_t> same;
    for (const auto& entry : all) {
      if (entry.second.level == lvl && entry.first != root_dablk_) {
        same.push_back(entry.first);
      }
    }
    std::sort(same.begin(), same.end());
    for (size_t i = 0; i < same.size(); ++i) {
      all[same[i]].back = i == 0 ? 0 : same[i - 1];
      all[same[i]].forw = i + 1 == same.size() ? 0 : same[i + 1];
    }
  }
  node_blocks_ = std::move(all);
  return OkStatus();
}

Status DirectoryEditor::NodeToLeaf1() {
  if (leaf_blocks_.size() != 1) return OkStatus();
  LeafBlock& leaf = leaf_blocks_.begin()->second;
  const uint32_t bestcount = static_cast<uint32_t>(data_size_ >> block_log_);
  if (kEnts + leaf.ents.size() * 8 + static_cast<size_t>(bestcount) * 2 + 4 >
      dir_block_size_) {
    return OkStatus();
  }
  if (free_blocks_.size() > 1) return OkStatus();
  const uint32_t leaf_dablk = leaf.dablk;
  if (leaf_dablk != root_dablk_) {
    // root_dablk_ is already mapped (as the node root); reuse it.
    Status s = FreeDirBlock(leaf_dablk);
    if (!s.ok()) return s;
    LeafBlock moved = std::move(leaf);
    leaf_blocks_.erase(leaf_blocks_.begin());
    moved.dablk = root_dablk_;
    moved.forw = 0;
    moved.back = 0;
    moved.dirty = true;
    leaf_blocks_[root_dablk_] = std::move(moved);
  }
  for (const auto& entry : free_blocks_) {
    Status s = FreeDirBlock(entry.first);
    if (!s.ok()) return s;
  }
  free_blocks_.clear();
  for (const auto& entry : node_blocks_) {
    if (entry.first == root_dablk_) continue;
    Status s = FreeDirBlock(entry.first);
    if (!s.ok()) return s;
  }
  node_blocks_.clear();
  fmt_ = XfsDirFormat::kLeaf;
  leaf_blocks_[root_dablk_].dirty = true;
  dirty_ = true;
  return OkStatus();
}

Status DirectoryEditor::RebuildFreeBlocks() {
  if (fmt_ != XfsDirFormat::kNode) return OkStatus();
  const uint32_t bestcount = static_cast<uint32_t>(data_size_ >> block_log_);
  const uint32_t fdb_count =
      (bestcount + free_max_bests_ - 1) / free_max_bests_;
  const uint64_t free_first = d::kFreeOffset >> block_log_;
  std::set<uint32_t> keep;
  for (uint32_t fdb = 0; fdb < fdb_count; ++fdb) {
    uint32_t last_valid = 0;
    uint32_t nused = 0;
    for (uint32_t i = 0; i < free_max_bests_; ++i) {
      const uint32_t db = fdb * free_max_bests_ + i;
      if (db >= bestcount) break;
      if (data_blocks_.count(db)) {
        last_valid = i + 1;
        ++nused;
      }
    }
    if (nused == 0) continue;
    const uint32_t dablk = static_cast<uint32_t>(free_first + fdb);
    keep.insert(dablk);
    auto it = free_blocks_.find(dablk);
    if (it == free_blocks_.end()) {
      auto alloc = AllocDirBlock(dablk);
      if (!alloc.ok()) return alloc.error();
      FreeBlock fb;
      fb.dablk = dablk;
      it = free_blocks_.emplace(dablk, std::move(fb)).first;
    }
    FreeBlock& fb = it->second;
    fb.bests.assign(last_valid, kNullDataOff);
    for (uint32_t i = 0; i < last_valid; ++i) {
      const uint32_t db = fdb * free_max_bests_ + i;
      if (data_blocks_.count(db)) fb.bests[i] = BestsFor(db);
    }
    fb.dirty = true;
  }
  for (auto it = free_blocks_.begin(); it != free_blocks_.end();) {
    if (keep.count(it->first)) {
      ++it;
      continue;
    }
    Status s = FreeDirBlock(it->first);
    if (!s.ok()) return s;
    it = free_blocks_.erase(it);
  }
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

Status DirectoryEditor::LogBlockImage(uint64_t db, std::vector<uint8_t> image,
                                      uint16_t blf_type, size_t lsn_off,
                                      size_t crc_off) {
  const uint64_t fsb = FsbForFileBlock(db);
  if (fsb == kNullFsb) {
    return XFS_ERR(XfsError::kInternal, "directory block is not mapped")
        .Ino(ino_)
        .DirBlock(db);
  }
  return txn_->AddMetadataBuffer(geo_->FsbToByte(fsb), std::move(image),
                                 blf_type, lsn_off, crc_off, "directory");
}

Status DirectoryEditor::LogDataBlock(uint32_t db) {
  auto it = data_blocks_.find(db);
  if (it == data_blocks_.end()) return OkStatus();
  return LogBlockImage(db, it->second, lf::kBlftDirData, kBlkLsn, kBlkCrc);
}

Status DirectoryEditor::LogLeafBlock(const LeafBlock& leaf) {
  std::vector<uint8_t> image;
  const bool leaf1 = fmt_ == XfsDirFormat::kLeaf;
  const uint64_t fsb = FsbForFileBlock(leaf.dablk);
  if (fsb == kNullFsb) {
    return XFS_ERR(XfsError::kInternal, "leaf block is not mapped").Ino(ino_);
  }
  InitLeafBlock(&image, geo_->FsbToByte(fsb) / 512, ino_,
                leaf1 ? ondisk::kDir3Leaf1Magic : ondisk::kDir3LeafnMagic);
  PutBE32(image.data(), kInfoForw, leaf.forw);
  PutBE32(image.data(), kInfoBack, leaf.back);
  uint16_t stale = 0;
  for (const LeafEnt& e : leaf.ents) {
    if (e.addr == 0) ++stale;
  }
  PutBE16(image.data(), kLeafCount, static_cast<uint16_t>(leaf.ents.size()));
  PutBE16(image.data(), kLeafStale, stale);
  for (size_t i = 0; i < leaf.ents.size(); ++i) {
    PutBE32(image.data(), kEnts + i * 8, leaf.ents[i].hash);
    PutBE32(image.data(), kEnts + i * 8 + 4, leaf.ents[i].addr);
  }
  if (leaf1) {
    const uint32_t bestcount =
        static_cast<uint32_t>(data_size_ >> block_log_);
    const size_t bests_off = dir_block_size_ - 4 - bestcount * 2;
    if (bests_off < kEnts + leaf.ents.size() * 8) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "leaf1 entries and bests do not fit")
          .Ino(ino_)
          .Metadata("directory");
    }
    for (uint32_t db = 0; db < bestcount; ++db) {
      PutBE16(image.data(), bests_off + db * 2, BestsFor(db));
    }
    PutBE32(image.data(), dir_block_size_ - 4, bestcount);
  }
  return LogBlockImage(leaf.dablk, std::move(image),
                       leaf1 ? lf::kBlftDirLeaf1 : lf::kBlftDirLeafn,
                       kInfoLsn, kInfoCrc);
}

Status DirectoryEditor::LogFreeBlock(const FreeBlock& fb) {
  std::vector<uint8_t> image;
  const uint64_t fsb = FsbForFileBlock(fb.dablk);
  if (fsb == kNullFsb) {
    return XFS_ERR(XfsError::kInternal, "free block is not mapped").Ino(ino_);
  }
  InitFreeBlock(&image, geo_->FsbToByte(fsb) / 512, ino_);
  const uint64_t free_first = d::kFreeOffset >> block_log_;
  const uint32_t firstdb = static_cast<uint32_t>(
      (fb.dablk - free_first) * static_cast<uint64_t>(free_max_bests_));
  uint32_t nused = 0;
  for (uint16_t v : fb.bests) {
    if (v != kNullDataOff) ++nused;
  }
  PutBE32(image.data(), kFreeFirstdb, firstdb);
  PutBE32(image.data(), kFreeNvalid, static_cast<uint32_t>(fb.bests.size()));
  PutBE32(image.data(), kFreeNused, nused);
  for (size_t i = 0; i < fb.bests.size(); ++i) {
    PutBE16(image.data(), kFreeBests + i * 2, fb.bests[i]);
  }
  return LogBlockImage(fb.dablk, std::move(image), lf::kBlftDirFree, kBlkLsn,
                       kBlkCrc);
}

Status DirectoryEditor::LogNodeBlock(const NodeBlock& node) {
  std::vector<uint8_t> image;
  const uint64_t fsb = FsbForFileBlock(node.dablk);
  if (fsb == kNullFsb) {
    return XFS_ERR(XfsError::kInternal, "node block is not mapped").Ino(ino_);
  }
  InitNodeBlock(&image, geo_->FsbToByte(fsb) / 512, ino_, node.level);
  PutBE32(image.data(), kInfoForw, node.forw);
  PutBE32(image.data(), kInfoBack, node.back);
  PutBE16(image.data(), kNodeCount, static_cast<uint16_t>(node.ents.size()));
  for (size_t i = 0; i < node.ents.size(); ++i) {
    PutBE32(image.data(), kEnts + i * 8, node.ents[i].hash);
    PutBE32(image.data(), kEnts + i * 8 + 4, node.ents[i].before);
  }
  return LogBlockImage(node.dablk, std::move(image), lf::kBlftDaNode,
                       kInfoLsn, kInfoCrc);
}

Status DirectoryEditor::LogBlocks() {
  if (fmt_ == XfsDirFormat::kShortform) return OkStatus();
  if (fmt_ == XfsDirFormat::kBlock) {
    if (!block_dirty_) return OkStatus();
    const uint64_t fsb = FsbForFileBlock(0);
    if (fsb == kNullFsb) {
      return XFS_ERR(XfsError::kInternal, "block directory is not mapped")
          .Ino(ino_);
    }
    std::vector<uint8_t> image;
    BuildBlockImage(&image);
    PutBE64(image.data(), kBlkBlkno, geo_->FsbToByte(fsb) / 512);
    std::memcpy(image.data() + kBlkUuid, geo_->meta_uuid.data(), 16);
    PutBE64(image.data(), kBlkOwner, ino_);
    return LogBlockImage(0, std::move(image), lf::kBlftDirBlock, kBlkLsn,
                         kBlkCrc);
  }
  for (uint32_t db : data_dirty_) {
    Status s = LogDataBlock(db);
    if (!s.ok()) return s;
  }
  for (auto& entry : leaf_blocks_) {
    if (!entry.second.dirty) continue;
    Status s = LogLeafBlock(entry.second);
    if (!s.ok()) return s;
  }
  for (auto& entry : node_blocks_) {
    if (!entry.second.dirty) continue;
    Status s = LogNodeBlock(entry.second);
    if (!s.ok()) return s;
  }
  for (auto& entry : free_blocks_) {
    if (!entry.second.dirty) continue;
    Status s = LogFreeBlock(entry.second);
    if (!s.ok()) return s;
  }
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

Status DirectoryEditor::FinalizeInode(std::vector<uint8_t>* raw_inode,
                                      uint32_t* ilog_fields,
                                      std::vector<uint8_t>* fork_region) {
  if (raw_inode->size() < geo_->inode_size) {
    return XFS_ERR(XfsError::kInternal, "inode image too small").Ino(ino_);
  }
  uint8_t* core = raw_inode->data();
  if (fmt_ == XfsDirFormat::kShortform) {
    std::vector<uint8_t> image;
    BuildShortformImage(&image);
    data_size_ = image.size();
    const size_t literal =
        static_cast<size_t>(geo_->inode_size) - di::kCoreSizeV3;
    if (image.size() > literal) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "shortform directory does not fit the inode")
          .Ino(ino_)
          .Metadata("directory");
    }
    core[di::kFormat] = di::kFormatLocal;
    std::memset(core + di::kCoreSizeV3, 0, literal);
    std::memcpy(core + di::kCoreSizeV3, image.data(), image.size());
    PutBE64(core, di::kSize, image.size());
    *ilog_fields = lf::kILogDData;
    *fork_region = std::move(image);
    return OkStatus();
  }
  Status s = fork_->Finalize(raw_inode, ilog_fields, fork_region);
  if (!s.ok()) return s;
  PutBE64(core, di::kSize, data_size_);
  return OkStatus();
}

Status DirectoryEditor::Insert(const DirEntryWrite& entry) {
  Status s = ValidateDirName(entry.name, ino_);
  if (!s.ok()) return s;
  if (fmt_ == XfsDirFormat::kShortform) {
    s = InsertPlaced(entry, d::kData3HeaderSize + 32, true);
    if (s.ok()) return OkStatus();
    if (s.error().code != XfsError::kNoSpace) return s;
    // Convert to the block format and retry.
    auto alloc = AllocDirBlock(0);
    if (!alloc.ok()) return alloc.error();
    (void)alloc;
    fmt_ = XfsDirFormat::kBlock;
    data_size_ = dir_block_size_;
    block_dirty_ = true;
    dirty_ = true;
    return Insert(entry);
  }
  if (fmt_ == XfsDirFormat::kBlock) {
    s = InsertPlaced(entry, d::kData3HeaderSize + 32, false);
    if (s.ok()) return OkStatus();
    if (s.error().code != XfsError::kNoSpace) return s;
    s = BlockToLeaf();
    if (!s.ok()) return s;
    return Insert(entry);
  }
  if (fmt_ == XfsDirFormat::kLeaf) {
    s = InsertLeaf(entry);
    if (s.ok()) return OkStatus();
    if (s.error().code != XfsError::kNoSpace) return s;
    s = LeafToNode();
    if (!s.ok()) return s;
    return Insert(entry);
  }
  return InsertNode(entry);
}

Result<XfsDirEntry> DirectoryEditor::Remove(std::string_view name) {
  XfsDirEntry removed;
  Status s = OkStatus();
  switch (fmt_) {
    case XfsDirFormat::kShortform:
    case XfsDirFormat::kBlock:
      s = RemovePlaced(name, &removed);
      break;
    case XfsDirFormat::kLeaf:
      s = RemoveLeaf(name, &removed);
      break;
    case XfsDirFormat::kNode:
      s = RemoveNode(name, &removed);
      break;
    default:
      s = XFS_ERR(XfsError::kInternal, "unhandled directory format");
      break;
  }
  if (!s.ok()) return s.error();
  return removed;
}

Status DirectoryEditor::BlockToLeaf() {
  const uint64_t fsb = FsbForFileBlock(0);
  if (fsb == kNullFsb) {
    return XFS_ERR(XfsError::kInternal, "block directory is not mapped")
        .Ino(ino_);
  }
  std::vector<uint8_t> data_image;
  BuildDataBlockFromPlaced(&data_image, fsb);
  data_blocks_[0] = std::move(data_image);
  bests_[0] = DataBestFree(data_blocks_[0]);
  data_dirty_.insert(0);
  auto leaf_alloc = AllocDirBlock(root_dablk_);
  if (!leaf_alloc.ok()) return leaf_alloc.error();
  LeafBlock leaf;
  leaf.dablk = root_dablk_;
  leaf.forw = 0;
  leaf.back = 0;
  BuildLeafFromPlaced(&leaf);
  leaf.dirty = true;
  leaf_blocks_[root_dablk_] = std::move(leaf);
  data_size_ = dir_block_size_;
  fmt_ = XfsDirFormat::kLeaf;
  dirty_ = true;
  placed_.clear();
  return OkStatus();
}

Status DirectoryEditor::RepointDotDot(uint64_t parent_ino) {
  switch (fmt_) {
    case XfsDirFormat::kShortform:
      sf_parent_ = parent_ino;
      dirty_ = true;
      return OkStatus();
    case XfsDirFormat::kBlock:
      sf_parent_ = parent_ino;
      block_dirty_ = true;
      dirty_ = true;
      return OkStatus();
    case XfsDirFormat::kLeaf:
    case XfsDirFormat::kNode: {
      const uint32_t hash = DirHash("..");
      LeafBlock* leaf = fmt_ == XfsDirFormat::kLeaf
                            ? &leaf_blocks_[root_dablk_]
                            : LeafForHash(hash);
      if (leaf == nullptr) {
        return XFS_ERR(XfsError::kCorruptDirectory, "no leaf for '..'")
            .Ino(ino_);
      }
      size_t index = 0;
      while (index < leaf->ents.size() && leaf->ents[index].hash < hash) {
        ++index;
      }
      for (; index < leaf->ents.size() && leaf->ents[index].hash == hash;
           ++index) {
        const uint32_t addr = leaf->ents[index].addr;
        if (addr == 0) continue;
        const uint32_t db = addr >> (block_log_ - 3);
        const uint32_t off = (addr & ((1u << (block_log_ - 3)) - 1)) << 3;
        auto it = data_blocks_.find(db);
        if (it == data_blocks_.end()) continue;
        uint8_t* b = it->second.data();
        const uint8_t namelen = GetU8(b, off + 8);
        if (namelen != 2 || b[off + 9] != '.' || b[off + 10] != '.') continue;
        PutBE64(b, off, parent_ino);
        data_dirty_.insert(db);
        sf_parent_ = parent_ino;
        dirty_ = true;
        return OkStatus();
      }
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "directory has no '..' entry")
          .Ino(ino_);
    }
  }
  return XFS_ERR(XfsError::kInternal, "unhandled directory format").Ino(ino_);
}

}  // namespace xfs
