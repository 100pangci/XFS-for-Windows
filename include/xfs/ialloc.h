// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - inode allocator (RW-3).
//
// Scope: the writable profile (XFS v5, crc=1, no finobt/rmapbt/reflink, no
// sparse inodes, no quotas). For that profile inode allocation changes:
//
//   AGI          count, freecount, newino, unlinked[] (through the namespace
//                layer), root/level when the inobt grows or shrinks
//   inobt        one record per 64-inode chunk:
//                  startino be32, freecount be32 (no-sparse layout), free be64
//                - allocating clears a bit in `free` and decrements freecount,
//                - freeing sets the bit and increments freecount; a chunk that
//                  becomes fully free is removed from the inobt and its blocks
//                  are freed when the filesystem stores <= 64 inodes per
//                  block (xfs_difree_inobt), which is the same condition Linux
//                  applies for this geometry,
//   superblock   sb_icount / sb_ifree
//
// A new chunk is allocated exactly like xfs_ialloc_ag_alloc: a contiguous run
// of `ialloc_blks` blocks (64 inodes), preferring the block right after the
// last chunk (AGI newino), inserted as a full (holemask 0) inobt record with
// all 64 inodes free. The freshly allocated inode cluster is initialised with
// pristine free-inode images (magic, version, generation, di_ino, uuid, CRC)
// through an XFS_LI_ICREATE log item - the same mechanism Linux uses - so
// replay rebuilds the cluster even if the inode buffer write was lost.
//
// Everything here happens inside a transaction: buffers are only mutated in
// memory, and Commit() logs them (LSN + CRC stamped) and queues their write.
// A refused allocation never writes anything.
#pragma once

#include <cstdint>
#include <memory>

#include "xfs/alloc.h"
#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/superblock.h"
#include "xfs/transaction.h"

namespace xfs {

// One inobt record (host order).
struct InobtRecord {
  uint32_t startino = 0;  // first inode number of the chunk (AG relative)
  uint32_t freecount = 0;
  uint64_t free = 0;  // bit i set = inode i of the chunk is free
};

class InodeAllocator {
 public:
  InodeAllocator(XfsTransaction* txn, BlockAllocator* alloc, BlockDevice* device,
                 const XfsSuperblock* sb, const XfsGeometry* geo);
  ~InodeAllocator();
  InodeAllocator(const InodeAllocator&) = delete;
  InodeAllocator& operator=(const InodeAllocator&) = delete;

  // Allocates one inode, preferring `preferred_agno` (the parent directory's
  // AG, exactly like xfs_dialloc) and, inside it, a chunk near
  // `preferred_agino` (0 = no preference). Allocates a new chunk when the
  // filesystem has no free inode; fails with kNoSpace when nothing can be
  // allocated.
  Status AllocateInode(uint32_t preferred_agno, uint32_t preferred_agino,
                       uint64_t* ino_out);

  // Frees an allocated inode: clears the inobt free bit, updates the AGI and
  // superblock counters, and removes the chunk (freeing its blocks through the
  // block allocator) when the whole chunk became free. The caller must have
  // already truncated the inode to zero size and removed it from the AGI
  // unlinked list.
  Status FreeInode(uint64_t ino);

  // AGI unlinked list support (xfs_iunlink / xfs_iunlink_remove). These only
  // touch the AGI bucket; the caller updates di_next_unlinked in the inode
  // items.
  // Reads the bucket head (NULLAGINO = 0xffffffff when empty).
  Result<uint32_t> UnlinkedBucketHead(uint32_t agno, uint32_t bucket);
  // Sets the bucket head to `agino` (NULLAGINO clears it).
  Status SetUnlinkedBucketHead(uint32_t agno, uint32_t bucket, uint32_t agino);

  // Adds AGI/superblock deltas and every dirty buffer to the transaction (LSN
  // + CRC, log items, write-back). Must be called once before the commit.
  Status Commit();

  // Superblock inode counter deltas accumulated by this allocator (for tests
  // and diagnostics).
  int64_t icount_delta() const;
  int64_t ifree_delta() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Geometry values libxfs derives from the superblock (xfs_ialloc_setup_geometry).
struct XfsInodeGeometry {
  uint32_t ialloc_inos = 0;     // inodes per chunk allocation (64)
  uint32_t ialloc_blks = 0;     // blocks per chunk allocation
  uint32_t inodes_per_cluster = 0;
  uint32_t blocks_per_cluster = 0;
  uint32_t cluster_align = 0;   // block alignment for chunk allocation
  uint32_t inodes_per_block = 0;
};

XfsInodeGeometry MakeInodeGeometry(const XfsSuperblock& sb,
                                   const XfsGeometry& geo);

}  // namespace xfs
