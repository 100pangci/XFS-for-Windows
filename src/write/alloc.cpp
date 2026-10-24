// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - allocation group block allocator (RW-2).
//
// On-disk rules implemented here (verified against fs/xfs/libxfs and xfsprogs;
// see docs/ALLOCATOR.md):
//
//   - a free space extent lives in two trees over the same records: bnobt
//     ordered by start block, cntbt ordered by (block count, start block).
//     Both trees are always updated together.
//   - allocation takes the best fitting extent (smallest length >= request,
//     tie: lowest start) and carves the requested run from the *start* of it
//     (xfs_alloc_ag_vextent_size with alignment 1).
//   - freeing merges contiguous neighbours on both sides into one record.
//   - AGF counters: freeblks = sum of the records, longest = largest record,
//     btreeblks = non-root blocks of both trees, flcount/flfirst/fllast = the
//     AGFL ring. Free space btree blocks come from the AGFL.
//   - superblock: sb_fdblocks changes with the AG free space, where the AGFL
//     and the non-root free space btree blocks count as free (the kernel's
//     fdblocks = freeblks + flcount + btreeblks identity, which
//     xfs_initialize_perag_data and xfs_repair both compute).
//   - every non-root block keeps at least half of its maximum records (what
//     xfs_repair requires) through split, merge and borrow.
//
// Nothing is written until Commit(): if any step fails, the whole in-memory
// state is discarded and the transaction is never committed, so a refused
// allocation cannot leave a half-edited tree behind.
#include "xfs/alloc.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "xfs/ag.h"
#include "xfs/btree.h"
#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/log_format.h"
#include "xfs/ondisk.h"

namespace xfs {
namespace {

namespace agf = ondisk::agf;
namespace agfl = ondisk::agfl;
namespace bt = ondisk::btree;
namespace sb = ondisk::sb;
namespace lf = logfmt;

constexpr uint32_t kNullAgBlock = 0xffffffffu;
constexpr size_t kShortHeader = bt::kSHeaderLenV5;  // 56 bytes
constexpr size_t kRecSize = bt::kAllocRecSize;      // 8 bytes (leaf record)
constexpr size_t kKeySize = bt::kAllocKeySize;      // 8 bytes (node key)
constexpr size_t kPtrSize = bt::kAllocPtrSize;      // 4 bytes (node pointer)
// Cap on the number of tree blocks this build will load and edit.
constexpr size_t kMaxTreeBlocks = 8192;

// One free space record (host order).
struct FreeRec {
  uint32_t start = 0;
  uint32_t len = 0;
};

// One btree entry: a leaf record, or a node's key/pointer pair.
struct Entry {
  FreeRec key;
  uint32_t ptr = kNullAgBlock;
};

enum class TreeKind { kBno, kCnt };

// Ordering: bnobt by (start, len), cntbt by (len, start). Record order and key
// order use the same composite order; the kernel's own checks
// (xfs_allocbt_{recs,keys}_inorder) compare only the leading field, which is
// still consistent with this.
int CompareRec(TreeKind kind, const FreeRec& a, const FreeRec& b) {
  if (kind == TreeKind::kBno) {
    if (a.start != b.start) return a.start < b.start ? -1 : 1;
    if (a.len != b.len) return a.len < b.len ? -1 : 1;
    return 0;
  }
  if (a.len != b.len) return a.len < b.len ? -1 : 1;
  if (a.start != b.start) return a.start < b.start ? -1 : 1;
  return 0;
}

class Allocator;

// ---------------------------------------------------------------------------
// One free space btree of one AG, editable in memory.
// ---------------------------------------------------------------------------
class AllocTree {
 public:
  AllocTree(Allocator* owner, uint32_t agno, TreeKind kind, const char* name)
      : owner_(owner), agno_(agno), kind_(kind), name_(name) {}

  Status Load(uint32_t root, uint32_t level);
  Status CollectRecords(std::vector<FreeRec>* out) const;
  Status Insert(const FreeRec& rec);
  Status Remove(const FreeRec& rec);

  uint32_t root() const { return root_; }
  uint32_t level() const { return level_; }
  size_t block_count() const { return blocks_.size(); }
  bool dirty() const { return !dirty_.empty(); }
  const std::set<uint32_t>& dirty_blocks() const { return dirty_; }
  const std::map<uint32_t, std::vector<uint8_t>>& blocks() const {
    return blocks_;
  }
  uint32_t max_leaf() const {
    return static_cast<uint32_t>((owner_block_size() - kShortHeader) / kRecSize);
  }
  uint32_t max_node() const {
    return static_cast<uint32_t>((owner_block_size() - kShortHeader) /
                                 (kKeySize + kPtrSize));
  }

 private:
  struct Frame {
    uint32_t block = 0;
    size_t index = 0;  // 0-based child index inside that block
  };

  uint32_t owner_block_size() const;
  uint8_t* MutableBlock(uint32_t agbno);
  const uint8_t* Block(uint32_t agbno) const;
  uint32_t LevelOf(uint32_t agbno) const {
    return GetBE16(Block(agbno), bt::kLevel);
  }
  uint32_t NumRecs(uint32_t agbno) const {
    return GetBE16(Block(agbno), bt::kNumrecs);
  }
  uint32_t NumRecs(const uint8_t* b) const { return GetBE16(b, bt::kNumrecs); }
  void SetNumRecs(uint8_t* b, uint32_t n) {
    PutBE16(b, bt::kNumrecs, static_cast<uint16_t>(n));
  }
  FreeRec RecAt(const uint8_t* b, size_t i) const {
    FreeRec r;
    r.start = GetBE32(b, kShortHeader + i * kRecSize);
    r.len = GetBE32(b, kShortHeader + i * kRecSize + 4);
    return r;
  }
  void SetRecAt(uint8_t* b, size_t i, const FreeRec& r) {
    PutBE32(b, kShortHeader + i * kRecSize, r.start);
    PutBE32(b, kShortHeader + i * kRecSize + 4, r.len);
  }
  FreeRec KeyAt(const uint8_t* b, size_t i) const {
    FreeRec r;
    r.start = GetBE32(b, kShortHeader + i * kKeySize);
    r.len = GetBE32(b, kShortHeader + i * kKeySize + 4);
    return r;
  }
  void SetKeyAt(uint8_t* b, size_t i, const FreeRec& r) {
    PutBE32(b, kShortHeader + i * kKeySize, r.start);
    PutBE32(b, kShortHeader + i * kKeySize + 4, r.len);
  }
  uint32_t PtrAt(const uint8_t* b, size_t i) const {
    return GetBE32(b, kShortHeader + max_node() * kKeySize + i * kPtrSize);
  }
  void SetPtrAt(uint8_t* b, size_t i, uint32_t v) {
    PutBE32(b, kShortHeader + max_node() * kKeySize + i * kPtrSize, v);
  }
  uint32_t LeftSib(const uint8_t* b) const { return GetBE32(b, bt::kSLeftsib); }
  uint32_t RightSib(const uint8_t* b) const {
    return GetBE32(b, bt::kSRightSib);
  }
  void SetLeftSib(uint8_t* b, uint32_t v) { PutBE32(b, bt::kSLeftsib, v); }
  void SetRightSib(uint8_t* b, uint32_t v) { PutBE32(b, bt::kSRightSib, v); }

  FreeRec FirstKey(uint32_t agbno) const {
    return LevelOf(agbno) == 0 ? RecAt(Block(agbno), 0)
                               : KeyAt(Block(agbno), 0);
  }
  size_t ChooseChild(const uint8_t* b, const FreeRec& key) const;
  void FixKeysUpward(const std::vector<Frame>& path, uint32_t child);
  // Reads all entries of a block (leaf records or node key/ptr pairs).
  std::vector<Entry> ReadEntries(uint32_t agbno) const;
  void WriteEntries(uint32_t agbno, uint32_t level,
                    const std::vector<Entry>& entries);
  Status InitBlock(uint32_t agbno, uint32_t level, uint32_t numrecs);
  // Splits `entries` (exactly max+1 of them) between `left` (in place) and a
  // newly allocated right block; returns the right block and its first key.
  Status SplitEntries(uint32_t left, uint32_t level,
                      const std::vector<Entry>& entries, uint32_t* right_out,
                      FreeRec* right_key);
  Status NewRoot(uint32_t left, uint32_t right);

  Allocator* owner_;
  uint32_t agno_;
  TreeKind kind_;
  const char* name_;
  uint32_t root_ = 0;
  uint32_t level_ = 1;
  std::map<uint32_t, std::vector<uint8_t>> blocks_;
  std::set<uint32_t> dirty_;
};

// ---------------------------------------------------------------------------
// One AG's allocator state.
// ---------------------------------------------------------------------------
struct AgState {
  Allocator* owner = nullptr;
  uint32_t agno = 0;
  uint32_t length = 0;
  uint32_t agfl_size = 0;

  std::vector<uint8_t> agf_buf;
  std::vector<uint8_t> agfl_buf;
  bool agf_dirty = false;
  bool agfl_dirty = false;

  uint32_t flfirst = 0, fllast = 0, flcount = 0;
  uint32_t freeblks = 0;
  uint32_t longest = 0;
  uint32_t orig_freeblks = 0, orig_flcount = 0, orig_btreeblks = 0;
  uint32_t rmap_root = 0, rmap_level = 0, rmap_blocks = 0;

  std::vector<FreeRec> recs;  // sorted by start, disjoint
  std::unique_ptr<AllocTree> bno, cnt;

  bool refilling = false;
  bool counted = false;
  bool rmap_loaded = false;

  Status Load(Allocator* allocator, uint32_t agno_in);
  Status InsertRec(const FreeRec& rec);
  Status RemoveRec(const FreeRec& rec);
  Status GetTreeBlock(uint32_t* agbno);
  Status PutTreeBlock(uint32_t agbno);
  // Removes free extents and pushes their blocks onto the AGFL without any
  // ownership change.  The caller maps the added ranges OWN_AG afterwards;
  // doing it in this order is what keeps the rmap tree's own split from
  // recursing into an empty freelist.
  Status RefillAgflRaw(uint32_t want, std::vector<FreeRec>* added);
  // Moves AGFL entries back to ordinary free space, removing OWN_AG first.
  Status DrainAgfl(uint32_t count);
  Status AllocateBestFit(uint32_t want, const RmapOwner* owner,
                         Allocation* out);
  Status AllocateAligned(uint32_t want, uint32_t align,
                         const RmapOwner* owner, Allocation* out,
                         uint32_t min_agbno = 0,
                         uint32_t max_agbno = 0xffffffffu);
  void RecomputeLongest();
  uint32_t Contribution() const;
};

// ---------------------------------------------------------------------------
// The allocator itself.
// ---------------------------------------------------------------------------
class Allocator {
 public:
  Allocator(XfsTransaction* txn, BlockDevice* device, const XfsGeometry* geo);

  XfsTransaction* txn() { return txn_; }
  BlockDevice* device() { return device_; }
  const XfsGeometry* geo() const { return geo_; }
  uint32_t block_size() const { return geo_->block_size; }
  uint64_t FsbToByte(uint64_t fsb) const { return geo_->FsbToByte(fsb); }

  Status LoadAg(uint32_t agno, AgState** out);
  AgState* Ag(uint32_t agno) { return ags_.at(agno).get(); }
  Status AddBuffer(uint64_t offset, std::vector<uint8_t> data, uint16_t type,
                   size_t lsn_offset, size_t crc_offset, const char* what);
  Status Commit();
  int64_t fdblocks_delta() const { return fdblocks_delta_; }

  bool rmap_enabled() const { return geo_->features.rmapbt; }
  bool rmap_editing() const { return rmap_editing_; }
  uint32_t RmapBlocks(uint32_t agno) const;
  Status MapOwned(uint32_t agno, uint32_t agbno, uint32_t len,
                  const RmapOwner& owner);
  Status UnmapOwned(uint32_t agno, uint32_t agbno, uint32_t len,
                    const RmapOwner& owner, bool unknown_owner);
  Status ConvertOwnedRmap(uint32_t agno, uint32_t agbno, uint32_t len,
                          const RmapOwner& owner, bool old_unwritten);
  Status MapMetadata(uint32_t agno, uint32_t agbno, uint32_t len,
                     uint64_t owner);
  Status UnmapMetadata(uint32_t agno, uint32_t agbno, uint32_t len,
                       uint64_t owner);
  Status AllocateRmapBlock(uint32_t agno, uint32_t* agbno);
  Status FreeRmapBlock(uint32_t agno, uint32_t agbno);
  Status ReserveForRmap(uint32_t agno);
  Status EnsureAgflInternal(uint32_t agno, uint32_t want);
  Status FreeBlocksInternal(uint64_t fsb, uint32_t len, const RmapOwner* owner,
                            bool unknown_owner);

 private:
  struct OutBuffer {
    uint64_t offset = 0;
    std::vector<uint8_t> data;
    uint16_t type = 0;
    size_t lsn_offset = 0;
    size_t crc_offset = 0;
    const char* what = "metadata";
  };
  XfsTransaction* txn_;
  BlockDevice* device_;
  const XfsGeometry* geo_;
  std::map<uint32_t, std::unique_ptr<AgState>> ags_;
  std::map<uint64_t, OutBuffer> buffers_;
  std::unique_ptr<RmapEditor> rmap_;
  bool rmap_editing_ = false;
  int64_t fdblocks_delta_ = 0;
};

Allocator::Allocator(XfsTransaction* txn, BlockDevice* device,
                     const XfsGeometry* geo)
    : txn_(txn), device_(device), geo_(geo) {
  if (!geo_->features.rmapbt) return;
  RmapEditor::Hooks hooks;
  hooks.alloc_block = [this](uint32_t agno, uint32_t* agbno) {
    return AllocateRmapBlock(agno, agbno);
  };
  hooks.free_block = [this](uint32_t agno, uint32_t agbno) {
    return FreeRmapBlock(agno, agbno);
  };
  hooks.publish_block = [this](uint32_t agno, uint32_t agbno,
                               const uint8_t* data, size_t len) {
    std::vector<uint8_t> buffer(data, data + len);
    const uint64_t offset = FsbToByte(geo_->AgFsb(agno, agbno));
    return txn_->AddMetadataBuffer(offset, std::move(buffer), lf::kBlftBtree,
                                   bt::kSLsn, bt::kSCrc, "rmapbt");
  };
  rmap_ = std::make_unique<RmapEditor>(device_, geo_, std::move(hooks));
}

// ---------------------------------------------------------------------------
// AllocTree
// ---------------------------------------------------------------------------

uint32_t AllocTree::owner_block_size() const { return owner_->block_size(); }

const uint8_t* AllocTree::Block(uint32_t agbno) const {
  auto it = blocks_.find(agbno);
  return it == blocks_.end() ? nullptr : it->second.data();
}

uint8_t* AllocTree::MutableBlock(uint32_t agbno) {
  auto it = blocks_.find(agbno);
  if (it == blocks_.end()) return nullptr;
  dirty_.insert(agbno);
  return it->second.data();
}

size_t AllocTree::ChooseChild(const uint8_t* b, const FreeRec& key) const {
  const uint32_t n = NumRecs(b);
  size_t chosen = 0;
  for (size_t i = 0; i < n; ++i) {
    if (CompareRec(kind_, KeyAt(b, i), key) <= 0) {
      chosen = i;
    } else {
      break;
    }
  }
  return chosen;
}

void AllocTree::FixKeysUpward(const std::vector<Frame>& path, uint32_t child) {
  uint32_t cur = child;
  FreeRec key = FirstKey(cur);
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    uint8_t* parent = MutableBlock(it->block);
    if (parent == nullptr || NumRecs(parent) == 0 ||
        it->index >= NumRecs(parent)) {
      break;
    }
    SetKeyAt(parent, it->index, key);
    cur = it->block;
    key = KeyAt(parent, 0);
  }
  (void)cur;
}

std::vector<Entry> AllocTree::ReadEntries(uint32_t agbno) const {
  const uint8_t* b = Block(agbno);
  const uint32_t n = NumRecs(b);
  const uint32_t level = GetBE16(b, bt::kLevel);
  std::vector<Entry> out;
  out.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    Entry entry;
    if (level == 0) {
      entry.key = RecAt(b, i);
    } else {
      entry.key = KeyAt(b, i);
      entry.ptr = PtrAt(b, i);
    }
    out.push_back(entry);
  }
  return out;
}

void AllocTree::WriteEntries(uint32_t agbno, uint32_t level,
                             const std::vector<Entry>& entries) {
  uint8_t* b = MutableBlock(agbno);
  SetNumRecs(b, static_cast<uint32_t>(entries.size()));
  for (size_t i = 0; i < entries.size(); ++i) {
    if (level == 0) {
      SetRecAt(b, i, entries[i].key);
    } else {
      SetKeyAt(b, i, entries[i].key);
      SetPtrAt(b, i, entries[i].ptr);
    }
  }
}

Status AllocTree::InitBlock(uint32_t agbno, uint32_t level, uint32_t numrecs) {
  const XfsGeometry* geo = owner_->geo();
  auto it = blocks_.find(agbno);
  if (it == blocks_.end()) {
    return XFS_ERR(XfsError::kInternal, "initializing an unloaded tree block")
        .Ag(agno_)
        .Metadata(name_)
        .Actual(agbno);
  }
  std::vector<uint8_t>& data = it->second;
  std::memset(data.data(), 0, data.size());
  PutBE32(data.data(), bt::kMagic,
          kind_ == TreeKind::kBno ? ondisk::kBtbCrcMagic
                                  : ondisk::kBtcCrcMagic);
  PutBE16(data.data(), bt::kLevel, static_cast<uint16_t>(level));
  PutBE16(data.data(), bt::kNumrecs, static_cast<uint16_t>(numrecs));
  PutBE32(data.data(), bt::kSLeftsib, kNullAgBlock);
  PutBE32(data.data(), bt::kSRightSib, kNullAgBlock);
  PutBE64(data.data(), bt::kSBlkno,
          owner_->FsbToByte(geo->AgFsb(agno_, agbno)) / 512);
  PutBE64(data.data(), bt::kSLsn, 0);  // stamped at commit
  std::memcpy(data.data() + bt::kSUuid, geo->meta_uuid.data(), 16);
  PutBE32(data.data(), bt::kSOwner, agno_);
  StoreLE32(data.data() + bt::kSCrc, 0);  // stamped at commit
  dirty_.insert(agbno);
  return OkStatus();
}

Status AllocTree::Load(uint32_t root, uint32_t level) {
  root_ = root;
  level_ = level;  // tree height: 1 = a single leaf root (AGF "levels")
  const XfsGeometry* geo = owner_->geo();
  if (level < 1 || level > bt::kMaxTraversalLevels) {
    return XFS_ERR(XfsError::kCorruptBtree, "free space btree level out of range")
        .Ag(agno_)
        .Metadata(name_)
        .Actual(level);
  }
  std::vector<uint32_t> stack{root};
  std::set<uint32_t> seen;
  while (!stack.empty()) {
    const uint32_t agbno = stack.back();
    stack.pop_back();
    if (agbno == kNullAgBlock || agbno >= geo->AgBlocksFor(agno_)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "free space btree pointer out of range")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(agbno);
    }
    if (!seen.insert(agbno).second) {
      return XFS_ERR(XfsError::kCorruptBtree, "free space btree cycle")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(agbno);
    }
    if (seen.size() > kMaxTreeBlocks) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "free space btree has more blocks than this build can edit")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(seen.size());
    }
    std::vector<uint8_t> data(geo->block_size, 0);
    const uint64_t offset = owner_->FsbToByte(geo->AgFsb(agno_, agbno));
    if (!owner_->device()->Read(offset, data.data(), data.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to read free space btree block")
          .Ag(agno_)
          .Offset(offset)
          .Metadata(name_);
    }
    const uint32_t magic = GetBE32(data.data(), bt::kMagic);
    const uint32_t expected = kind_ == TreeKind::kBno ? ondisk::kBtbCrcMagic
                                                      : ondisk::kBtcCrcMagic;
    if (magic != expected) {
      return XFS_ERR(XfsError::kBadMagic, "free space btree magic mismatch")
          .Ag(agno_)
          .Metadata(name_)
          .Expected(expected)
          .Actual(magic);
    }
    const uint32_t blk_level = GetBE16(data.data(), bt::kLevel);
    const uint32_t numrecs = GetBE16(data.data(), bt::kNumrecs);
    if (blk_level > bt::kMaxTraversalLevels) {
      return XFS_ERR(XfsError::kCorruptBtree, "free space btree level too deep")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(blk_level);
    }
    const uint32_t maxrecs = blk_level == 0 ? max_leaf() : max_node();
    const bool empty_root_leaf =
        numrecs == 0 && blk_level == 0 && agbno == root;
    if ((numrecs == 0 && !empty_root_leaf) || numrecs > maxrecs) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "free space btree record count out of range")
          .Ag(agno_)
          .Metadata(name_)
          .Expected(maxrecs)
          .Actual(numrecs);
    }
    if (blk_level > 0) {
      for (uint32_t i = 0; i < numrecs; ++i) {
        stack.push_back(GetBE32(data.data(), kShortHeader + max_node() * kKeySize +
                                                i * kPtrSize));
      }
    }
    blocks_.emplace(agbno, std::move(data));
  }
  if (LevelOf(root_) != level_ - 1) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "free space btree root level does not match the AGF")
        .Ag(agno_)
        .Metadata(name_)
        .Expected(level_ - 1)
        .Actual(LevelOf(root_));
  }
  uint32_t cur = root_;
  while (LevelOf(cur) > 0) cur = PtrAt(Block(cur), 0);
  if (LeftSib(Block(cur)) != kNullAgBlock) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "leftmost free space btree leaf has a left sibling")
        .Ag(agno_)
        .Metadata(name_);
  }
  return OkStatus();
}

Status AllocTree::CollectRecords(std::vector<FreeRec>* out) const {
  uint32_t cur = root_;
  while (LevelOf(cur) > 0) cur = PtrAt(Block(cur), 0);
  std::set<uint32_t> seen;
  FreeRec prev;
  bool first = true;
  while (cur != kNullAgBlock) {
    if (!seen.insert(cur).second) {
      return XFS_ERR(XfsError::kCorruptBtree, "free space btree leaf cycle")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(cur);
    }
    const uint8_t* b = Block(cur);
    if (b == nullptr) {
      return XFS_ERR(XfsError::kCorruptBtree, "free space btree leaf missing")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(cur);
    }
    const uint32_t n = NumRecs(b);
    for (uint32_t i = 0; i < n; ++i) {
      const FreeRec r = RecAt(b, i);
      if (r.len == 0 || r.start >= owner_->geo()->AgBlocksFor(agno_) ||
          r.len > owner_->geo()->AgBlocksFor(agno_) - r.start) {
        return XFS_ERR(XfsError::kCorruptBtree, "invalid free space record")
            .Ag(agno_)
            .Metadata(name_)
            .Actual(r.start);
      }
      if (!first) {
        if (CompareRec(kind_, prev, r) >= 0) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "free space records out of order")
              .Ag(agno_)
              .Metadata(name_)
              .Actual(r.start);
        }
        // Extents must be disjoint. Only the bnobt is ordered by start block,
        // so only there are adjacent records comparable this way (the cntbt is
        // ordered by length and its records cover the same extents).
        if (kind_ == TreeKind::kBno && r.start < prev.start + prev.len) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "free space records overlap")
              .Ag(agno_)
              .Metadata(name_)
              .Actual(r.start);
        }
      }
      first = false;
      prev = r;
      out->push_back(r);
    }
    cur = GetBE32(b, bt::kSRightSib);
    if (cur != kNullAgBlock && Block(cur) == nullptr) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "free space btree sibling not loaded")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(cur);
    }
  }
  return OkStatus();
}

Status AllocTree::SplitEntries(uint32_t left, uint32_t level,
                               const std::vector<Entry>& entries,
                               uint32_t* right_out, FreeRec* right_key) {
  // entries has max+1 records: the left half stays, the right half moves into
  // a newly allocated block.
  const size_t left_count = entries.size() / 2;
  const size_t right_count = entries.size() - left_count;
  uint32_t right = kNullAgBlock;
  Status s = owner_->Ag(agno_)->GetTreeBlock(&right);
  if (!s.ok()) return s;
  blocks_.emplace(right, std::vector<uint8_t>(owner_->block_size(), 0));
  s = InitBlock(right, level, static_cast<uint32_t>(right_count));
  if (!s.ok()) return s;
  std::vector<Entry> left_entries(entries.begin(),
                                  entries.begin() +
                                      static_cast<ptrdiff_t>(left_count));
  std::vector<Entry> right_entries(entries.begin() +
                                       static_cast<ptrdiff_t>(left_count),
                                   entries.end());
  WriteEntries(left, level, left_entries);
  WriteEntries(right, level, right_entries);
  // Sibling links.
  const uint32_t old_right = RightSib(Block(left));
  uint8_t* rb = MutableBlock(right);
  SetRightSib(rb, old_right);
  SetLeftSib(rb, left);
  uint8_t* lb = MutableBlock(left);
  SetRightSib(lb, right);
  if (old_right != kNullAgBlock) {
    uint8_t* orb = MutableBlock(old_right);
    if (orb != nullptr) SetLeftSib(orb, right);
  }
  *right_out = right;
  *right_key = right_entries.empty() ? FreeRec{} : right_entries[0].key;
  return OkStatus();
}

Status AllocTree::NewRoot(uint32_t left, uint32_t right) {
  uint32_t new_root = kNullAgBlock;
  Status s = owner_->Ag(agno_)->GetTreeBlock(&new_root);
  if (!s.ok()) return s;
  blocks_.emplace(new_root, std::vector<uint8_t>(owner_->block_size(), 0));
  s = InitBlock(new_root, level_, 2);
  if (!s.ok()) return s;
  uint8_t* b = blocks_[new_root].data();
  SetKeyAt(b, 0, FirstKey(left));
  SetKeyAt(b, 1, FirstKey(right));
  SetPtrAt(b, 0, left);
  SetPtrAt(b, 1, right);
  root_ = new_root;
  level_ += 1;
  return OkStatus();
}

Status AllocTree::Insert(const FreeRec& rec) {
  if (rec.len == 0) {
    return XFS_ERR(XfsError::kInternal, "inserting an empty free extent")
        .Ag(agno_)
        .Metadata(name_);
  }
  // Descend to the leaf.
  std::vector<Frame> path;
  uint32_t cur = root_;
  uint32_t lvl = level_ - 1;  // root block level
  while (lvl > 0) {
    const uint8_t* b = Block(cur);
    if (b == nullptr) {
      return XFS_ERR(XfsError::kInternal, "free space btree block lost")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(cur);
    }
    const size_t idx = ChooseChild(b, rec);
    path.push_back({cur, idx});
    cur = PtrAt(b, idx);
    lvl--;
  }
  uint8_t* leaf = MutableBlock(cur);
  if (leaf == nullptr) {
    return XFS_ERR(XfsError::kInternal, "free space btree leaf lost")
        .Ag(agno_)
        .Metadata(name_)
        .Actual(cur);
  }
  const uint32_t n = NumRecs(leaf);
  size_t pos = 0;
  while (pos < n && CompareRec(kind_, RecAt(leaf, pos), rec) < 0) ++pos;
  if (pos < n && CompareRec(kind_, RecAt(leaf, pos), rec) == 0) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "free space record already present")
        .Ag(agno_)
        .Metadata(name_)
        .Actual(rec.start);
  }

  uint32_t child_block = cur;
  FreeRec child_key = rec;
  if (n < max_leaf()) {
    // Room: shift the tail up and store the record.
    for (size_t i = n; i > pos; --i) SetRecAt(leaf, i, RecAt(leaf, i - 1));
    SetRecAt(leaf, pos, rec);
    SetNumRecs(leaf, n + 1);
    if (pos == 0) FixKeysUpward(path, cur);
    return OkStatus();
  }
  // Full leaf: distribute the max+1 records over this leaf and a new right
  // sibling, then push the new child up the path.
  std::vector<Entry> entries;
  entries.reserve(n + 1);
  for (size_t i = 0; i < pos; ++i) entries.push_back({RecAt(leaf, i), 0});
  entries.push_back({rec, 0});
  for (size_t i = pos; i < n; ++i) entries.push_back({RecAt(leaf, i), 0});
  uint32_t right = kNullAgBlock;
  FreeRec right_key;
  Status s = SplitEntries(cur, 0, entries, &right, &right_key);
  if (!s.ok()) return s;
  if (pos == 0) FixKeysUpward(path, cur);
  child_block = right;
  child_key = right_key;

  // Push the new child into the parent chain, splitting nodes as needed.
  while (true) {
    if (path.empty()) return NewRoot(root_, child_block);
    Frame frame = path.back();
    path.pop_back();
    uint8_t* parent = MutableBlock(frame.block);
    const uint32_t level = LevelOf(frame.block);
    const uint32_t pn = NumRecs(parent);
    const size_t insert_pos = frame.index + 1;
    if (pn < max_node()) {
      for (size_t i = pn; i > insert_pos; --i) {
        SetKeyAt(parent, i, KeyAt(parent, i - 1));
        SetPtrAt(parent, i, PtrAt(parent, i - 1));
      }
      SetKeyAt(parent, insert_pos, child_key);
      SetPtrAt(parent, insert_pos, child_block);
      SetNumRecs(parent, pn + 1);
      return OkStatus();
    }
    std::vector<Entry> entries;
    entries.reserve(pn + 1);
    for (size_t i = 0; i < insert_pos; ++i) {
      entries.push_back({KeyAt(parent, i), PtrAt(parent, i)});
    }
    entries.push_back({child_key, child_block});
    for (size_t i = insert_pos; i < pn; ++i) {
      entries.push_back({KeyAt(parent, i), PtrAt(parent, i)});
    }
    uint32_t right = kNullAgBlock;
    FreeRec right_key;
    Status s = SplitEntries(frame.block, level, entries, &right, &right_key);
    if (!s.ok()) return s;
    child_block = right;
    child_key = right_key;
  }
}

Status AllocTree::Remove(const FreeRec& rec) {
  std::vector<Frame> path;
  uint32_t cur = root_;
  uint32_t lvl = level_ - 1;  // root block level
  while (lvl > 0) {
    const uint8_t* b = Block(cur);
    if (b == nullptr) {
      return XFS_ERR(XfsError::kInternal, "free space btree block lost")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(cur);
    }
    const size_t idx = ChooseChild(b, rec);
    path.push_back({cur, idx});
    cur = PtrAt(b, idx);
    lvl--;
  }
  uint8_t* leaf = MutableBlock(cur);
  if (leaf == nullptr) {
    return XFS_ERR(XfsError::kInternal, "free space btree leaf lost")
        .Ag(agno_)
        .Metadata(name_)
        .Actual(cur);
  }
  const uint32_t n = NumRecs(leaf);
  size_t pos = n;
  for (size_t i = 0; i < n; ++i) {
    if (CompareRec(kind_, RecAt(leaf, i), rec) == 0) {
      pos = i;
      break;
    }
  }
  if (pos == n) {
    return XFS_ERR(XfsError::kCorruptBtree, "free space record not found")
        .Ag(agno_)
        .Metadata(name_)
        .Actual(rec.start);
  }
  for (size_t i = pos; i + 1 < n; ++i) SetRecAt(leaf, i, RecAt(leaf, i + 1));
  SetNumRecs(leaf, n - 1);
  if (pos == 0 && n - 1 > 0) FixKeysUpward(path, cur);

  // Rebalance: every non-root block keeps at least half of its maximum.
  while (cur != root_) {
    const uint32_t clvl = LevelOf(cur);
    const uint32_t n_cur = NumRecs(cur);
    const uint32_t minr = (clvl == 0 ? max_leaf() : max_node()) / 2;
    if (n_cur >= minr) break;
    Frame frame = path.back();
    uint8_t* parent = MutableBlock(frame.block);
    const size_t idx = frame.index;
    const uint32_t pn = NumRecs(parent);
    const uint32_t left = idx > 0 ? PtrAt(parent, idx - 1) : kNullAgBlock;
    const uint32_t right =
        idx + 1 < pn ? PtrAt(parent, idx + 1) : kNullAgBlock;
    const uint32_t maxr = (clvl == 0 ? max_leaf() : max_node());
    bool merged = false;

    if (left == kNullAgBlock && right == kNullAgBlock && path.size() == 1 &&
        pn == 1) {
      // The only child of the root: nothing to merge with. This is legal (the
      // root may keep a single child).
      break;
    }
    if (left != kNullAgBlock && NumRecs(Block(left)) + n_cur <= maxr) {
      std::vector<Entry> children = ReadEntries(cur);
      uint8_t* lb = MutableBlock(left);
      const uint32_t ln = NumRecs(lb);
      SetNumRecs(lb, ln + static_cast<uint32_t>(children.size()));
      for (size_t i = 0; i < children.size(); ++i) {
        if (clvl == 0) {
          SetRecAt(lb, ln + i, children[i].key);
        } else {
          SetKeyAt(lb, ln + i, children[i].key);
          SetPtrAt(lb, ln + i, children[i].ptr);
        }
      }
      const uint32_t rr = RightSib(Block(cur));
      SetRightSib(lb, rr);
      if (rr != kNullAgBlock) {
        uint8_t* rb = MutableBlock(rr);
        if (rb != nullptr) SetLeftSib(rb, left);
      }
      Status s = owner_->Ag(agno_)->PutTreeBlock(cur);
      if (!s.ok()) return s;
      blocks_.erase(cur);
      dirty_.erase(cur);
      // Remove the parent's entry for the erased block.
      const uint32_t pk = NumRecs(parent);
      for (size_t i = idx; i + 1 < pk; ++i) {
        SetKeyAt(parent, i, KeyAt(parent, i + 1));
        SetPtrAt(parent, i, PtrAt(parent, i + 1));
      }
      SetNumRecs(parent, pk - 1);
      merged = true;
    } else if (right != kNullAgBlock &&
               NumRecs(Block(right)) + n_cur <= maxr) {
      std::vector<Entry> children = ReadEntries(right);
      uint8_t* cb = MutableBlock(cur);
      const uint32_t cn = NumRecs(cb);
      SetNumRecs(cb, cn + static_cast<uint32_t>(children.size()));
      for (size_t i = 0; i < children.size(); ++i) {
        if (clvl == 0) {
          SetRecAt(cb, cn + i, children[i].key);
        } else {
          SetKeyAt(cb, cn + i, children[i].key);
          SetPtrAt(cb, cn + i, children[i].ptr);
        }
      }
      const uint32_t rr = RightSib(Block(right));
      SetRightSib(cb, rr);
      if (rr != kNullAgBlock) {
        uint8_t* rb = MutableBlock(rr);
        if (rb != nullptr) SetLeftSib(rb, cur);
      }
      Status s = owner_->Ag(agno_)->PutTreeBlock(right);
      if (!s.ok()) return s;
      blocks_.erase(right);
      dirty_.erase(right);
      const uint32_t pk = NumRecs(parent);
      for (size_t i = idx + 1; i + 1 < pk; ++i) {
        SetKeyAt(parent, i, KeyAt(parent, i + 1));
        SetPtrAt(parent, i, PtrAt(parent, i + 1));
      }
      SetNumRecs(parent, pk - 1);
      merged = true;
    } else if (left != kNullAgBlock && NumRecs(Block(left)) > minr) {
      // Borrow the last entry of the left sibling.
      uint8_t* lb = MutableBlock(left);
      uint8_t* cb = MutableBlock(cur);
      const uint32_t ln = NumRecs(lb);
      if (clvl == 0) {
        const FreeRec moved = RecAt(lb, ln - 1);
        SetNumRecs(lb, ln - 1);
        for (size_t i = NumRecs(cb); i > 0; --i) SetRecAt(cb, i, RecAt(cb, i - 1));
        SetRecAt(cb, 0, moved);
        SetNumRecs(cb, NumRecs(cb) + 1);
      } else {
        const FreeRec moved_key = KeyAt(lb, ln - 1);
        const uint32_t moved_ptr = PtrAt(lb, ln - 1);
        SetNumRecs(lb, ln - 1);
        for (size_t i = NumRecs(cb); i > 0; --i) {
          SetKeyAt(cb, i, KeyAt(cb, i - 1));
          SetPtrAt(cb, i, PtrAt(cb, i - 1));
        }
        SetKeyAt(cb, 0, moved_key);
        SetPtrAt(cb, 0, moved_ptr);
        SetNumRecs(cb, NumRecs(cb) + 1);
      }
      SetKeyAt(parent, idx, KeyAt(cb, 0));
      if (idx == 0) {
        std::vector<Frame> ppath(path.begin(), path.end() - 1);
        FixKeysUpward(ppath, frame.block);
      }
      break;
    } else if (right != kNullAgBlock && NumRecs(Block(right)) > minr) {
      uint8_t* rb = MutableBlock(right);
      uint8_t* cb = MutableBlock(cur);
      const uint32_t cn = NumRecs(cb);
      if (clvl == 0) {
        const FreeRec moved = RecAt(rb, 0);
        for (size_t i = 0; i + 1 < NumRecs(rb); ++i) {
          SetRecAt(rb, i, RecAt(rb, i + 1));
        }
        SetNumRecs(rb, NumRecs(rb) - 1);
        SetRecAt(cb, cn, moved);
        SetNumRecs(cb, cn + 1);
      } else {
        const FreeRec moved_key = KeyAt(rb, 0);
        const uint32_t moved_ptr = PtrAt(rb, 0);
        for (size_t i = 0; i + 1 < NumRecs(rb); ++i) {
          SetKeyAt(rb, i, KeyAt(rb, i + 1));
          SetPtrAt(rb, i, PtrAt(rb, i + 1));
        }
        SetNumRecs(rb, NumRecs(rb) - 1);
        SetKeyAt(cb, cn, moved_key);
        SetPtrAt(cb, cn, moved_ptr);
        SetNumRecs(cb, cn + 1);
      }
      SetKeyAt(parent, idx + 1, KeyAt(rb, 0));
      break;
    } else {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "cannot rebalance a free space btree block")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(cur);
    }

    if (merged) {
      path.pop_back();
      cur = frame.block;
    }
  }

  // A root with a single leaf child collapses into the root block.
  if (level_ > 1 && NumRecs(root_) == 1) {
    uint8_t* root_block = MutableBlock(root_);
    const uint32_t child = PtrAt(root_block, 0);
    if (LevelOf(child) != 0) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "cannot collapse a multi-level free space btree")
          .Ag(agno_)
          .Metadata(name_);
    }
    const std::vector<Entry> children = ReadEntries(child);
    PutBE32(root_block, bt::kMagic,
            kind_ == TreeKind::kBno ? ondisk::kBtbCrcMagic
                                    : ondisk::kBtcCrcMagic);
    PutBE16(root_block, bt::kLevel, 0);
    PutBE32(root_block, bt::kSLeftsib, kNullAgBlock);
    PutBE32(root_block, bt::kSRightSib, kNullAgBlock);
    SetNumRecs(root_block, static_cast<uint32_t>(children.size()));
    for (size_t i = 0; i < children.size(); ++i) {
      SetRecAt(root_block, i, children[i].key);
    }
    Status s = owner_->Ag(agno_)->PutTreeBlock(child);
    if (!s.ok()) return s;
    blocks_.erase(child);
    dirty_.erase(child);
    level_ = 1;
  }
  return OkStatus();
}

// ---------------------------------------------------------------------------
// AgState
// ---------------------------------------------------------------------------

uint32_t AgState::Contribution() const {
  uint32_t btreeblks = static_cast<uint32_t>(bno->block_count() - 1) +
                       static_cast<uint32_t>(cnt->block_count() - 1);
  if (owner->rmap_enabled()) {
    const uint32_t blocks = owner->RmapBlocks(agno);
    if (blocks > 0) btreeblks += blocks - 1;
  }
  return freeblks + flcount + btreeblks;
}

Status AgState::Load(Allocator* allocator, uint32_t agno_in) {
  owner = allocator;
  agno = agno_in;
  const XfsGeometry* geo = owner->geo();
  length = geo->AgBlocksFor(agno);
  agfl_size = AgflSize(*geo);

  const uint64_t ag_byte = geo->AgByteOffset(agno);
  agf_buf.assign(geo->sector_size, 0);
  if (!owner->device()->Read(ag_byte + AgfByteOffset(*geo), agf_buf.data(),
                             agf_buf.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read AGF")
        .Ag(agno)
        .Metadata("AGF");
  }
  const uint8_t* af = agf_buf.data();
  if (GetBE32(af, agf::kMagic) != ondisk::kAgfMagic) {
    return XFS_ERR(XfsError::kBadMagic, "AGF magic mismatch")
        .Ag(agno)
        .Metadata("AGF");
  }
  if (GetBE32(af, agf::kVersionnum) != agf::kVersion) {
    return XFS_ERR(XfsError::kBadVersion, "AGF version mismatch")
        .Ag(agno)
        .Metadata("AGF");
  }
  if (GetBE32(af, agf::kSeqno) != agno) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF seqno mismatch")
        .Ag(agno)
        .Metadata("AGF");
  }
  if (GetBE32(af, agf::kLength) != length) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF length mismatch")
        .Ag(agno)
        .Metadata("AGF");
  }
  const uint32_t bno_root = GetBE32(af, agf::kBnoRoot);
  const uint32_t cnt_root = GetBE32(af, agf::kCntRoot);
  const uint32_t bno_level = GetBE32(af, agf::kBnoLevel);
  const uint32_t cnt_level = GetBE32(af, agf::kCntLevel);
  if (!geo->AgbnoValid(agno, bno_root) || !geo->AgbnoValid(agno, cnt_root)) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF btree root out of range")
        .Ag(agno)
        .Metadata("AGF");
  }
  flfirst = GetBE32(af, agf::kFlfirst);
  fllast = GetBE32(af, agf::kFllast);
  flcount = GetBE32(af, agf::kFlcount);
  if (flcount > agfl_size || flfirst >= agfl_size || fllast >= agfl_size) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF freelist out of range")
        .Ag(agno)
        .Metadata("AGF");
  }
  freeblks = GetBE32(af, agf::kFreeblks);
  if (freeblks > length) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF freeblks out of range")
        .Ag(agno)
        .Metadata("AGF");
  }
  orig_freeblks = freeblks;
  orig_flcount = flcount;
  orig_btreeblks = GetBE32(af, agf::kBtreeblks);
  rmap_root = GetBE32(af, agf::kRmapRoot);
  rmap_level = GetBE32(af, agf::kRmapLevel);
  rmap_blocks = GetBE32(af, agf::kRmapBlocks);
  if (geo->features.rmapbt) {
    if (rmap_root == kNullAgBlock || !geo->AgbnoValid(agno, rmap_root) ||
        rmap_level < 1 || rmap_level > bt::kMaxTraversalLevels ||
        rmap_blocks == 0 || rmap_blocks > length) {
      return XFS_ERR(XfsError::kCorruptAgHeader,
                     "AGF rmap root/level/blocks is invalid")
          .Ag(agno)
          .Actual(rmap_root)
          .Metadata("AGF");
    }
  }

  bno = std::make_unique<AllocTree>(owner, agno, TreeKind::kBno, "bnobt");
  cnt = std::make_unique<AllocTree>(owner, agno, TreeKind::kCnt, "cntbt");
  Status s = bno->Load(bno_root, bno_level);
  if (!s.ok()) return s;
  s = cnt->Load(cnt_root, cnt_level);
  if (!s.ok()) return s;

  recs.clear();
  s = bno->CollectRecords(&recs);
  if (!s.ok()) return s;
  std::vector<FreeRec> cnt_recs;
  s = cnt->CollectRecords(&cnt_recs);
  if (!s.ok()) return s;
  if (cnt_recs.size() != recs.size()) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "bnobt and cntbt record counts differ")
        .Ag(agno)
        .Metadata("free space");
  }
  std::sort(cnt_recs.begin(), cnt_recs.end(),
            [](const FreeRec& a, const FreeRec& b) {
              if (a.start != b.start) return a.start < b.start;
              return a.len < b.len;
            });
  for (size_t i = 0; i < recs.size(); ++i) {
    if (recs[i].start != cnt_recs[i].start || recs[i].len != cnt_recs[i].len) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "bnobt and cntbt records differ")
          .Ag(agno)
          .Metadata("free space")
          .Actual(recs[i].start);
    }
  }
  {
    uint64_t sum = 0;
    for (const FreeRec& r : recs) sum += r.len;
    if (sum != freeblks) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "free space records do not sum to agf_freeblks")
          .Ag(agno)
          .Metadata("AGF")
          .Expected(freeblks)
          .Actual(sum);
    }
  }
  RecomputeLongest();

  agfl_buf.assign(geo->sector_size, 0);
  if (!owner->device()->Read(ag_byte + AgflByteOffset(*geo), agfl_buf.data(),
                             agfl_buf.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read AGFL")
        .Ag(agno)
        .Metadata("AGFL");
  }
  if (GetBE32(agfl_buf.data(), agfl::kMagic) != ondisk::kAgflMagic) {
    return XFS_ERR(XfsError::kBadMagic, "AGFL magic mismatch")
        .Ag(agno)
        .Metadata("AGFL");
  }
  if (GetBE32(agfl_buf.data(), agfl::kSeqno) != agno) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGFL seqno mismatch")
        .Ag(agno)
        .Metadata("AGFL");
  }
  return OkStatus();
}

void AgState::RecomputeLongest() {
  longest = 0;
  for (const FreeRec& r : recs) longest = std::max(longest, r.len);
}

Status AgState::InsertRec(const FreeRec& rec) {
  if (rec.len == 0) {
    return XFS_ERR(XfsError::kInternal, "empty free extent")
        .Ag(agno)
        .Metadata("free space");
  }
  auto it = std::lower_bound(recs.begin(), recs.end(), rec,
                             [](const FreeRec& a, const FreeRec& b) {
                               return a.start < b.start;
                             });
  if (it != recs.begin()) {
    const FreeRec& prev = *(it - 1);
    if (prev.start + prev.len > rec.start) {
      return XFS_ERR(XfsError::kCorruptBtree, "free extent overlaps previous")
          .Ag(agno)
          .Metadata("free space")
          .Actual(rec.start);
    }
  }
  if (it != recs.end() && rec.start + rec.len > it->start) {
    return XFS_ERR(XfsError::kCorruptBtree, "free extent overlaps next")
        .Ag(agno)
        .Metadata("free space")
        .Actual(rec.start);
  }
  Status s = bno->Insert(rec);
  if (!s.ok()) return s;
  s = cnt->Insert(rec);
  if (!s.ok()) return s;
  recs.insert(it, rec);
  agf_dirty = true;
  return OkStatus();
}

Status AgState::RemoveRec(const FreeRec& rec) {
  auto it = std::find_if(recs.begin(), recs.end(), [&](const FreeRec& r) {
    return r.start == rec.start && r.len == rec.len;
  });
  if (it == recs.end()) {
    return XFS_ERR(XfsError::kInternal, "free extent not in the record list")
        .Ag(agno)
        .Metadata("free space")
        .Actual(rec.start);
  }
  Status s = bno->Remove(rec);
  if (!s.ok()) return s;
  s = cnt->Remove(rec);
  if (!s.ok()) return s;
  recs.erase(it);
  agf_dirty = true;
  return OkStatus();
}

Status AgState::GetTreeBlock(uint32_t* agbno) {
  if (flcount == 0) {
    if (owner->rmap_editing()) {
      // The reservation failed earlier; never recurse into rmap mapping while
      // the rmap tree is already being edited.
      return XFS_ERR(XfsError::kNoSpace,
                     "AGFL reservation exhausted during an rmap edit")
          .Ag(agno)
          .Metadata("AGFL");
    }
    Status s = owner->EnsureAgflInternal(agno, 1);
    if (!s.ok()) return s;
  }
  if (flcount == 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "AGFL is empty and cannot be refilled")
        .Ag(agno)
        .Metadata("AGFL");
  }
  const uint32_t agb = GetBE32(agfl_buf.data(), kAgflHeaderSize + flfirst * 4);
  if (agb >= length) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGFL entry out of range")
        .Ag(agno)
        .Metadata("AGFL")
        .Actual(agb);
  }
  flfirst = (flfirst + 1) % agfl_size;
  flcount--;
  agfl_dirty = true;
  agf_dirty = true;
  *agbno = agb;
  return OkStatus();
}

Status AgState::PutTreeBlock(uint32_t agbno) {
  if (flcount >= agfl_size) {
    if (owner->rmap_editing()) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "AGFL is full during an rmap edit")
          .Ag(agno)
          .Metadata("AGFL");
    }
    Status s = DrainAgfl(1);
    if (!s.ok()) return s;
    if (flcount >= agfl_size) {
      return XFS_ERR(XfsError::kUnsupportedFeature, "AGFL is full")
          .Ag(agno)
          .Metadata("AGFL");
    }
  }
  if (agbno >= length) {
    return XFS_ERR(XfsError::kInternal, "freeing an out of range AGFL block")
        .Ag(agno)
        .Metadata("AGFL")
        .Actual(agbno);
  }
  fllast = (fllast + 1) % agfl_size;
  PutBE32(agfl_buf.data(), kAgflHeaderSize + fllast * 4, agbno);
  flcount++;
  agfl_dirty = true;
  agf_dirty = true;
  return OkStatus();
}

Status AgState::RefillAgflRaw(uint32_t want, std::vector<FreeRec>* added) {
  if (refilling) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "AGFL refill would recurse into itself")
        .Ag(agno)
        .Metadata("AGFL");
  }
  if (want > agfl_size) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "AGFL refill target exceeds the ring size")
        .Ag(agno)
        .Metadata("AGFL");
  }
  refilling = true;
  Status result = OkStatus();
  while (flcount < want) {
    const FreeRec* best = nullptr;
    for (const FreeRec& r : recs) {
      if (r.len >= 1 && (best == nullptr || r.len > best->len)) best = &r;
    }
    if (best == nullptr) {
      result = XFS_ERR(XfsError::kNoSpace,
                       "no free space available to refill the AGFL")
                   .Ag(agno)
                   .Metadata("AGFL");
      break;
    }
    const FreeRec old = *best;
    const uint32_t take = std::min<uint32_t>(want - flcount, old.len);
    if (take > agfl_size - flcount) {
      result = XFS_ERR(XfsError::kUnsupportedFeature,
                       "AGFL refill would overflow the ring")
                   .Ag(agno)
                   .Metadata("AGFL");
      break;
    }
    result = RemoveRec(old);
    if (!result.ok()) break;
    if (old.len > take) {
      result = InsertRec(FreeRec{old.start + take, old.len - take});
      if (!result.ok()) break;
    }
    freeblks -= take;
    for (uint32_t i = 0; i < take; ++i) {
      result = PutTreeBlock(old.start + i);
      if (!result.ok()) break;
    }
    if (!result.ok()) break;
    if (added != nullptr) added->push_back(FreeRec{old.start, take});
  }
  refilling = false;
  // Refill removes blocks from the free trees even when no data allocation
  // follows (e.g. RUI completion). Keep the AGF maximum in sync with recs;
  // callers must not have to perform a later Allocate/Free to refresh it.
  RecomputeLongest();
  return result;
}

Status AgState::DrainAgfl(uint32_t count) {
  while (count > 0 && flcount > 0) {
    const uint32_t agb =
        GetBE32(agfl_buf.data(), kAgflHeaderSize + flfirst * 4);
    if (agb >= length) {
      return XFS_ERR(XfsError::kCorruptAgHeader, "AGFL entry out of range")
          .Ag(agno)
          .Actual(agb)
          .Metadata("AGFL");
    }
    if (owner->rmap_enabled()) {
      Status s = owner->UnmapMetadata(agno, agb, 1, kRmapOwnerAg);
      if (!s.ok()) return s;
    }
    flfirst = (flfirst + 1) % agfl_size;
    flcount--;
    agfl_dirty = true;
    agf_dirty = true;

    // Return the block to ordinary free space, merging both neighbours.
    FreeRec left;
    bool have_left = false;
    FreeRec right;
    bool have_right = false;
    for (const FreeRec& r : recs) {
      if (r.start + r.len == agb) {
        left = r;
        have_left = true;
      }
      if (r.start == agb + 1) {
        right = r;
        have_right = true;
      }
    }
    Status s = OkStatus();
    if (have_left && have_right) {
      s = RemoveRec(left);
      if (s.ok()) s = RemoveRec(right);
      if (s.ok()) {
        s = InsertRec(FreeRec{left.start, left.len + 1 + right.len});
      }
    } else if (have_left) {
      s = RemoveRec(left);
      if (s.ok()) s = InsertRec(FreeRec{left.start, left.len + 1});
    } else if (have_right) {
      s = RemoveRec(right);
      if (s.ok()) s = InsertRec(FreeRec{agb, 1 + right.len});
    } else {
      s = InsertRec(FreeRec{agb, 1});
    }
    if (!s.ok()) return s;
    ++freeblks;
    --count;
  }
  RecomputeLongest();
  return OkStatus();
}

Status AgState::AllocateBestFit(uint32_t want, const RmapOwner* rmap_owner,
                                Allocation* out) {
  if (this->owner->rmap_enabled() && rmap_owner == nullptr) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmapbt allocation requires an owner")
        .Ag(agno)
        .Metadata("allocator");
  }
  const FreeRec* best = nullptr;
  for (const FreeRec& r : recs) {
    if (r.len < want) continue;
    if (best == nullptr || r.len < best->len ||
        (r.len == best->len && r.start < best->start)) {
      best = &r;
    }
  }
  if (best == nullptr) {
    return XFS_ERR(XfsError::kNoSpace, "AG has no free extent that large")
        .Ag(agno)
        .Metadata("free space")
        .Actual(want);
  }
  const FreeRec old = *best;
  Status s = RemoveRec(old);
  if (!s.ok()) return s;
  if (old.len > want) {
    s = InsertRec(FreeRec{old.start + want, old.len - want});
    if (!s.ok()) return s;
  }
  freeblks -= want;
  RecomputeLongest();
  agf_dirty = true;
  if (rmap_owner != nullptr) {
    Status s = this->owner->MapOwned(agno, old.start, want, *rmap_owner);
    if (!s.ok()) return s;
  }
  out->fsb = this->owner->geo()->AgFsb(agno, old.start);
  out->len = want;
  return OkStatus();
}

Status AgState::AllocateAligned(uint32_t want, uint32_t align,
                                const RmapOwner* rmap_owner, Allocation* out,
                                uint32_t min_agbno, uint32_t max_agbno) {
  if (this->owner->rmap_enabled() && rmap_owner == nullptr) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmapbt allocation requires an owner")
        .Ag(agno)
        .Metadata("allocator");
  }
  if (align <= 1 && min_agbno == 0 && max_agbno >= length) {
    return AllocateBestFit(want, rmap_owner, out);
  }
  if (align <= 1) align = 1;
  // Best fit among the free extents that contain an aligned run of `want`
  // blocks inside [min_agbno, max_agbno]. The candidate with the smallest
  // extent wins, then the one whose aligned start is closest to the extent
  // start, then the lowest start.
  const FreeRec* best = nullptr;
  uint32_t best_off = 0;
  uint32_t best_slack = 0;
  for (const FreeRec& r : recs) {
    if (r.len < want) continue;
    const uint64_t range_end = std::min<uint64_t>(
        static_cast<uint64_t>(r.start) + r.len, max_agbno);
    if (range_end < r.start + want) continue;
    uint32_t off = (r.start + align - 1) & ~(align - 1);
    if (off < r.start) continue;  // overflow cannot happen inside an AG
    if (off < min_agbno) {
      off = (min_agbno + align - 1) & ~(align - 1);
    }
    if (off < r.start || off > range_end ||
        want > range_end - off) {
      continue;
    }
    const uint32_t slack = off - r.start;
    if (best == nullptr || r.len < best->len ||
        (r.len == best->len &&
         (slack < best_slack || (slack == best_slack && r.start < best->start)))) {
      best = &r;
      best_off = off;
      best_slack = slack;
    }
  }
  if (best == nullptr) {
    return XFS_ERR(XfsError::kNoSpace,
                   "AG has no free extent with an aligned run that large")
        .Ag(agno)
        .Metadata("free space")
        .Actual(want);
  }
  const FreeRec old = *best;
  Status s = RemoveRec(old);
  if (!s.ok()) return s;
  if (best_off > old.start) {
    s = InsertRec(FreeRec{old.start, best_off - old.start});
    if (!s.ok()) return s;
  }
  if (old.start + old.len > best_off + want) {
    s = InsertRec(FreeRec{best_off + want, old.start + old.len - (best_off + want)});
    if (!s.ok()) return s;
  }
  freeblks -= want;
  RecomputeLongest();
  agf_dirty = true;
  if (rmap_owner != nullptr) {
    Status mapped = this->owner->MapOwned(agno, best_off, want, *rmap_owner);
    if (!mapped.ok()) return mapped;
  }
  out->fsb = this->owner->geo()->AgFsb(agno, best_off);
  out->len = want;
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Allocator
// ---------------------------------------------------------------------------

Status Allocator::LoadAg(uint32_t agno, AgState** out) {
  auto it = ags_.find(agno);
  if (it != ags_.end()) {
    *out = it->second.get();
    return OkStatus();
  }
  auto state = std::make_unique<AgState>();
  Status s = state->Load(this, agno);
  if (!s.ok()) return s;
  AgState* raw = state.get();
  ags_.emplace(agno, std::move(state));
  *out = raw;
  if (rmap_enabled() && !raw->rmap_loaded) {
    s = rmap_->Load(agno, raw->rmap_root, raw->rmap_level, raw->rmap_blocks);
    if (!s.ok()) {
      ags_.erase(agno);
      return s;
    }
    raw->rmap_loaded = true;
  }
  return OkStatus();
}

uint32_t Allocator::RmapBlocks(uint32_t agno) const {
  if (!rmap_) return 0;
  return rmap_->Blocks(agno);
}

Status Allocator::MapOwned(uint32_t agno, uint32_t agbno, uint32_t len,
                           const RmapOwner& owner) {
  if (!rmap_enabled()) return OkStatus();
  if (rmap_editing_) {
    return XFS_ERR(XfsError::kInternal,
                   "recursive rmap ownership update")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  rmap_editing_ = true;
  Status s = rmap_->AllocateExtent(agno, agbno, len, owner);
  rmap_editing_ = false;
  if (s.ok()) {
    auto it = ags_.find(agno);
    if (it != ags_.end()) it->second->agf_dirty = true;
  }
  return s;
}

Status Allocator::UnmapOwned(uint32_t agno, uint32_t agbno, uint32_t len,
                             const RmapOwner& owner, bool unknown_owner) {
  if (!rmap_enabled()) return OkStatus();
  if (rmap_editing_) {
    return XFS_ERR(XfsError::kInternal,
                   "recursive rmap ownership update")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  rmap_editing_ = true;
  Status s = rmap_->FreeExtent(agno, agbno, len, owner, unknown_owner);
  rmap_editing_ = false;
  if (s.ok()) {
    auto it = ags_.find(agno);
    if (it != ags_.end()) it->second->agf_dirty = true;
  }
  return s;
}

Status Allocator::ConvertOwnedRmap(uint32_t agno, uint32_t agbno,
                                   uint32_t len, const RmapOwner& owner,
                                   bool old_unwritten) {
  if (!rmap_enabled()) return OkStatus();
  if (rmap_editing_) {
    return XFS_ERR(XfsError::kInternal,
                   "recursive rmap ownership update")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  rmap_editing_ = true;
  Status s = rmap_->Convert(agno, agbno, len, owner, old_unwritten);
  rmap_editing_ = false;
  if (s.ok()) {
    auto it = ags_.find(agno);
    if (it != ags_.end()) it->second->agf_dirty = true;
  }
  return s;
}

Status Allocator::MapMetadata(uint32_t agno, uint32_t agbno, uint32_t len,
                              uint64_t owner) {
  if (!rmap_enabled()) return OkStatus();
  if (rmap_editing_) {
    return XFS_ERR(XfsError::kInternal,
                   "recursive metadata ownership update")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  rmap_editing_ = true;
  Status s = rmap_->AllocateMetadata(agno, agbno, len, owner);
  rmap_editing_ = false;
  if (s.ok()) {
    auto it = ags_.find(agno);
    if (it != ags_.end()) it->second->agf_dirty = true;
  }
  return s;
}

Status Allocator::UnmapMetadata(uint32_t agno, uint32_t agbno, uint32_t len,
                                uint64_t owner) {
  if (!rmap_enabled()) return OkStatus();
  if (rmap_editing_) {
    return XFS_ERR(XfsError::kInternal,
                   "recursive metadata ownership update")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  rmap_editing_ = true;
  Status s = rmap_->FreeMetadata(agno, agbno, len, owner);
  rmap_editing_ = false;
  if (s.ok()) {
    auto it = ags_.find(agno);
    if (it != ags_.end()) it->second->agf_dirty = true;
  }
  return s;
}

Status Allocator::AllocateRmapBlock(uint32_t agno, uint32_t* agbno) {
  auto it = ags_.find(agno);
  if (it == ags_.end()) {
    return XFS_ERR(XfsError::kInternal,
                   "rmap tree block allocation before the AG is loaded")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  return it->second->GetTreeBlock(agbno);
}

Status Allocator::FreeRmapBlock(uint32_t agno, uint32_t agbno) {
  auto it = ags_.find(agno);
  if (it == ags_.end()) {
    return XFS_ERR(XfsError::kInternal,
                   "rmap tree block free before the AG is loaded")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  return it->second->PutTreeBlock(agbno);
}

Status Allocator::EnsureAgflInternal(uint32_t agno, uint32_t want) {
  AgState* ag = nullptr;
  Status s = LoadAg(agno, &ag);
  if (!s.ok()) return s;
  if (!rmap_enabled()) {
    return ag->RefillAgflRaw(want, nullptr);
  }
  // Reserve for the current rmap height plus the caller's request.  Mapping
  // the newly-added ranges can itself split the rmap tree; those splits take
  // blocks from the AGFL, so the refill target must already include the
  // worst-case reserve.  The loop handles blocks consumed by that split.
  for (uint32_t attempt = 0; attempt < 8; ++attempt) {
    const uint32_t reserve =
        std::max<uint32_t>(rmap_->ReserveBlocks(agno) + 2, want);
    if (ag->flcount >= reserve) return OkStatus();
    std::vector<FreeRec> added;
    s = ag->RefillAgflRaw(reserve, &added);
    if (!s.ok()) return s;
    for (const FreeRec& rec : added) {
      s = MapMetadata(agno, rec.start, rec.len, kRmapOwnerAg);
      if (!s.ok()) return s;
    }
    if (ag->flcount >= reserve) return OkStatus();
  }
  return XFS_ERR(XfsError::kNoSpace,
                 "could not establish the rmap AGFL reservation")
      .Ag(agno)
      .Metadata("AGFL");
}

Status Allocator::ReserveForRmap(uint32_t agno) {
  if (!rmap_enabled()) return OkStatus();
  return EnsureAgflInternal(agno, rmap_->ReserveBlocks(agno) + 2);
}

Status Allocator::FreeBlocksInternal(uint64_t fsb, uint32_t len,
                                     const RmapOwner* owner,
                                     bool unknown_owner) {
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot free zero blocks")
        .Metadata("allocator");
  }
  if (!geo_->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "free range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("allocator");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo_->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo_->agblklog) - 1));
  AgState* ag = nullptr;
  Status s = LoadAg(agno, &ag);
  if (!s.ok()) return s;
  if (rmap_enabled()) {
    s = ReserveForRmap(agno);
    if (!s.ok()) return s;
    if (unknown_owner) {
      s = UnmapOwned(agno, agbno, len, RmapOwner{}, true);
    } else if (owner != nullptr) {
      s = UnmapOwned(agno, agbno, len, *owner, false);
    } else {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "rmapbt free requires an owner")
          .Ag(agno)
          .Actual(agbno)
          .Metadata("allocator");
    }
    if (!s.ok()) return s;
  }

  for (const FreeRec& r : ag->recs) {
    if (agbno < r.start + r.len && r.start < agbno + len) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "freeing blocks that are already free")
          .Ag(agno)
          .Actual(agbno)
          .Metadata("free space");
    }
  }
  FreeRec left;
  bool have_left = false;
  FreeRec right;
  bool have_right = false;
  for (const FreeRec& r : ag->recs) {
    if (r.start + r.len == agbno) {
      left = r;
      have_left = true;
    }
    if (r.start == agbno + len) {
      right = r;
      have_right = true;
    }
  }
  if (have_left && have_right) {
    s = ag->RemoveRec(left);
    if (!s.ok()) return s;
    s = ag->RemoveRec(right);
    if (!s.ok()) return s;
    s = ag->InsertRec(FreeRec{left.start, left.len + len + right.len});
  } else if (have_left) {
    s = ag->RemoveRec(left);
    if (!s.ok()) return s;
    s = ag->InsertRec(FreeRec{left.start, left.len + len});
  } else if (have_right) {
    s = ag->RemoveRec(right);
    if (!s.ok()) return s;
    s = ag->InsertRec(FreeRec{agbno, len + right.len});
  } else {
    s = ag->InsertRec(FreeRec{agbno, len});
  }
  if (!s.ok()) return s;
  ag->freeblks += len;
  ag->RecomputeLongest();
  ag->agf_dirty = true;
  return OkStatus();
}

Status Allocator::AddBuffer(uint64_t offset, std::vector<uint8_t> data,
                            uint16_t type, size_t lsn_offset,
                            size_t crc_offset, const char* what) {
  OutBuffer buffer;
  buffer.offset = offset;
  buffer.data = std::move(data);
  buffer.type = type;
  buffer.lsn_offset = lsn_offset;
  buffer.crc_offset = crc_offset;
  buffer.what = what;
  auto it = buffers_.find(offset);
  if (it != buffers_.end()) {
    it->second = std::move(buffer);
  } else {
    buffers_.emplace(offset, std::move(buffer));
  }
  return OkStatus();
}

Status Allocator::Commit() {
  if (rmap_) {
    // Publish rmap tree blocks before the AGF that points at their new root.
    // Both are part of the same buffer-logging transaction; publication order
    // only affects the record layout, not durability.
    Status s = rmap_->Commit();
    if (!s.ok()) return s;
  }
  for (auto& entry : ags_) {
    AgState& ag = *entry.second;

    if (!ag.counted) {
      const uint32_t old_contribution =
          ag.orig_freeblks + ag.orig_flcount + ag.orig_btreeblks;
      const uint32_t new_contribution = ag.Contribution();
      fdblocks_delta_ += static_cast<int64_t>(new_contribution) -
                         static_cast<int64_t>(old_contribution);
      ag.counted = true;
    }

    if (ag.agf_dirty || ag.agfl_dirty) {
      uint8_t* af = ag.agf_buf.data();
      PutBE32(af, agf::kFreeblks, ag.freeblks);
      PutBE32(af, agf::kLongest, ag.longest);
      PutBE32(af, agf::kFlfirst, ag.flfirst);
      PutBE32(af, agf::kFllast, ag.fllast);
      PutBE32(af, agf::kFlcount, ag.flcount);
      uint32_t btreeblks = static_cast<uint32_t>(ag.bno->block_count() - 1) +
                           static_cast<uint32_t>(ag.cnt->block_count() - 1);
      if (rmap_enabled()) {
        const uint32_t blocks = rmap_->Blocks(ag.agno);
        if (blocks > 0) btreeblks += blocks - 1;
        PutBE32(af, agf::kRmapRoot, rmap_->Root(ag.agno));
        PutBE32(af, agf::kRmapLevel, rmap_->Level(ag.agno));
        PutBE32(af, agf::kRmapBlocks, blocks);
      }
      PutBE32(af, agf::kBtreeblks, btreeblks);
      PutBE32(af, agf::kBnoRoot, ag.bno->root());
      PutBE32(af, agf::kCntRoot, ag.cnt->root());
      PutBE32(af, agf::kBnoLevel, ag.bno->level());
      PutBE32(af, agf::kCntLevel, ag.cnt->level());
      const uint64_t offset =
          geo_->AgByteOffset(ag.agno) + AgfByteOffset(*geo_);
      Status s = AddBuffer(offset, ag.agf_buf, lf::kBlftAgf, agf::kLsn,
                           agf::kCrc, "AGF");
      if (!s.ok()) return s;
    }
    if (ag.agfl_dirty) {
      const uint64_t offset =
          geo_->AgByteOffset(ag.agno) + AgflByteOffset(*geo_);
      Status s = AddBuffer(offset, ag.agfl_buf, lf::kBlftAgfl, agfl::kLsn,
                           agfl::kCrc, "AGFL");
      if (!s.ok()) return s;
    }
    for (AllocTree* tree : {ag.bno.get(), ag.cnt.get()}) {
      for (uint32_t agbno : tree->dirty_blocks()) {
        const auto& blocks = tree->blocks();
        auto block_it = blocks.find(agbno);
        if (block_it == blocks.end()) continue;
        const uint64_t offset = FsbToByte(geo_->AgFsb(ag.agno, agbno));
        Status s = AddBuffer(offset, block_it->second, lf::kBlftBtree,
                             bt::kSLsn, bt::kSCrc, "free space btree");
        if (!s.ok()) return s;
      }
    }
  }

  if (fdblocks_delta_ != 0) {
    std::vector<uint8_t> sb_buf;
    const std::vector<uint8_t>* queued = txn_->QueuedBuffer(0);
    if (queued != nullptr) {
      sb_buf = *queued;
    } else {
      sb_buf.assign(geo_->sector_size, 0);
      if (!device_->Read(0, sb_buf.data(), sb_buf.size())) {
        return XFS_ERR(XfsError::kIoError, "failed to read the superblock")
            .Offset(0)
            .Metadata("superblock");
      }
    }
    const uint64_t current = GetBE64(sb_buf.data(), sb::kFdblocks);
    const int64_t updated = static_cast<int64_t>(current) + fdblocks_delta_;
    if (updated < 0 || static_cast<uint64_t>(updated) > geo_->dblocks) {
      return XFS_ERR(XfsError::kInternal,
                     "superblock free block counter would leave its range")
          .Metadata("superblock")
          .Expected(geo_->dblocks)
          .Actual(static_cast<uint64_t>(updated));
    }
    PutBE64(sb_buf.data(), sb::kFdblocks, static_cast<uint64_t>(updated));
    Status s = AddBuffer(0, std::move(sb_buf), lf::kBlftSb, sb::kLsn, sb::kCrc,
                         "superblock");
    if (!s.ok()) return s;
  }

  for (auto& entry : buffers_) {
    OutBuffer& buffer = entry.second;
    Status s = txn_->AddMetadataBuffer(
        buffer.offset, std::move(buffer.data), buffer.type, buffer.lsn_offset,
        buffer.crc_offset, buffer.what);
    if (!s.ok()) return s;
  }
  if (!txn_->record_fits()) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "allocation metadata does not fit in one log record")
        .Metadata("transaction");
  }
  return OkStatus();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

class BlockAllocator::Impl {
 public:
  std::unique_ptr<Allocator> alloc;
};

BlockAllocator::BlockAllocator(XfsTransaction* txn, BlockDevice* device,
                               const XfsGeometry* geo)
    : impl_(std::make_unique<Impl>()) {
  impl_->alloc = std::make_unique<Allocator>(txn, device, geo);
}

BlockAllocator::~BlockAllocator() = default;

Status BlockAllocator::Allocate(uint32_t preferred_agno, uint32_t len,
                                Allocation* out) {
  if (impl_->alloc->rmap_enabled()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmapbt allocation requires an owner")
        .Metadata("allocator");
  }
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot allocate zero blocks")
        .Metadata("allocator");
  }
  if (preferred_agno >= impl_->alloc->geo()->agcount) preferred_agno = 0;
  const uint32_t agcount = impl_->alloc->geo()->agcount;
  for (uint32_t i = 0; i < agcount; ++i) {
    const uint32_t agno = (preferred_agno + i) % agcount;
    AgState* ag = nullptr;
    Status s = impl_->alloc->LoadAg(agno, &ag);
    if (!s.ok()) return s;
    s = ag->AllocateBestFit(len, nullptr, out);
    if (s.ok()) return s;
    if (s.error().code != XfsError::kNoSpace) return s;
  }
  return XFS_ERR(XfsError::kNoSpace, "no AG has a free extent that large")
      .Metadata("allocator")
      .Actual(len);
}

Status BlockAllocator::AllocateOwned(uint32_t preferred_agno, uint32_t len,
                                     const RmapOwner& owner, Allocation* out) {
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot allocate zero blocks")
        .Metadata("allocator");
  }
  if (preferred_agno >= impl_->alloc->geo()->agcount) preferred_agno = 0;
  const uint32_t agcount = impl_->alloc->geo()->agcount;
  for (uint32_t i = 0; i < agcount; ++i) {
    const uint32_t agno = (preferred_agno + i) % agcount;
    Status s = impl_->alloc->ReserveForRmap(agno);
    if (!s.ok()) {
      if (s.error().code == XfsError::kNoSpace) continue;
      return s;
    }
    AgState* ag = nullptr;
    s = impl_->alloc->LoadAg(agno, &ag);
    if (!s.ok()) return s;
    s = ag->AllocateBestFit(len, &owner, out);
    if (s.ok()) return s;
    if (s.error().code != XfsError::kNoSpace) return s;
  }
  return XFS_ERR(XfsError::kNoSpace, "no AG has a free extent that large")
      .Metadata("allocator")
      .Actual(len);
}

Status BlockAllocator::AllocateFromAg(uint32_t agno, uint32_t len,
                                      uint32_t align, Allocation* out) {
  const XfsGeometry* geo = impl_->alloc->geo();
  if (agno >= geo->agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("allocator");
  }
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot allocate zero blocks")
        .Metadata("allocator");
  }
  if (align <= 1) align = 1;
  if ((align & (align - 1)) != 0) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "alignment must be a power of two")
        .Metadata("allocator")
        .Actual(align);
  }
  if (impl_->alloc->rmap_enabled()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmapbt allocation requires an owner")
        .Ag(agno)
        .Metadata("allocator");
  }
  AgState* ag = nullptr;
  Status s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  return ag->AllocateAligned(len, align, nullptr, out);
}

Status BlockAllocator::AllocateFromAgOwned(uint32_t agno, uint32_t len,
                                           uint32_t align,
                                           const RmapOwner& owner,
                                           Allocation* out) {
  const XfsGeometry* geo = impl_->alloc->geo();
  if (agno >= geo->agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("allocator");
  }
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot allocate zero blocks")
        .Metadata("allocator");
  }
  if (align <= 1) align = 1;
  if ((align & (align - 1)) != 0) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "alignment must be a power of two")
        .Metadata("allocator")
        .Actual(align);
  }
  Status s = impl_->alloc->ReserveForRmap(agno);
  if (!s.ok()) return s;
  AgState* ag = nullptr;
  s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  return ag->AllocateAligned(len, align, &owner, out);
}

Status BlockAllocator::AllocateFromAgRange(uint32_t agno, uint32_t len,
                                           uint32_t align, uint32_t min_agbno,
                                           uint32_t max_agbno,
                                           Allocation* out) {
  const XfsGeometry* geo = impl_->alloc->geo();
  if (agno >= geo->agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("allocator");
  }
  if (len == 0 || min_agbno >= max_agbno) {
    return XFS_ERR(XfsError::kInvalidArgument, "invalid allocation range")
        .Metadata("allocator");
  }
  if (align <= 1) align = 1;
  if ((align & (align - 1)) != 0) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "alignment must be a power of two")
        .Metadata("allocator")
        .Actual(align);
  }
  if (impl_->alloc->rmap_enabled()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmapbt allocation requires an owner")
        .Ag(agno)
        .Metadata("allocator");
  }
  AgState* ag = nullptr;
  Status s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  return ag->AllocateAligned(len, align, nullptr, out, min_agbno, max_agbno);
}

Status BlockAllocator::AllocateFromAgRangeOwned(
    uint32_t agno, uint32_t len, uint32_t align, uint32_t min_agbno,
    uint32_t max_agbno, const RmapOwner& owner, Allocation* out) {
  const XfsGeometry* geo = impl_->alloc->geo();
  if (agno >= geo->agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("allocator");
  }
  if (len == 0 || min_agbno >= max_agbno) {
    return XFS_ERR(XfsError::kInvalidArgument, "invalid allocation range")
        .Metadata("allocator");
  }
  if (align <= 1) align = 1;
  if ((align & (align - 1)) != 0) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "alignment must be a power of two")
        .Metadata("allocator")
        .Actual(align);
  }
  Status s = impl_->alloc->ReserveForRmap(agno);
  if (!s.ok()) return s;
  AgState* ag = nullptr;
  s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  return ag->AllocateAligned(len, align, &owner, out, min_agbno, max_agbno);
}

Status BlockAllocator::Free(uint64_t fsb, uint32_t len) {
  if (impl_->alloc->rmap_enabled()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmapbt free requires an owner")
        .Fsb(fsb)
        .Metadata("allocator");
  }
  return impl_->alloc->FreeBlocksInternal(fsb, len, nullptr, false);
}

Status BlockAllocator::FreeOwned(uint64_t fsb, uint32_t len,
                                 const RmapOwner& owner) {
  return impl_->alloc->FreeBlocksInternal(fsb, len, &owner, false);
}

Status BlockAllocator::FreeUnknown(uint64_t fsb, uint32_t len) {
  if (!impl_->alloc->rmap_enabled()) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "OWN_UNKNOWN free requires rmapbt")
        .Fsb(fsb)
        .Metadata("allocator");
  }
  return impl_->alloc->FreeBlocksInternal(fsb, len, nullptr, true);
}

Status BlockAllocator::ConvertOwned(uint64_t fsb, uint32_t len,
                                    const RmapOwner& owner,
                                    bool old_unwritten) {
  if (!impl_->alloc->rmap_enabled()) return OkStatus();
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "convert range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("allocator");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  Status s = impl_->alloc->ReserveForRmap(agno);
  if (!s.ok()) return s;
  AgState* ag = nullptr;
  s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  (void)ag;
  return impl_->alloc->ConvertOwnedRmap(agno, agbno, len, owner,
                                        old_unwritten);
}

Status BlockAllocator::RmapMap(uint64_t fsb, uint32_t len,
                               const RmapOwner& owner) {
  if (!impl_->alloc->rmap_enabled()) return OkStatus();
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmap map range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("rmapbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  Status s = impl_->alloc->ReserveForRmap(agno);
  if (!s.ok()) return s;
  AgState* ag = nullptr;
  s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  (void)ag;
  return impl_->alloc->MapOwned(agno, agbno, len, owner);
}

Status BlockAllocator::RmapUnmap(uint64_t fsb, uint32_t len,
                                 const RmapOwner& owner) {
  if (!impl_->alloc->rmap_enabled()) return OkStatus();
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmap unmap range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("rmapbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  Status s = impl_->alloc->ReserveForRmap(agno);
  if (!s.ok()) return s;
  AgState* ag = nullptr;
  s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  (void)ag;
  return impl_->alloc->UnmapOwned(agno, agbno, len, owner, false);
}

Status BlockAllocator::RmapConvert(uint64_t fsb, uint32_t len,
                                   const RmapOwner& owner,
                                   bool old_unwritten) {
  if (!impl_->alloc->rmap_enabled()) return OkStatus();
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmap convert range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("rmapbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  Status s = impl_->alloc->ReserveForRmap(agno);
  if (!s.ok()) return s;
  AgState* ag = nullptr;
  s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  (void)ag;
  return impl_->alloc->ConvertOwnedRmap(agno, agbno, len, owner,
                                        old_unwritten);
}

Status BlockAllocator::EnsureAgfl(uint32_t agno, uint32_t blocks) {
  const XfsGeometry* geo = impl_->alloc->geo();
  if (agno >= geo->agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("allocator");
  }
  return impl_->alloc->EnsureAgflInternal(agno, blocks);
}

Status BlockAllocator::Commit() { return impl_->alloc->Commit(); }

int64_t BlockAllocator::fdblocks_delta() const {
  return impl_->alloc->fdblocks_delta();
}

}  // namespace xfs
