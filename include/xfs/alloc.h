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

#include "xfs/block_device.h"
#include "xfs/error.h"
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
  Status Allocate(uint32_t preferred_agno, uint32_t len, Allocation* out);

  // Frees [fsb, fsb+len), which must lie inside one AG and be currently
  // allocated. Contiguous free neighbours are merged.
  Status Free(uint64_t fsb, uint32_t len);

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
