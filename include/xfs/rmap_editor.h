// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - writable reverse mapping (rmapbt) editor.
//
// The reader in xfs/rmap.h exposes a verified, immutable RmapTree.  This
// header is the write-side abstraction: every logical record mutation goes
// through RmapEditor, which owns the in-memory B+tree and its low/high keys.
// Allocators and fork editors must never hand-edit rmap nodes.
//
// Physical tree block allocation is deliberately injected as callbacks.  On a
// real filesystem rmapbt blocks come from the AGFL (xfs_rmapbt_alloc_block),
// *not* from the generic free-space allocator: taking a block from the free
// trees would recursively need another rmap update.  The callbacks also keep
// the AGFL/AGF counters and the rmap tree's own `agf_rmap_blocks` accounting in
// the same transaction as the mutation.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/rmap.h"
#include "xfs/superblock.h"

namespace xfs {

// One logical owner reference.  `offset` is the data/attr fork logical block
// for inode extents, and zero for metadata, bmbt and special owners.
struct RmapOwner {
  uint64_t owner = 0;
  uint64_t offset = 0;
  uint32_t flags = 0;

  static RmapOwner Data(uint64_t ino, uint64_t logical_block,
                        bool unwritten = false) {
    return RmapOwner{ino, logical_block,
                     unwritten ? kRmapUnwritten : 0u};
  }
  static RmapOwner Attr(uint64_t ino, uint64_t logical_block,
                        bool unwritten = false) {
    return RmapOwner{ino, logical_block,
                     kRmapAttrFork | (unwritten ? kRmapUnwritten : 0u)};
  }
  static RmapOwner Bmbt(uint64_t ino, bool attr = false) {
    return RmapOwner{ino, 0, kRmapBmbtBlock | (attr ? kRmapAttrFork : 0u)};
  }
  static RmapOwner Metadata(uint64_t owner) { return RmapOwner{owner, 0, 0}; }
};

// Result of a logical edit, useful for diagnostics and tests.
struct RmapEditStats {
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
};

class RmapEditor {
 public:
  // Allocation/free of an rmapbt tree block.  Production implementations use
  // the AGFL only; they must not call back into MapExtent/UnmapExtent.
  using AllocBlockFn = std::function<Status(uint32_t agno, uint32_t* agbno)>;
  using FreeBlockFn = std::function<Status(uint32_t agno, uint32_t agbno)>;
  // Publishes one complete dirty rmapbt block (LSN/CRC are added by the
  // transaction layer).  It is called exactly once per dirty block at Commit.
  using PublishBlockFn = std::function<Status(
      uint32_t agno, uint32_t agbno, const uint8_t* data, size_t len)>;

  struct Hooks {
    AllocBlockFn alloc_block;
    FreeBlockFn free_block;
    PublishBlockFn publish_block;
  };

  RmapEditor(BlockDevice* device, const XfsGeometry* geo, Hooks hooks);
  ~RmapEditor();
  RmapEditor(const RmapEditor&) = delete;
  RmapEditor& operator=(const RmapEditor&) = delete;

  // Loads and verifies AG `agno`'s tree.  `root`, `level` and `blocks` come
  // from the AGF the caller already parsed.  Loading is idempotent; the
  // allocator calls it once per transaction/AG.
  Status Load(uint32_t agno, uint32_t root, uint32_t level, uint32_t blocks);
  bool IsLoaded(uint32_t agno) const;

  // Typed lookups over the in-memory tree.  RmapTree's semantics are reused
  // by constructing its record view; callers that need ranges should use
  // Records().
  Result<RmapRecord> LookupEq(uint32_t agno, const RmapKey& key) const;
  Result<RmapRecord> LookupLE(uint32_t agno, const RmapKey& key) const;
  Result<RmapRecord> LookupGE(uint32_t agno, const RmapKey& key) const;

  // Exact record edits.  InsertExact rejects duplicate keys and overlapping
  // physical ranges.  DeleteExact requires the key to exist.  UpdateExact
  // replaces one record and re-runs physical normalization only when the key
  // changes.
  Status InsertExact(uint32_t agno, const RmapRecord& rec);
  Status DeleteExact(uint32_t agno, const RmapKey& key);
  Status UpdateExact(uint32_t agno, const RmapRecord& old_rec,
                     const RmapRecord& new_rec);

  // Linux xfs_rmap_map.  The incoming range must not overlap an existing
  // record.  It may merge left, right, both, or stay isolated; merging needs
  // physical adjacency, equal owner, all three flags, and (except for special
  // owners/bmbt records) logical adjacency.
  Status MapExtent(uint32_t agno, const RmapRecord& rec);

  // Linux xfs_rmap_unmap.  `key` must match owner/offset/flags; `len` must be
  // covered by the record.  Handles exact, prefix, suffix and middle removal.
  // `unknown_owner` implements the EFI OWN_UNKNOWN wildcard (fully covering
  // record or no overlap at all; partial overlap is corruption).
  Status UnmapExtent(uint32_t agno, const RmapKey& key, uint32_t len,
                     bool unknown_owner = false,
                     bool no_overlap_ok = false);

  // Linux xfs_rmap_convert.  `old_unwritten` is the current on-disk state;
  // the target is the opposite state.  Exact, prefix, suffix and middle
  // ranges are supported and merge with eligible target-state neighbours.
  Status ConvertExtent(uint32_t agno, const RmapKey& key, uint32_t len,
                       bool old_unwritten);

  // Metadata ownership helpers (OWN_AG, OWN_INOBT, OWN_INODES, ...).  These
  // are thin wrappers so callers never construct special-owner records by
  // hand.
  Status AllocateMetadata(uint32_t agno, uint32_t startblock, uint32_t len,
                          uint64_t owner);
  Status FreeMetadata(uint32_t agno, uint32_t startblock, uint32_t len,
                      uint64_t owner);

  // Convenience wrappers used by fork editors and the allocator.
  Status AllocateExtent(uint32_t agno, uint32_t startblock, uint32_t len,
                        const RmapOwner& owner);
  Status FreeExtent(uint32_t agno, uint32_t startblock, uint32_t len,
                    const RmapOwner& owner, bool unknown_owner = false,
                    bool no_overlap_ok = false);
  Status Convert(uint32_t agno, uint32_t startblock, uint32_t len,
                 const RmapOwner& owner, bool old_unwritten);

  // AGF view after mutations.
  uint32_t Root(uint32_t agno) const;
  uint32_t Level(uint32_t agno) const;
  uint32_t Blocks(uint32_t agno) const;
  const std::vector<RmapRecord>& Records(uint32_t agno) const;

  // Number of tree blocks an insert/delete may need at the current height.
  // The allocator reserves this many AGFL entries before entering a mutation;
  // existence of the reservation is what prevents recursive allocation.
  uint32_t ReserveBlocks(uint32_t agno) const;

  // Publishes every dirty block through PublishBlockFn.  Must be called before
  // the owning transaction commits.
  Status Commit();

  // In-memory structural verification (keys, siblings, levels, counters).
  // This is intentionally independent of the on-disk reader: it catches
  // writer bugs before the buffers are published.
  Status Verify(uint32_t agno) const;

  const RmapEditStats& stats() const { return stats_; }
  void ResetStats() { stats_ = {}; }

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  RmapEditStats stats_;
};

}  // namespace xfs
