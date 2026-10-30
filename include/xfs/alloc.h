// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - allocation group block allocator (RW-2).
//
// Scope: the writable profile (XFS v5, crc=1, no finobt/rmapbt/reflink/no
// sparse inodes/no quota). For that profile a block allocation changes exactly
// this metadata:
//
//   AGF          freeblks, longest, flfirst/fllast/flcount, btreeblks,
//                bno/cnt root + level when a tree grows or shrinks
//   AGFL         the reserved-block ring entries
//   bnobt        free extents ordered by start block
//   cntbt        free extents ordered by (block count, start block)
//   superblock   sb_fdblocks (incl. the AGFL and the non-root free space btree
//                blocks, which XFS counts as free: fdblocks = sum over AGs of
//                freeblks + flcount + btreeblks, exactly what the kernel's
//                xfs_initialize_perag_data and xfs_repair compute)
//
// Every change goes through the transaction: buffers are only mutated in
// memory, and Commit() stamps the transaction LSN and fresh CRCs, logs each
// buffer as an XFS_LI_BUF item and queues its write-back. Linux replays those
// items natively (docs/JOURNAL.md section 4).
//
// Allocator behaviour (deliberately conservative, never silently unsafe):
//   - one AG per allocation, contiguous run only;
//   - best fit by length in cntbt order (allocate from the start of the free
//     extent, exactly like xfs_alloc_ag_vextent_size with alignment 1);
//   - freeing merges with contiguous neighbours on both sides;
//   - btree blocks for the free space trees come from the AGFL; when the AGFL
//     runs dry the allocator takes one block back from free space first
//     (xfs_alloc_fix_freelist); structural mutations that this build cannot
//     perform terminate with kUnsupportedFeature instead of writing a tree
//     that is not valid.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/refcount.h"
#include "xfs/rmap_editor.h"
#include "xfs/superblock.h"
#include "xfs/transaction.h"

namespace xfs {

// One allocation returned by BlockAllocator::Allocate: a contiguous run of
// blocks inside a single AG, addressed by its absolute filesystem block number.
struct Allocation {
  uint64_t fsb = 0;
  uint32_t len = 0;
};

class BlockAllocator {
 public:
  BlockAllocator(XfsTransaction* txn, BlockDevice* device,
                 const XfsGeometry* geo);
  ~BlockAllocator();
  BlockAllocator(const BlockAllocator&) = delete;
  BlockAllocator& operator=(const BlockAllocator&) = delete;

  // Allocates `len` (>= 1) contiguous blocks from a single AG, preferring
  // `preferred_agno` and then scanning the remaining AGs in order. Fails with
  // kNoSpace when no AG has a free extent that large, and with
  // kUnsupportedFeature when the allocation would require a tree mutation this
  // build does not implement (the AGFL cannot provide a block).
  //
  // On an rmapbt filesystem this unowned form is refused: every allocation has
  // exactly one owner and the rmap record must be created in the same
  // transaction as the free-space change.  Use Allocate*Owned instead.
  Status Allocate(uint32_t preferred_agno, uint32_t len, Allocation* out);

  Status AllocateOwned(uint32_t preferred_agno, uint32_t len,
                       const RmapOwner& owner, Allocation* out);

  // Allocates `len` contiguous blocks from *one* AG (never another), with the
  // run's start block aligned to `align` blocks (a power of two, 1 = no
  // alignment). This is the inode chunk allocation (`xfs_ialloc_ag_alloc`
  // with a cluster/inode-chunk alignment). Fails with kNoSpace when that AG
  // has no free extent that fits an aligned run.
  Status AllocateFromAg(uint32_t agno, uint32_t len, uint32_t align,
                        Allocation* out);
  Status AllocateFromAgOwned(uint32_t agno, uint32_t len, uint32_t align,
                             const RmapOwner& owner, Allocation* out);

  // Like AllocateFromAg, but the run must start at or after `min_agbno` and
  // end at or before `max_agbno` (both AG-relative blocks, max exclusive).
  // This is the sparse inode chunk allocation window
  // (xfs_ialloc_ag_alloc's sparse_alloc path).
  Status AllocateFromAgRange(uint32_t agno, uint32_t len, uint32_t align,
                             uint32_t min_agbno, uint32_t max_agbno,
                             Allocation* out);
  Status AllocateFromAgRangeOwned(uint32_t agno, uint32_t len, uint32_t align,
                                  uint32_t min_agbno, uint32_t max_agbno,
                                  const RmapOwner& owner, Allocation* out);

  // Frees [fsb, fsb+len), which must lie inside one AG and be currently
  // allocated. Contiguous free neighbours are merged.
  //
  // The unowned form is refused on rmapbt filesystems.  FreeOwned validates
  // the reverse mapping (owner, offset, flags) before the free-space tree is
  // changed.  FreeUnknown implements the EFI OWN_UNKNOWN wildcard: a fully
  // covering record is removed, no overlap is accepted as an idempotent replay,
  // and partial overlap is refused.
  Status Free(uint64_t fsb, uint32_t len);
  Status FreeOwned(uint64_t fsb, uint32_t len, const RmapOwner& owner);
  Status FreeUnknown(uint64_t fsb, uint32_t len);

  // Cancels a metadata buffer whose owner is releasing it (`len` filesystem
  // blocks). Free-owner paths call this before returning the blocks to free
  // space or the AGFL. It must not be used for file data, remote xattr values,
  // or CoW staging: those are direct writes with no buffer log item to cancel.
  Status InvalidateMetadata(uint64_t fsb, uint32_t len, const char* what);

  // Removes one inode mapping from the ownership ledger.  On a reflink
  // filesystem the owner rmap is removed, the refcount is decreased, and the
  // physical blocks are returned to free space only when the last reference
  // disappears.  On older profiles this is exactly FreeOwned.
  Status ReleaseData(uint64_t fsb, uint32_t len, const RmapOwner& owner);

  // CoW staging.  AllocateCowStaging takes a private run from free space with
  // rmap owner OWN_COW and records it in refcountbt with COW flag/refcount 1.
  // CommitCowStaging atomically removes the staging record/owner and installs
  // the final inode owner; the physical blocks are already allocated.
  // ReleaseCowStaging cancels an orphan staging extent (mount recovery).
  Status AllocateCowStaging(uint32_t preferred_agno, uint32_t len,
                            Allocation* out);
  Status CommitCowStaging(uint64_t fsb, uint32_t len,
                          const RmapOwner& final_owner);
  Status ReleaseCowStaging(uint64_t fsb, uint32_t len);

  // Refcount adjustments used by clone, truncate and CUI recovery.
  // RefcountDecrease with unknown_owner implements the CUI recovery path: a
  // range that reaches zero is freed through the OWN_UNKNOWN wildcard.
  Status RefcountIncrease(uint64_t fsb, uint32_t len);
  Status RefcountDecrease(uint64_t fsb, uint32_t len,
                          bool unknown_owner = false);
  Status RefcountCowAlloc(uint64_t fsb, uint32_t len);
  Status RefcountCowFree(uint64_t fsb, uint32_t len);

  // Loads AG `agno`'s refcount tree and reports the first shared run that
  // intersects the physical range.  Returns found=false when not shared.
  Status FindShared(uint64_t fsb, uint32_t len, RefcountRecord* out,
                    bool* found);
  // Enumerates CoW staging records in one AG (requires the tree loaded).
  Status LoadRefcountAg(uint32_t agno);
  Status ListCowStaging(uint32_t agno, std::vector<RefcountRecord>* out);

  // Written <-> unwritten state conversion for an inode data extent.  This is
  // the rmap half of the bmap conversion; it never changes free space.
  Status ConvertOwned(uint64_t fsb, uint32_t len, const RmapOwner& owner,
                      bool old_unwritten);

  // RUI completion: apply the rmap half of a recovered intent without
  // touching the free-space trees (the original transaction already did).
  Status RmapMap(uint64_t fsb, uint32_t len, const RmapOwner& owner);
  Status RmapUnmap(uint64_t fsb, uint32_t len, const RmapOwner& owner,
                   bool no_overlap_ok = false);
  Status RmapConvert(uint64_t fsb, uint32_t len, const RmapOwner& owner,
                     bool old_unwritten);

  // Ensures the AGFL holds at least `blocks` entries, refilling from free
  // space (and recording OWN_AG ownership) before a structural rmap edit.
  // This is the reservation that prevents AllocateRmapBlock recursion.
  Status EnsureAgfl(uint32_t agno, uint32_t blocks);

  // Adds every dirty metadata buffer to the transaction (LSN + CRC, buffer log
  // items, write-back) and applies the superblock free block delta. Must be
  // called exactly once after the last Allocate/Free and before the commit.
  Status Commit();

  // Net change of the free block counters (sb_fdblocks) accumulated so far.
  int64_t fdblocks_delta() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xfs
