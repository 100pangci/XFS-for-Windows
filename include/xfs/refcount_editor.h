// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - writable reference-count btree editor (RW-6C).
//
// The reader exposes a verified immutable RefcountTree.  This header is the
// write-side abstraction.  It owns the in-memory B+tree (leaf/internal split,
// merge, borrow, root growth/collapse and sibling maintenance) and implements
// the xfs_refcount_adjust / _adjust_cow state machines.
//
// Tree blocks for refcountbt come from the regular free-space allocator with
// rmap owner OWN_REFC -- not from the AGFL.  The allocator callbacks keep the
// AGF refcount root/level/blocks fields, free-space btrees, AGFL and rmap in
// the same transaction.  All metadata is staged: nothing reaches the device
// until Commit() publishes the verified blocks.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/refcount.h"
#include "xfs/superblock.h"

namespace xfs {

struct RefcountEditStats {
  uint64_t inserted = 0;
  uint64_t deleted = 0;
  uint64_t split_leaf = 0;
  uint64_t split_node = 0;
  uint64_t merge_leaf = 0;
  uint64_t merge_node = 0;
  uint64_t borrow_leaf = 0;
  uint64_t borrow_node = 0;
  uint64_t root_grow = 0;
  uint64_t root_collapse = 0;
  uint64_t shared_increase = 0;
  uint64_t shared_decrease = 0;
  uint64_t cow_alloc = 0;
  uint64_t cow_free = 0;
  uint64_t freed_ranges = 0;
};

class RefcountEditor {
 public:
  // Allocate/free one refcountbt tree block.  The allocator callback is
  // responsible for the rmap OWN_REFC record and free-space trees; the editor
  // maintains agf_refcount_blocks itself.
  using AllocBlockFn = std::function<Status(uint32_t agno, uint32_t* agbno)>;
  using FreeBlockFn = std::function<Status(uint32_t agno, uint32_t agbno)>;
  // Publish one complete dirty refcountbt block (LSN/CRC are added by the
  // transaction layer).
  using PublishBlockFn = std::function<Status(
      uint32_t agno, uint32_t agbno, const uint8_t* data, size_t len)>;
  // Called for each physical range whose shared refcount reached zero.  The
  // caller removes this mapping's rmap before calling Decrease, then this
  // callback frees the free-space extents.  COW effects never use this hook.
  using FreeExtentFn = std::function<Status(uint32_t agno,
                                            uint32_t startblock,
                                            uint32_t blockcount)>;

  struct Hooks {
    AllocBlockFn alloc_block;
    FreeBlockFn free_block;
    PublishBlockFn publish_block;
    FreeExtentFn free_extent;
  };

  RefcountEditor(BlockDevice* device, const XfsGeometry* geo, Hooks hooks);
  ~RefcountEditor();
  RefcountEditor(const RefcountEditor&) = delete;
  RefcountEditor& operator=(const RefcountEditor&) = delete;

  Status Load(uint32_t agno, uint32_t root, uint32_t level, uint32_t blocks);
  bool IsLoaded(uint32_t agno) const;

  Result<RefcountRecord> LookupEq(uint32_t agno,
                                  const RefcountKey& key) const;
  Result<RefcountRecord> LookupLE(uint32_t agno,
                                  const RefcountKey& key) const;
  Result<RefcountRecord> LookupGE(uint32_t agno,
                                  const RefcountKey& key) const;
  bool OverlapsShared(uint32_t agno, uint32_t agbno, uint32_t len) const;
  bool FindShared(uint32_t agno, uint32_t agbno, uint32_t len,
                  RefcountRecord* out) const;
  bool FindCow(uint32_t agno, uint32_t agbno, uint32_t len,
               RefcountRecord* out) const;

  // Exact record edits used by the logical layer and unit tests.
  Status InsertExact(uint32_t agno, const RefcountRecord& rec);
  Status DeleteExact(uint32_t agno, const RefcountKey& key);
  Status UpdateExact(uint32_t agno, const RefcountRecord& old_rec,
                     const RefcountRecord& new_rec);

  // xfs_refcount_adjust(SHARED, INCREASE/DECREASE).  Increase creates records
  // for implied-refcount-1 holes; decrease removes records that fall to one
  // and invokes FreeExtent when the last reference disappears.
  Status Increase(uint32_t agno, uint32_t agbno, uint32_t len);
  Status Decrease(uint32_t agno, uint32_t agbno, uint32_t len);

  // xfs_refcount_alloc_cow_extent / xfs_refcount_free_cow_extent.  These only
  // touch the refcountbt; the caller maintains OWN_COW rmap and free space.
  Status CowAlloc(uint32_t agno, uint32_t agbno, uint32_t len);
  Status CowFree(uint32_t agno, uint32_t agbno, uint32_t len);

  uint32_t Root(uint32_t agno) const;
  uint32_t Level(uint32_t agno) const;
  uint32_t Blocks(uint32_t agno) const;
  const std::vector<RefcountRecord>& Records(uint32_t agno) const;

  // Worst-case number of tree blocks a single record edit may need.
  uint32_t ReserveBlocks(uint32_t agno) const;

  // Verifies every in-memory tree, then publishes all dirty blocks.
  Status Commit();
  Status Verify(uint32_t agno) const;

  const RefcountEditStats& stats() const { return stats_; }
  void ResetStats() { stats_ = {}; }

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  RefcountEditStats stats_;
};

}  // namespace xfs
