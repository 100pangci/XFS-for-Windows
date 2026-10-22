// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - data fork (extent map) mutation for an existing inode.
//
// Layouts (see include/xfs/bmap.h for the invariants):
//
//   extents format: records packed from the start of the literal area,
//                   16 bytes each, di_nextents of them.
//   btree format:   the fork holds the btree root (xfs_bmdr_block):
//                     +0  level   be16 (>= 1)
//                     +2  numrecs be16
//                     +4  keys    be64[numrecs]          (startoff)
//                     +4 + max*8  ptrs  be64[numrecs]     (child fsb)
//                   max = (fork_bytes - 4) / 16.
//   bmbt block:     xfs_btree_block (long, v5): 72-byte header,
//                   leaf records at +72 (16 bytes each),
//                   node keys at +72, node pointers at +72 + max*8,
//                   max = (blocksize - 72) / 16.
//
// The in-fork root is kept in the same in-memory shape as a block (header +
// keys + pointers) using the bmdr key/pointer offsets, so one set of accessors
// serves both. Finalize emits the in-core root form (72-byte header) for the
// XFS_ILOG_DBROOT log region, which is what recovery converts back.
#include "xfs/bmap.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "xfs/alloc.h"
#include "xfs/btree.h"
#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/log_format.h"
#include "xfs/ondisk.h"

namespace xfs {
namespace {

namespace bt = ondisk::btree;
namespace di = ondisk::inode;
namespace lf = logfmt;

constexpr uint64_t kNullFsb = ~0ull;
constexpr size_t kLongHeader = bt::kLHeaderLenV5;  // 72
constexpr size_t kRecSize = bt::kBmbtRecSize;      // 16
constexpr size_t kKeySize = bt::kBmbtKeySize;      // 8
constexpr size_t kPtrSize = bt::kBmbtPtrSize;      // 8
constexpr size_t kRootHeader = bt::kBmdrSize;      // 4
constexpr uint64_t kMaxExtentLen = ondisk::kBmbtMaxBlockcount;

// Long-format header field offsets (v5).
constexpr size_t kLMagic = bt::kMagic;         // be32 'BMA3'
constexpr size_t kLLevel = bt::kLevel;         // be16
constexpr size_t kLNumrecs = bt::kNumrecs;     // be16
constexpr size_t kLLeftsib = bt::kLLeftsib;    // be64
constexpr size_t kLRightSib = bt::kLRightSib;  // be64
constexpr size_t kLBlkno = bt::kLBlkno;        // be64 (daddr)
constexpr size_t kLLsn = bt::kLLsn;            // be64
constexpr size_t kLUuid = bt::kLUuid;          // 16 bytes
constexpr size_t kLOwner = bt::kLOwner;        // be64
constexpr size_t kLCrc = bt::kLCrc;            // le32

}  // namespace

DataForkEditor::DataForkEditor(XfsTransaction* txn, BlockAllocator* alloc,
                               BlockDevice* device, const XfsGeometry* geo)
    : txn_(txn), alloc_(alloc), device_(device), geo_(geo) {}

DataForkEditor::~DataForkEditor() = default;

// ---------------------------------------------------------------------------
// Node access
// ---------------------------------------------------------------------------

uint16_t DataForkEditor::NodeLevel(const Node& node) const {
  if (node.is_root) return GetBE16(root_.data.data(), 0);
  return GetBE16(node.data.data(), kLLevel);
}

uint16_t DataForkEditor::NodeNumRecs(const Node& node) const {
  if (node.is_root) return GetBE16(root_.data.data(), 2);
  return GetBE16(node.data.data(), kLNumrecs);
}

void DataForkEditor::SetNodeNumRecs(Node& node, uint16_t numrecs) {
  if (node.is_root) {
    PutBE16(root_.data.data(), 2, numrecs);
    root_.dirty = true;
    return;
  }
  PutBE16(node.data.data(), kLNumrecs, numrecs);
  node.dirty = true;
}

uint32_t DataForkEditor::NodeMaxRecs(const Node& node) const {
  if (node.is_root) {
    return static_cast<uint32_t>((fork_bytes_ - kRootHeader) / kRecSize);
  }
  return static_cast<uint32_t>((geo_->block_size - kLongHeader) / kRecSize);
}

uint32_t DataForkEditor::NodeMinRecs(const Node& node) const {
  if (node.is_root) return 1;
  return NodeMaxRecs(node) / 2;
}

size_t DataForkEditor::KeyOffset(const Node& node) const {
  return node.is_root ? kRootHeader : kLongHeader;
}

size_t DataForkEditor::PtrOffset(const Node& node) const {
  if (node.is_root) {
    const size_t maxrecs = (fork_bytes_ - kRootHeader) / kRecSize;
    return kRootHeader + maxrecs * kKeySize;
  }
  const size_t maxrecs = (geo_->block_size - kLongHeader) / kRecSize;
  return kLongHeader + maxrecs * kKeySize;
}

uint64_t DataForkEditor::NodeKey(const Node& node, size_t i) const {
  return GetBE64(node.data.data(), KeyOffset(node) + i * kKeySize);
}

void DataForkEditor::SetNodeKey(Node& node, size_t i, uint64_t startoff) {
  PutBE64(node.data.data(), KeyOffset(node) + i * kKeySize, startoff);
  node.dirty = true;
}

uint64_t DataForkEditor::NodePtr(const Node& node, size_t i) const {
  return GetBE64(node.data.data(), PtrOffset(node) + i * kPtrSize);
}

void DataForkEditor::SetNodePtr(Node& node, size_t i, uint64_t fsb) {
  PutBE64(node.data.data(), PtrOffset(node) + i * kPtrSize, fsb);
  node.dirty = true;
}

XfsExtent DataForkEditor::NodeLeafRec(const Node& node, size_t i) const {
  XfsExtent e;
  const uint8_t* p = node.data.data() + kLongHeader + i * kRecSize;
  const ondisk::BmbtRec r = ondisk::DecodeBmbtRec(GetBE64(p), GetBE64(p + 8));
  e.startoff = r.startoff;
  e.startblock = r.startblock;
  e.blockcount = r.blockcount;
  e.unwritten = r.unwritten;
  return e;
}

void DataForkEditor::SetNodeLeafRec(Node& node, size_t i,
                                    const XfsExtent& rec) {
  uint8_t* p = node.data.data() + kLongHeader + i * kRecSize;
  ondisk::BmbtRec r;
  r.startoff = rec.startoff;
  r.startblock = rec.startblock;
  r.blockcount = rec.blockcount;
  r.unwritten = rec.unwritten;
  uint64_t l0 = 0, l1 = 0;
  ondisk::EncodeBmbtRec(r, &l0, &l1);
  PutBE64(p, 0, l0);
  PutBE64(p, 8, l1);
  node.dirty = true;
}

uint64_t DataForkEditor::FirstKey(const Node& node) const {
  if (NodeLevel(node) == 0) return NodeLeafRec(node, 0).startoff;
  return NodeKey(node, 0);
}

size_t DataForkEditor::ChooseChild(const Node& node, uint64_t startoff) const {
  const uint16_t n = NodeNumRecs(node);
  size_t chosen = 0;
  for (size_t i = 0; i < n; ++i) {
    if (NodeKey(node, i) <= startoff) {
      chosen = i;
    } else {
      break;
    }
  }
  return chosen;
}

std::vector<DataForkEditor::Entry> DataForkEditor::ReadEntries(const Node& node) const {
  const uint16_t n = NodeNumRecs(node);
  const uint16_t level = NodeLevel(node);
  std::vector<Entry> out;
  out.reserve(n);
  for (uint16_t i = 0; i < n; ++i) {
    Entry entry;
    if (level == 0) {
      entry.rec = NodeLeafRec(node, i);
    } else {
      entry.rec.startoff = NodeKey(node, i);
      entry.ptr = NodePtr(node, i);
    }
    out.push_back(entry);
  }
  return out;
}

void DataForkEditor::WriteEntries(Node& node, const std::vector<Entry>& entries) {
  const uint16_t level = NodeLevel(node);
  SetNodeNumRecs(node, static_cast<uint16_t>(entries.size()));
  for (size_t i = 0; i < entries.size(); ++i) {
    if (level == 0) {
      SetNodeLeafRec(node, i, entries[i].rec);
    } else {
      SetNodeKey(node, i, entries[i].rec.startoff);
      SetNodePtr(node, i, entries[i].ptr);
    }
  }
}

void DataForkEditor::RemoveEntry(Node& node, size_t pos) {
  const uint16_t n = NodeNumRecs(node);
  if (pos >= n) return;
  if (NodeLevel(node) == 0) {
    for (size_t i = pos; i + 1 < n; ++i) {
      SetNodeLeafRec(node, i, NodeLeafRec(node, i + 1));
    }
  } else {
    for (size_t i = pos; i + 1 < n; ++i) {
      SetNodeKey(node, i, NodeKey(node, i + 1));
      SetNodePtr(node, i, NodePtr(node, i + 1));
    }
  }
  SetNodeNumRecs(node, static_cast<uint16_t>(n - 1));
}

// ---------------------------------------------------------------------------
// Loading and block allocation
// ---------------------------------------------------------------------------

DataForkEditor::Node* DataForkEditor::FindNode(uint64_t fsb) {
  auto it = blocks_.find(fsb);
  return it == blocks_.end() ? nullptr : &it->second;
}

Status DataForkEditor::ReadBlock(uint64_t fsb, Node* out) {
  if (!geo_->FsbValid(fsb)) {
    return XFS_ERR(XfsError::kCorruptBtree, "bmbt block out of range")
        .Ino(ino_)
        .Fsb(fsb)
        .Metadata("bmbt");
  }
  Node node;
  node.data.assign(geo_->block_size, 0);
  node.fsb = fsb;
  node.is_root = false;
  const uint64_t offset = geo_->FsbToByte(fsb);
  if (!device_->Read(offset, node.data.data(), node.data.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read bmbt block")
        .Ino(ino_)
        .Fsb(fsb)
        .Offset(offset)
        .Metadata("bmbt");
  }
  if (GetBE32(node.data.data(), kLMagic) != ondisk::kBmapCrcMagic) {
    return XFS_ERR(XfsError::kBadMagic, "bmbt block magic mismatch")
        .Ino(ino_)
        .Fsb(fsb)
        .Metadata("bmbt");
  }
  if (GetBE64(node.data.data(), kLOwner) != ino_) {
    return XFS_ERR(XfsError::kBadOwner, "bmbt block owner mismatch")
        .Ino(ino_)
        .Fsb(fsb)
        .Metadata("bmbt");
  }
  const uint16_t level = GetBE16(node.data.data(), kLLevel);
  const uint16_t numrecs = GetBE16(node.data.data(), kLNumrecs);
  if (level > bt::kMaxTraversalLevels) {
    return XFS_ERR(XfsError::kCorruptBtree, "bmbt block level too deep")
        .Ino(ino_)
        .Fsb(fsb)
        .Metadata("bmbt");
  }
  const uint32_t maxrecs =
      static_cast<uint32_t>((geo_->block_size - kLongHeader) / kRecSize);
  if (numrecs > maxrecs) {
    return XFS_ERR(XfsError::kCorruptBtree, "bmbt record count out of range")
        .Ino(ino_)
        .Fsb(fsb)
        .Metadata("bmbt")
        .Expected(maxrecs)
        .Actual(numrecs);
  }
  *out = std::move(node);
  return OkStatus();
}

Status DataForkEditor::Load(const XfsInode& inode) {
  ino_ = inode.ino;
  fork_bytes_ = static_cast<uint32_t>(inode.data_fork.size());
  format_ = inode.format;
  extents_.clear();
  if (format_ == di::kFormatLocal) {
    // A shortform directory has no extent list yet; the first InsertExtent
    // converts the fork to extents format (xfs_bmap_local_to_extents).
    nextents_ = 0;
    loaded_ = true;
    return OkStatus();
  }
  Status s = LoadDataForkExtents(*device_, *geo_, inode, &extents_,
                                 /*cap=*/1u << 20);
  if (!s.ok()) return s;
  nextents_ = static_cast<uint32_t>(extents_.size());

  if (format_ == di::kFormatBtree) {
    root_.data.assign(fork_bytes_, 0);
    std::memcpy(root_.data.data(), inode.data_fork.data(),
                std::min<size_t>(fork_bytes_, inode.data_fork.size()));
    root_.is_root = true;
    root_.dirty = false;
    const uint16_t level = GetBE16(root_.data.data(), 0);
    const uint16_t numrecs = GetBE16(root_.data.data(), 2);
    if (level < 1 || level > bt::kMaxTraversalLevels) {
      return XFS_ERR(XfsError::kCorruptInode, "bmbt root level out of range")
          .Ino(ino_)
          .Metadata("inode")
          .Actual(level);
    }
    if (numrecs == 0 || numrecs > NodeMaxRecs(root_)) {
      return XFS_ERR(XfsError::kCorruptInode, "bmbt root record count invalid")
          .Ino(ino_)
          .Metadata("inode")
          .Actual(numrecs);
    }
    std::vector<std::pair<uint64_t, uint16_t>> stack;  // fsb, expected level
    for (size_t i = 0; i < numrecs; ++i) {
      stack.push_back({NodePtr(root_, i), static_cast<uint16_t>(level - 1)});
    }
    while (!stack.empty()) {
      const uint64_t fsb = stack.back().first;
      const uint16_t expected = stack.back().second;
      stack.pop_back();
      if (blocks_.count(fsb)) {
        return XFS_ERR(XfsError::kCorruptBtree, "bmbt cycle")
            .Ino(ino_)
            .Fsb(fsb)
            .Metadata("bmbt");
      }
      if (blocks_.size() > 4096) {
        return XFS_ERR(XfsError::kUnsupportedFeature,
                       "bmbt has more blocks than this build can edit")
            .Ino(ino_)
            .Metadata("bmbt");
      }
      Node node;
      s = ReadBlock(fsb, &node);
      if (!s.ok()) return s;
      if (GetBE16(node.data.data(), kLLevel) != expected) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "bmbt child level does not match the parent")
            .Ino(ino_)
            .Fsb(fsb)
            .Metadata("bmbt")
            .Expected(expected)
            .Actual(GetBE16(node.data.data(), kLLevel));
      }
      if (expected > 0) {
        const uint16_t n = GetBE16(node.data.data(), kLNumrecs);
        for (size_t i = 0; i < n; ++i) {
          stack.push_back(
              {NodePtr(node, i), static_cast<uint16_t>(expected - 1)});
        }
      }
      blocks_.emplace(fsb, std::move(node));
    }
  }
  loaded_ = true;
  return OkStatus();
}

uint32_t DataForkEditor::preferred_agno() const {
  return geo_->InoAgno(ino_);
}

Status DataForkEditor::AllocBmbtBlock(uint64_t* fsb) {
  Allocation alloc;
  Status s = alloc_->Allocate(preferred_agno(), 1, &alloc);
  if (!s.ok()) return s;
  nblocks_delta_ += 1;  // bmbt blocks are counted by di_nblocks
  *fsb = alloc.fsb;
  return OkStatus();
}

Status DataForkEditor::FreeBmbtBlock(uint64_t fsb) {
  Status s = alloc_->Free(fsb, 1);
  if (!s.ok()) return s;
  nblocks_delta_ -= 1;
  return OkStatus();
}

Status DataForkEditor::AllocNode(uint64_t fsb, uint16_t level,
                                 uint16_t numrecs, Node** out) {
  Node node;
  node.data.assign(geo_->block_size, 0);
  node.fsb = fsb;
  node.is_root = false;
  node.dirty = true;
  PutBE32(node.data.data(), kLMagic, ondisk::kBmapCrcMagic);
  PutBE16(node.data.data(), kLLevel, level);
  PutBE16(node.data.data(), kLNumrecs, numrecs);
  PutBE64(node.data.data(), kLLeftsib, kNullFsb);
  PutBE64(node.data.data(), kLRightSib, kNullFsb);
  PutBE64(node.data.data(), kLBlkno, geo_->FsbToByte(fsb) / 512);
  std::memcpy(node.data.data() + kLUuid, geo_->meta_uuid.data(), 16);
  PutBE64(node.data.data(), kLOwner, ino_);
  auto inserted = blocks_.emplace(fsb, std::move(node));
  *out = &inserted.first->second;
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Extent list operations
// ---------------------------------------------------------------------------

size_t DataForkEditor::FindExtentAt(uint64_t startoff) const {
  size_t lo = 0, hi = extents_.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (extents_[mid].startoff < startoff) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

Status DataForkEditor::ApplyInsert(const XfsExtent& rec) {
  if (format_ == di::kFormatBtree) {
    Status s = TreeInsert(rec);
    if (!s.ok()) return s;
  }
  const size_t pos = FindExtentAt(rec.startoff);
  extents_.insert(extents_.begin() + static_cast<ptrdiff_t>(pos), rec);
  nextents_ = static_cast<uint32_t>(extents_.size());
  return OkStatus();
}

Status DataForkEditor::ApplyRemove(const XfsExtent& rec) {
  if (format_ == di::kFormatBtree) {
    Status s = TreeRemove(rec);
    if (!s.ok()) return s;
  }
  const size_t pos = FindExtentAt(rec.startoff);
  if (pos >= extents_.size() || extents_[pos].startoff != rec.startoff ||
      extents_[pos].blockcount != rec.blockcount) {
    return XFS_ERR(XfsError::kInternal, "removing an unknown extent")
        .Ino(ino_)
        .Metadata("extent")
        .Actual(rec.startoff);
  }
  extents_.erase(extents_.begin() + static_cast<ptrdiff_t>(pos));
  nextents_ = static_cast<uint32_t>(extents_.size());
  return OkStatus();
}

Status DataForkEditor::ApplyUpdate(const XfsExtent& old_rec,
                                   const XfsExtent& new_rec) {
  if (format_ == di::kFormatBtree) {
    Status s = TreeUpdate(old_rec, new_rec);
    if (!s.ok()) return s;
  }
  const size_t pos = FindExtentAt(old_rec.startoff);
  if (pos >= extents_.size() || extents_[pos].startoff != old_rec.startoff) {
    return XFS_ERR(XfsError::kInternal, "updating an unknown extent")
        .Ino(ino_)
        .Actual(old_rec.startoff)
        .Metadata("extent");
  }
  extents_[pos] = new_rec;
  return OkStatus();
}

Status DataForkEditor::InsertExtent(const XfsExtent& extent) {
  if (!loaded_) {
    return XFS_ERR(XfsError::kInternal, "data fork editor is not loaded")
        .Ino(ino_)
        .Metadata("inode");
  }
  if (extent.blockcount == 0 || extent.blockcount > kMaxExtentLen) {
    return XFS_ERR(XfsError::kInvalidArgument, "invalid extent length")
        .Ino(ino_)
        .Actual(extent.blockcount)
        .Metadata("extent");
  }
  if (!geo_->FsbRangeValid(extent.startblock, extent.blockcount)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "extent does not lie inside one AG")
        .Ino(ino_)
        .Fsb(extent.startblock)
        .Metadata("extent");
  }
  // A local fork gains its first extent: xfs_bmap_local_to_extents.
  if (format_ == di::kFormatLocal) format_ = di::kFormatExtents;
  const size_t pos = FindExtentAt(extent.startoff);
  if (pos < extents_.size() &&
      extents_[pos].startoff < extent.startoff + extent.blockcount) {
    return XFS_ERR(XfsError::kInternal, "extent overlaps an existing extent")
        .Ino(ino_)
        .Actual(extents_[pos].startoff)
        .Metadata("extent");
  }
  if (pos > 0 &&
      extents_[pos - 1].startoff + extents_[pos - 1].blockcount >
          extent.startoff) {
    return XFS_ERR(XfsError::kInternal,
                   "extent overlaps the previous extent")
        .Ino(ino_)
        .Actual(extents_[pos - 1].startoff)
        .Metadata("extent");
  }

  // Merge with contiguous neighbours (xfs_bmap_add_extent_hole_real).
  XfsExtent left;
  bool have_left = false;
  if (pos > 0) {
    const XfsExtent& candidate = extents_[pos - 1];
    if (candidate.startoff + candidate.blockcount == extent.startoff &&
        candidate.startblock + candidate.blockcount == extent.startblock &&
        candidate.unwritten == extent.unwritten &&
        candidate.blockcount + extent.blockcount <= kMaxExtentLen) {
      left = candidate;
      have_left = true;
    }
  }
  XfsExtent right;
  bool have_right = false;
  if (pos < extents_.size()) {
    const XfsExtent& candidate = extents_[pos];
    if (extent.startoff + extent.blockcount == candidate.startoff &&
        extent.startblock + extent.blockcount == candidate.startblock &&
        candidate.unwritten == extent.unwritten &&
        candidate.blockcount + extent.blockcount <= kMaxExtentLen &&
        (!have_left ||
         left.blockcount + extent.blockcount + candidate.blockcount <=
             kMaxExtentLen)) {
      right = candidate;
      have_right = true;
    }
  }

  if (have_left && have_right) {
    XfsExtent merged = left;
    merged.blockcount += extent.blockcount + right.blockcount;
    Status s = ApplyRemove(right);
    if (!s.ok()) return s;
    s = ApplyUpdate(left, merged);
    if (!s.ok()) return s;
  } else if (have_left) {
    XfsExtent merged = left;
    merged.blockcount += extent.blockcount;
    Status s = ApplyUpdate(left, merged);
    if (!s.ok()) return s;
  } else if (have_right) {
    XfsExtent merged = extent;
    merged.blockcount += right.blockcount;
    Status s = ApplyRemove(right);
    if (!s.ok()) return s;
    s = ApplyInsert(merged);
    if (!s.ok()) return s;
  } else {
    Status s = ApplyInsert(extent);
    if (!s.ok()) return s;
  }
  return OkStatus();
}

Status DataForkEditor::RemoveRange(uint64_t first_block, uint64_t end_block) {
  if (!loaded_) {
    return XFS_ERR(XfsError::kInternal, "data fork editor is not loaded")
        .Ino(ino_)
        .Metadata("inode");
  }
  if (first_block >= end_block) return OkStatus();
  size_t pos = 0;
  while (pos < extents_.size()) {
    const XfsExtent old = extents_[pos];
    if (old.startoff >= end_block) break;
    if (old.startoff + old.blockcount <= first_block) {
      ++pos;
      continue;
    }
    const uint64_t overlap_start = std::max(old.startoff, first_block);
    const uint64_t overlap_end = std::min(old.EndBlock(), end_block);
    if (free_removed_ && overlap_start < overlap_end) {
      // Release the data blocks that leave the file (truncate).
      const uint64_t fsb = old.startblock + (overlap_start - old.startoff);
      Status s = alloc_->Free(fsb, static_cast<uint32_t>(overlap_end -
                                                        overlap_start));
      if (!s.ok()) return s;
      nblocks_delta_ -= static_cast<int64_t>(overlap_end - overlap_start);
    }
    if (overlap_start == old.startoff && overlap_end == old.EndBlock()) {
      Status s = ApplyRemove(old);
      if (!s.ok()) return s;
      continue;  // the next extent now sits at `pos`
    }
    if (old.startoff < first_block) {
      XfsExtent lower = old;
      lower.blockcount = first_block - old.startoff;
      Status s = ApplyUpdate(old, lower);
      if (!s.ok()) return s;
      ++pos;
      continue;
    }
    XfsExtent upper;
    upper.startoff = overlap_end;
    upper.startblock = old.startblock + (overlap_end - old.startoff);
    upper.blockcount = old.EndBlock() - overlap_end;
    upper.unwritten = old.unwritten;
    Status s = ApplyRemove(old);
    if (!s.ok()) return s;
    s = ApplyInsert(upper);
    if (!s.ok()) return s;
  }
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Tree edits
// ---------------------------------------------------------------------------

void DataForkEditor::FixKeysUpward(const std::vector<Frame>& path,
                                   const Node& child) {
  uint64_t key = FirstKey(child);
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    if (it->index >= NodeNumRecs(*it->node)) break;
    SetNodeKey(*it->node, it->index, key);
    key = NodeKey(*it->node, 0);
  }
}

Status DataForkEditor::TreeInsert(const XfsExtent& rec) {
  std::vector<Frame> path;
  Node* cur = &root_;
  while (NodeLevel(*cur) > 0) {
    const size_t idx = ChooseChild(*cur, rec.startoff);
    path.push_back({cur, idx});
    Node* child = FindNode(NodePtr(*cur, idx));
    if (child == nullptr) {
      return XFS_ERR(XfsError::kInternal, "bmbt child block missing")
          .Ino(ino_)
          .Fsb(NodePtr(*cur, idx))
          .Metadata("bmbt");
    }
    cur = child;
  }
  const uint16_t n = NodeNumRecs(*cur);
  size_t pos = 0;
  while (pos < n && NodeLeafRec(*cur, pos).startoff < rec.startoff) ++pos;
  const bool at_front = pos == 0;

  uint64_t child_fsb = 0;
  XfsExtent child_key;
  if (cur->is_root) {
    // The in-fork root is a level >= 1 node, so `cur` is a node here only if
    // the loop above ended immediately (root level 0) which the loader
    // refuses. Kept for completeness: insert into the root's entries.
    return XFS_ERR(XfsError::kInternal, "bmbt root is a leaf")
        .Ino(ino_)
        .Metadata("bmbt");
  }
  if (n < NodeMaxRecs(*cur)) {
    for (size_t i = n; i > pos; --i) {
      SetNodeLeafRec(*cur, i, NodeLeafRec(*cur, i - 1));
    }
    SetNodeLeafRec(*cur, pos, rec);
    SetNodeNumRecs(*cur, static_cast<uint16_t>(n + 1));
    if (at_front) FixKeysUpward(path, *cur);
    return OkStatus();
  }
  // Full leaf: distribute the max+1 records over this leaf and a new sibling.
  std::vector<Entry> entries;
  entries.reserve(n + 1);
  for (size_t i = 0; i < pos; ++i) entries.push_back({NodeLeafRec(*cur, i), 0});
  entries.push_back({rec, 0});
  for (size_t i = pos; i < n; ++i) entries.push_back({NodeLeafRec(*cur, i), 0});
  Status s = SplitLeaf(*cur, entries, &child_fsb, &child_key);
  if (!s.ok()) return s;
  if (at_front) FixKeysUpward(path, *cur);

  // Push the new child up the path.
  while (true) {
    if (path.empty()) {
      // The split block was a child of the in-fork root... which cannot be a
      // leaf, so this only happens when the root is the parent of `cur`.
      return XFS_ERR(XfsError::kInternal,
                     "bmbt leaf split without a parent")
          .Ino(ino_)
          .Metadata("bmbt");
    }
    Frame frame = path.back();
    path.pop_back();
    Node& parent = *frame.node;
    const uint16_t level = NodeLevel(parent);
    const uint16_t pn = NodeNumRecs(parent);
    const size_t insert_pos = frame.index + 1;
    if (pn < NodeMaxRecs(parent)) {
      for (size_t i = pn; i > insert_pos; --i) {
        SetNodeKey(parent, i, NodeKey(parent, i - 1));
        SetNodePtr(parent, i, NodePtr(parent, i - 1));
      }
      SetNodeKey(parent, insert_pos, child_key.startoff);
      SetNodePtr(parent, insert_pos, child_fsb);
      SetNodeNumRecs(parent, static_cast<uint16_t>(pn + 1));
      return OkStatus();
    }
    // Full node: distribute max+1 entries over this node and a new sibling.
    std::vector<Entry> node_entries;
    node_entries.reserve(pn + 1);
    for (size_t i = 0; i < insert_pos; ++i) {
      Entry e;
      e.rec.startoff = NodeKey(parent, i);
      e.ptr = NodePtr(parent, i);
      node_entries.push_back(e);
    }
    Entry new_entry;
    new_entry.rec.startoff = child_key.startoff;
    new_entry.ptr = child_fsb;
    node_entries.push_back(new_entry);
    for (size_t i = insert_pos; i < pn; ++i) {
      Entry e;
      e.rec.startoff = NodeKey(parent, i);
      e.ptr = NodePtr(parent, i);
      node_entries.push_back(e);
    }
    if (parent.is_root) {
      return GrowRoot(node_entries, level);
    }
    uint64_t right_fsb = 0;
    XfsExtent right_key;
    Status s = SplitNode(parent, node_entries, &right_fsb, &right_key);
    if (!s.ok()) return s;
    child_fsb = right_fsb;
    child_key = right_key;
  }
}

Status DataForkEditor::SplitLeaf(Node& leaf, const std::vector<Entry>& entries,
                                 uint64_t* right_fsb, XfsExtent* right_key) {
  const size_t left_count = entries.size() / 2;
  uint64_t fsb = 0;
  Status s = AllocBmbtBlock(&fsb);
  if (!s.ok()) return s;
  Node* right = nullptr;
  s = AllocNode(fsb, 0, static_cast<uint16_t>(entries.size() - left_count),
                &right);
  if (!s.ok()) return s;
  std::vector<Entry> left_entries(entries.begin(),
                                  entries.begin() +
                                      static_cast<ptrdiff_t>(left_count));
  std::vector<Entry> right_entries(entries.begin() +
                                       static_cast<ptrdiff_t>(left_count),
                                   entries.end());
  WriteEntries(leaf, left_entries);
  WriteEntries(*right, right_entries);
  const uint64_t old_right = GetBE64(leaf.data.data(), kLRightSib);
  PutBE64(right->data.data(), kLRightSib, old_right);
  PutBE64(right->data.data(), kLLeftsib, leaf.fsb);
  PutBE64(leaf.data.data(), kLRightSib, fsb);
  leaf.dirty = true;
  if (old_right != kNullFsb) {
    Node* orb = FindNode(old_right);
    if (orb != nullptr) {
      PutBE64(orb->data.data(), kLLeftsib, fsb);
      orb->dirty = true;
    }
  }
  *right_fsb = fsb;
  *right_key = right_entries.front().rec;
  return OkStatus();
}

Status DataForkEditor::SplitNode(Node& node, const std::vector<Entry>& entries,
                                 uint64_t* right_fsb, XfsExtent* right_key) {
  const uint16_t level = NodeLevel(node);
  const size_t left_count = entries.size() / 2;
  uint64_t fsb = 0;
  Status s = AllocBmbtBlock(&fsb);
  if (!s.ok()) return s;
  Node* right = nullptr;
  s = AllocNode(fsb, level,
                static_cast<uint16_t>(entries.size() - left_count), &right);
  if (!s.ok()) return s;
  std::vector<Entry> left_entries(entries.begin(),
                                  entries.begin() +
                                      static_cast<ptrdiff_t>(left_count));
  std::vector<Entry> right_entries(entries.begin() +
                                       static_cast<ptrdiff_t>(left_count),
                                   entries.end());
  WriteEntries(node, left_entries);
  WriteEntries(*right, right_entries);
  const uint64_t old_right = GetBE64(node.data.data(), kLRightSib);
  PutBE64(right->data.data(), kLRightSib, old_right);
  PutBE64(right->data.data(), kLLeftsib, node.fsb);
  PutBE64(node.data.data(), kLRightSib, fsb);
  node.dirty = true;
  if (old_right != kNullFsb) {
    Node* orb = FindNode(old_right);
    if (orb != nullptr) {
      PutBE64(orb->data.data(), kLLeftsib, fsb);
      orb->dirty = true;
    }
  }
  *right_fsb = fsb;
  *right_key = right_entries.front().rec;
  return OkStatus();
}

Status DataForkEditor::GrowRoot(const std::vector<Entry>& entries,
                                uint16_t child_level) {
  // The in-fork root holds `entries` (its maximum + 1): distribute them over
  // two new blocks and make the root a level+1 node over both.
  const size_t left_count = entries.size() / 2;
  uint64_t left_fsb = 0;
  uint64_t right_fsb = 0;
  Status s = AllocBmbtBlock(&left_fsb);
  if (!s.ok()) return s;
  s = AllocBmbtBlock(&right_fsb);
  if (!s.ok()) return s;
  Node* left = nullptr;
  Node* right = nullptr;
  s = AllocNode(left_fsb, child_level,
                static_cast<uint16_t>(left_count), &left);
  if (!s.ok()) return s;
  s = AllocNode(right_fsb, child_level,
                static_cast<uint16_t>(entries.size() - left_count), &right);
  if (!s.ok()) return s;
  std::vector<Entry> left_entries(entries.begin(),
                                  entries.begin() +
                                      static_cast<ptrdiff_t>(left_count));
  std::vector<Entry> right_entries(entries.begin() +
                                       static_cast<ptrdiff_t>(left_count),
                                   entries.end());
  WriteEntries(*left, left_entries);
  WriteEntries(*right, right_entries);
  PutBE64(left->data.data(), kLRightSib, right_fsb);
  PutBE64(right->data.data(), kLLeftsib, left_fsb);
  // The root grows by one level over its two new children.
  PutBE16(root_.data.data(), 0, static_cast<uint16_t>(child_level + 1));
  PutBE16(root_.data.data(), 2, 2);
  SetNodeKey(root_, 0, left_entries.front().rec.startoff);
  SetNodePtr(root_, 0, left_fsb);
  SetNodeKey(root_, 1, right_entries.front().rec.startoff);
  SetNodePtr(root_, 1, right_fsb);
  root_.dirty = true;
  return OkStatus();
}

Status DataForkEditor::TreeUpdate(const XfsExtent& old_rec,
                                  const XfsExtent& new_rec) {
  if (old_rec.startoff != new_rec.startoff) {
    return XFS_ERR(XfsError::kInternal, "bmbt update changed the key")
        .Ino(ino_)
        .Metadata("bmbt");
  }
  Node* cur = &root_;
  while (NodeLevel(*cur) > 0) {
    const size_t idx = ChooseChild(*cur, old_rec.startoff);
    Node* child = FindNode(NodePtr(*cur, idx));
    if (child == nullptr) {
      return XFS_ERR(XfsError::kInternal, "bmbt child block missing")
          .Ino(ino_)
          .Metadata("bmbt");
    }
    cur = child;
  }
  const uint16_t n = NodeNumRecs(*cur);
  for (size_t i = 0; i < n; ++i) {
    if (NodeLeafRec(*cur, i).startoff == old_rec.startoff) {
      SetNodeLeafRec(*cur, i, new_rec);
      return OkStatus();
    }
  }
  return XFS_ERR(XfsError::kInternal, "bmbt record to update not found")
      .Ino(ino_)
      .Actual(old_rec.startoff)
      .Metadata("bmbt");
}

Status DataForkEditor::TreeRemove(const XfsExtent& rec) {
  std::vector<Frame> path;
  Node* cur = &root_;
  while (NodeLevel(*cur) > 0) {
    const size_t idx = ChooseChild(*cur, rec.startoff);
    path.push_back({cur, idx});
    Node* child = FindNode(NodePtr(*cur, idx));
    if (child == nullptr) {
      return XFS_ERR(XfsError::kInternal, "bmbt child block missing")
          .Ino(ino_)
          .Fsb(NodePtr(*cur, idx))
          .Metadata("bmbt");
    }
    cur = child;
  }
  const uint16_t n = NodeNumRecs(*cur);
  size_t pos = n;
  for (size_t i = 0; i < n; ++i) {
    if (NodeLeafRec(*cur, i).startoff == rec.startoff) {
      pos = i;
      break;
    }
  }
  if (pos == n) {
    return XFS_ERR(XfsError::kInternal, "bmbt record to remove not found")
        .Ino(ino_)
        .Actual(rec.startoff)
        .Metadata("bmbt");
  }
  RemoveEntry(*cur, pos);
  if (pos == 0 && NodeNumRecs(*cur) > 0) FixKeysUpward(path, *cur);

  Node* node = cur;
  while (!node->is_root) {
    const uint16_t n_cur = NodeNumRecs(*node);
    if (n_cur >= NodeMinRecs(*node)) break;
    Frame frame = path.back();
    Node& parent = *frame.node;
    const size_t idx = frame.index;
    const uint16_t pn = NodeNumRecs(parent);
    Node* left = idx > 0 ? FindNode(NodePtr(parent, idx - 1)) : nullptr;
    Node* right = idx + 1 < pn ? FindNode(NodePtr(parent, idx + 1)) : nullptr;
    const uint16_t maxr = static_cast<uint16_t>(NodeMaxRecs(*node));
    if (left == nullptr && right == nullptr) {
      if (parent.is_root && pn == 1 && idx == 0) break;  // root's only child
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "cannot rebalance a bmbt block without siblings")
          .Ino(ino_)
          .Metadata("bmbt");
    }
    bool merged = false;
    if (left != nullptr && NodeNumRecs(*left) + n_cur <= maxr) {
      AppendEntries(*left, *node);
      const uint64_t rr = GetBE64(node->data.data(), kLRightSib);
      PutBE64(left->data.data(), kLRightSib, rr);
      if (rr != kNullFsb) {
        Node* rb = FindNode(rr);
        if (rb != nullptr) {
          PutBE64(rb->data.data(), kLLeftsib, left->fsb);
          rb->dirty = true;
        }
      }
      const uint64_t node_fsb = node->fsb;
      Status s = FreeBmbtBlock(node_fsb);
      if (!s.ok()) return s;
      blocks_.erase(node_fsb);
      RemoveEntry(parent, idx);
      merged = true;
    } else if (right != nullptr && NodeNumRecs(*right) + n_cur <= maxr) {
      AppendEntries(*node, *right);
      const uint64_t rr = GetBE64(right->data.data(), kLRightSib);
      PutBE64(node->data.data(), kLRightSib, rr);
      if (rr != kNullFsb) {
        Node* rb = FindNode(rr);
        if (rb != nullptr) {
          PutBE64(rb->data.data(), kLLeftsib, node->fsb);
          rb->dirty = true;
        }
      }
      const uint64_t right_fsb = right->fsb;
      Status s = FreeBmbtBlock(right_fsb);
      if (!s.ok()) return s;
      blocks_.erase(right_fsb);
      RemoveEntry(parent, idx + 1);
      merged = true;
    } else if (left != nullptr && NodeNumRecs(*left) > NodeMinRecs(*left)) {
      BorrowLast(*left, *node);
      SetNodeKey(parent, idx, FirstKey(*node));
      if (idx == 0) {
        std::vector<Frame> ppath(path.begin(), path.end() - 1);
        FixKeysUpward(ppath, parent);
      }
      break;
    } else if (right != nullptr &&
               NodeNumRecs(*right) > NodeMinRecs(*right)) {
      BorrowFirst(*right, *node);
      SetNodeKey(parent, idx + 1, FirstKey(*right));
      break;
    } else {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "cannot rebalance a bmbt block")
          .Ino(ino_)
          .Fsb(node->fsb)
          .Metadata("bmbt");
    }
    if (merged) {
      path.pop_back();
      node = frame.node;
    }
  }

  if (root_.is_root && NodeNumRecs(root_) == 1 && NodeLevel(root_) >= 2) {
    return KillRootChild();
  }
  return OkStatus();
}

void DataForkEditor::AppendEntries(Node& dst, const Node& src) {
  const uint16_t dn = NodeNumRecs(dst);
  const uint16_t sn = NodeNumRecs(src);
  const uint16_t level = NodeLevel(dst);
  for (uint16_t i = 0; i < sn; ++i) {
    if (level == 0) {
      SetNodeLeafRec(dst, dn + i, NodeLeafRec(src, i));
    } else {
      SetNodeKey(dst, dn + i, NodeKey(src, i));
      SetNodePtr(dst, dn + i, NodePtr(src, i));
    }
  }
  SetNodeNumRecs(dst, static_cast<uint16_t>(dn + sn));
}

void DataForkEditor::BorrowLast(Node& from, Node& to) {
  const uint16_t level = NodeLevel(to);
  const uint16_t from_n = NodeNumRecs(from);
  if (level == 0) {
    const XfsExtent moved = NodeLeafRec(from, from_n - 1);
    RemoveEntry(from, from_n - 1);
    for (size_t i = NodeNumRecs(to); i > 0; --i) {
      SetNodeLeafRec(to, i, NodeLeafRec(to, i - 1));
    }
    SetNodeLeafRec(to, 0, moved);
  } else {
    const uint64_t moved_key = NodeKey(from, from_n - 1);
    const uint64_t moved_ptr = NodePtr(from, from_n - 1);
    RemoveEntry(from, from_n - 1);
    for (size_t i = NodeNumRecs(to); i > 0; --i) {
      SetNodeKey(to, i, NodeKey(to, i - 1));
      SetNodePtr(to, i, NodePtr(to, i - 1));
    }
    SetNodeKey(to, 0, moved_key);
    SetNodePtr(to, 0, moved_ptr);
  }
  SetNodeNumRecs(to, static_cast<uint16_t>(NodeNumRecs(to) + 1));
}

void DataForkEditor::BorrowFirst(Node& from, Node& to) {
  const uint16_t level = NodeLevel(to);
  if (level == 0) {
    const XfsExtent moved = NodeLeafRec(from, 0);
    RemoveEntry(from, 0);
    SetNodeLeafRec(to, NodeNumRecs(to), moved);
  } else {
    const uint64_t moved_key = NodeKey(from, 0);
    const uint64_t moved_ptr = NodePtr(from, 0);
    RemoveEntry(from, 0);
    SetNodeKey(to, NodeNumRecs(to), moved_key);
    SetNodePtr(to, NodeNumRecs(to), moved_ptr);
  }
  SetNodeNumRecs(to, static_cast<uint16_t>(NodeNumRecs(to) + 1));
}

Status DataForkEditor::KillRootChild() {
  if (!root_.is_root) return OkStatus();
  const uint16_t level = NodeLevel(root_);
  if (level < 2) return OkStatus();
  if (NodeNumRecs(root_) != 1) return OkStatus();
  Node* child = FindNode(NodePtr(root_, 0));
  if (child == nullptr) {
    return XFS_ERR(XfsError::kInternal, "bmbt root child missing")
        .Ino(ino_)
        .Metadata("bmbt");
  }
  if (NodeLevel(*child) != level - 1) {
    return XFS_ERR(XfsError::kCorruptBtree, "bmbt child level mismatch")
        .Ino(ino_)
        .Metadata("bmbt");
  }
  const uint16_t cn = NodeNumRecs(*child);
  if (cn > NodeMaxRecs(root_)) {
    return OkStatus();  // cannot demote; the single child stays
  }
  PutBE16(root_.data.data(), 0, static_cast<uint16_t>(level - 1));
  PutBE16(root_.data.data(), 2, cn);
  for (size_t i = 0; i < cn; ++i) {
    SetNodeKey(root_, i, NodeKey(*child, i));
    SetNodePtr(root_, i, NodePtr(*child, i));
  }
  root_.dirty = true;
  const uint64_t child_fsb = child->fsb;
  Status s = FreeBmbtBlock(child_fsb);
  if (!s.ok()) return s;
  blocks_.erase(child_fsb);
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Format conversions
// ---------------------------------------------------------------------------

Status DataForkEditor::ExtentsToBtree() {
  if (extents_.empty() ||
      extents_.size() >
          static_cast<size_t>((geo_->block_size - kLongHeader) / kRecSize)) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "conversion to btree format is not supported here")
        .Ino(ino_)
        .Metadata("bmbt");
  }
  if (NodeMaxRecs(root_) == 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "inode fork is too small for a btree root")
        .Ino(ino_)
        .Metadata("bmbt");
  }
  root_.data.assign(fork_bytes_, 0);
  root_.is_root = true;
  uint64_t leaf_fsb = 0;
  Status s = AllocBmbtBlock(&leaf_fsb);
  if (!s.ok()) return s;
  Node* leaf = nullptr;
  s = AllocNode(leaf_fsb, 0, static_cast<uint16_t>(extents_.size()), &leaf);
  if (!s.ok()) return s;
  for (size_t i = 0; i < extents_.size(); ++i) {
    SetNodeLeafRec(*leaf, i, extents_[i]);
  }
  PutBE16(root_.data.data(), 0, 1);
  PutBE16(root_.data.data(), 2, 1);
  SetNodeKey(root_, 0, extents_[0].startoff);
  SetNodePtr(root_, 0, leaf_fsb);
  root_.dirty = true;
  format_ = di::kFormatBtree;
  return OkStatus();
}

Status DataForkEditor::CollapseToExtents() {
  for (auto& entry : blocks_) {
    Status s = FreeBmbtBlock(entry.first);
    if (!s.ok()) return s;
  }
  blocks_.clear();
  root_.data.clear();
  root_.is_root = true;
  root_.dirty = false;
  format_ = di::kFormatExtents;
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Finalize
// ---------------------------------------------------------------------------

Status DataForkEditor::Finalize(std::vector<uint8_t>* raw_inode,
                                uint32_t* ilog_fields,
                                std::vector<uint8_t>* fork_region) {
  if (!loaded_) {
    return XFS_ERR(XfsError::kInternal, "data fork editor is not loaded")
        .Ino(ino_)
        .Metadata("inode");
  }
  if (raw_inode->size() < geo_->inode_size) {
    return XFS_ERR(XfsError::kInternal, "inode image too small")
        .Ino(ino_)
        .Metadata("inode");
  }
  const size_t literal = fork_bytes_;
  const size_t capacity = literal / kRecSize;
  const bool needs_btree = extents_.size() > capacity;

  if (format_ == di::kFormatExtents && needs_btree) {
    Status s = ExtentsToBtree();
    if (!s.ok()) return s;
  } else if (format_ == di::kFormatBtree && !needs_btree) {
    Status s = CollapseToExtents();
    if (!s.ok()) return s;
  }

  uint8_t* core = raw_inode->data();
  // di_nextents/di_big_nextents share a union; write the field selected by
  // the inode's own NREXT64 flag (the attr-fork counter is left untouched).
  SetInodeDataExtentCount(core, extents_.size());
  PutBE64(core, di::kNblocks,
          GetBE64(core, di::kNblocks) +
              static_cast<uint64_t>(nblocks_delta_));
  core[di::kFormat] = format_;
  uint8_t* fork = core + di::kCoreSizeV3;

  if (format_ == di::kFormatExtents) {
    std::memset(fork, 0, literal);
    for (size_t i = 0; i < extents_.size(); ++i) {
      ondisk::BmbtRec r;
      r.startoff = extents_[i].startoff;
      r.startblock = extents_[i].startblock;
      r.blockcount = extents_[i].blockcount;
      r.unwritten = extents_[i].unwritten;
      uint64_t l0 = 0, l1 = 0;
      ondisk::EncodeBmbtRec(r, &l0, &l1);
      PutBE64(fork, i * kRecSize, l0);
      PutBE64(fork, i * kRecSize + 8, l1);
    }
    fork_region->clear();
    if (!extents_.empty()) {
      fork_region->assign(extents_.size() * kRecSize, 0);
      std::memcpy(fork_region->data(), fork, fork_region->size());
    }
    *ilog_fields = lf::kILogDExt;
    return OkStatus();
  }

  // Btree format: the fork keeps the root in the on-disk bmdr layout; the log
  // region is the in-core root block (72-byte header + keys + pointers).
  std::memcpy(fork, root_.data.data(), std::min(literal, root_.data.size()));
  const uint16_t level = GetBE16(root_.data.data(), 0);
  const uint16_t numrecs = GetBE16(root_.data.data(), 2);
  std::vector<uint8_t> broot(kLongHeader + numrecs * (kKeySize + kPtrSize), 0);
  PutBE32(broot.data(), kLMagic, ondisk::kBmapCrcMagic);
  PutBE16(broot.data(), kLLevel, level);
  PutBE16(broot.data(), kLNumrecs, numrecs);
  PutBE64(broot.data(), kLLeftsib, kNullFsb);
  PutBE64(broot.data(), kLRightSib, kNullFsb);
  PutBE64(broot.data(), kLBlkno, ~0ull);  // XFS_BUF_DADDR_NULL
  PutBE64(broot.data(), kLLsn, 0);
  std::memcpy(broot.data() + kLUuid, geo_->meta_uuid.data(), 16);
  PutBE64(broot.data(), kLOwner, ino_);
  for (size_t i = 0; i < numrecs; ++i) {
    PutBE64(broot.data(), kLongHeader + i * kKeySize, NodeKey(root_, i));
    // In the in-core form the pointer array starts right after the keys
    // (xfs_bmap_broot_space_calc: 72 + numrecs * 16).
    PutBE64(broot.data(), kLongHeader + numrecs * kKeySize + i * kPtrSize,
            NodePtr(root_, i));
  }
  *fork_region = std::move(broot);
  *ilog_fields = lf::kILogDBRoot;

  for (auto& entry : blocks_) {
    Node& node = entry.second;
    if (!node.dirty) continue;
    Status s = txn_->AddMetadataBuffer(geo_->FsbToByte(node.fsb), node.data,
                                       lf::kBlftBtree, kLLsn, kLCrc, "bmbt");
    if (!s.ok()) return s;
  }
  return OkStatus();
}

}  // namespace xfs
