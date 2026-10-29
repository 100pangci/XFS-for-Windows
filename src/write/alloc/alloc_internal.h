// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - module-private block allocator internals.
//
// This header is private to src/write/alloc.  It is not part of the public
// xfs/ API.
//
// Invariant ownership of this module:
//
//   alloc_tree.cpp owns the in-memory bnobt/cntbt block layout: record/key
//   ordering, split, merge, borrow, sibling links, root growth/collapse.  It
//   does not own AGF counters, the AGFL, allocation policy or rmap/refcount
//   ownership.
//
//   alloc_ag.cpp owns one AG's free-space state: the AGF freeblks/longest
//   counters, the AGFL ring, the canonical disjoint free-record list and the
//   best-fit/aligned allocation search.  It does not own the ownership ledger
//   (rmap/refcount) or the public allocator API.
//
//   alloc_ownership.cpp owns the reverse-mapping and refcount integration:
//   owner-aware allocate/free/convert, CoW staging, release-on-last-reference
//   and RUI/CUI completion hooks.  It does not own free-space tree edits; it
//   calls into the AG/tree layer.
//
//   alloc_core.cpp owns transaction-level orchestration and publication:
//   loading AGs, the free/counter identity, the superblock fdblocks delta and
//   the single Commit() that stamps LSN/CRC and queues every dirty buffer.
//
//   alloc.cpp owns only the public BlockAllocator facade.
//
// The commit ordering is deliberately unchanged: deferred AGFL returns are
// settled first, refcount/rmap blocks are published, then AGF/AGFL/btree and
// superblock buffers are added to the transaction.  No module writes metadata
// before Commit().
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "xfs/alloc.h"
#include "xfs/block_device.h"
#include "xfs/refcount.h"
#include "xfs/refcount_editor.h"
#include "xfs/rmap_editor.h"
#include "xfs/superblock.h"
#include "xfs/transaction.h"

namespace xfs {
namespace alloc_detail {

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
int CompareRec(TreeKind kind, const FreeRec& a, const FreeRec& b);

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
  // Tree shrink can return more blocks than fit in the active AGFL. Keep
  // overflow transaction-local until ownership edits can safely drain it.
  std::vector<uint32_t> deferred_tree_blocks;
  uint32_t freeblks = 0;
  uint32_t longest = 0;
  uint32_t orig_freeblks = 0, orig_flcount = 0, orig_btreeblks = 0;
  uint32_t rmap_root = 0, rmap_level = 0, rmap_blocks = 0;
  uint32_t refcount_root = 0, refcount_level = 0, refcount_blocks = 0;

  std::vector<FreeRec> recs;  // sorted by start, disjoint
  std::unique_ptr<AllocTree> bno, cnt;

  bool refilling = false;
  bool counted = false;
  bool rmap_loaded = false;
  bool refcount_loaded = false;

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
  Status DrainDeferredTreeBlocks();
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
  bool reflink_enabled() const { return geo_->features.reflink; }
  bool rmap_editing() const { return rmap_editing_; }
  uint32_t RmapBlocks(uint32_t agno) const;
  Status MapOwned(uint32_t agno, uint32_t agbno, uint32_t len,
                  const RmapOwner& owner);
  Status UnmapOwned(uint32_t agno, uint32_t agbno, uint32_t len,
                    const RmapOwner& owner, bool unknown_owner,
                    bool no_overlap_ok = false);
  Status ConvertOwnedRmap(uint32_t agno, uint32_t agbno, uint32_t len,
                          const RmapOwner& owner, bool old_unwritten);
  Status MapMetadata(uint32_t agno, uint32_t agbno, uint32_t len,
                     uint64_t owner);
  Status UnmapMetadata(uint32_t agno, uint32_t agbno, uint32_t len,
                       uint64_t owner);
  Status AllocateRmapBlock(uint32_t agno, uint32_t* agbno);
  Status FreeRmapBlock(uint32_t agno, uint32_t agbno);
  Status AllocateRefcountBlock(uint32_t agno, uint32_t* agbno);
  Status FreeRefcountBlock(uint32_t agno, uint32_t agbno);
  Status ReserveForRmap(uint32_t agno);
  Status EnsureAgflInternal(uint32_t agno, uint32_t want);
  Status FreeBlocksInternal(uint64_t fsb, uint32_t len, const RmapOwner* owner,
                            bool unknown_owner);
  Status FreeRaw(uint32_t agno, uint32_t agbno, uint32_t len);
  Status ReleaseDataInternal(uint64_t fsb, uint32_t len,
                             const RmapOwner& owner);
  Status RefcountIncreaseInternal(uint32_t agno, uint32_t agbno, uint32_t len);
  Status RefcountDecreaseInternal(uint32_t agno, uint32_t agbno, uint32_t len,
                                  bool unmap_unknown, bool free_raw);
  Status CowAllocInternal(uint32_t agno, uint32_t agbno, uint32_t len);
  Status CowFreeInternal(uint32_t agno, uint32_t agbno, uint32_t len);
  Status RefcountFreeCallback(uint32_t agno, uint32_t agbno, uint32_t len);
  Status FreePendingRefcountBlocks();
  Status LoadRefcount(uint32_t agno);
  bool FindSharedExtent(uint64_t fsb, uint32_t len, RefcountRecord* out) const;
  Status ListCowStaging(uint32_t agno,
                        std::vector<RefcountRecord>* out) const;

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
  std::unique_ptr<RefcountEditor> refcount_;
  bool rmap_editing_ = false;
  bool refcount_editing_ = false;
  bool refcount_free_unknown_ = false;
  int64_t fdblocks_delta_ = 0;
  std::map<uint32_t, std::vector<uint32_t>> pending_refcount_frees_;
};

}  // namespace alloc_detail
}  // namespace xfs
