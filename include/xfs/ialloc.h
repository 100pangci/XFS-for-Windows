// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - inode allocator (RW-3, extended in RW-6A).
//
// Scope: the writable profile (XFS v5, crc=1, no rmapbt/reflink, no quotas).
// Inode allocation changes:
//
//   AGI          count, freecount, newino, unlinked[] (through the namespace
//                layer), root/level when the inobt grows or shrinks,
//                free_root/free_level for finobt, and iblocks/fblocks when
//                inobtcount is enabled
//   inobt        one record per 64-inode chunk
//   finobt       only chunks with free inodes; updated in the same transaction
//   superblock   sb_icount / sb_ifree
//
// A full chunk is a contiguous run of `ialloc_blks` blocks, inserted as a
// holemask-0 record. With SPINODES and no room for a full chunk, a
// cluster-sized (`ialloc_min_blks`) run is allocated inside the sparse window
// and inserted/merged as a sparse record. The freshly allocated inode
// clusters are initialised with pristine free-inode images (magic, version,
// generation, di_ino, uuid, CRC) through an XFS_LI_ICREATE log item - the same
// mechanism Linux uses - so replay rebuilds the clusters even if the inode
// buffer write was lost.
//
// Records are encoded per the SPINODES feature: full records use
// startino/freecount be32/free, sparse records use
// startino/holemask be16/count u8/freecount u8/free.
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

// One inobt record (host order). Sparse records use the same host structure;
// the on-disk encoding is selected by the SPINODES feature.
struct InobtRecord {
  uint32_t startino = 0;  // first inode number of the chunk (AG relative)
  uint32_t freecount = 0;
  uint64_t free = 0;  // bit i set = inode i of the chunk is free
  uint16_t holemask = 0;  // sparse: bit set = 4 inodes with no backing
  uint8_t count = 64;     // sparse: number of backed inodes (4..64)
};

// Inode-granularity allocation mask: bit i set means inode i has backing
// storage (xfs_inobt_irec_to_allocmask).
inline uint64_t InobtAllocMask(uint16_t holemask) {
  const uint32_t alloc = (~holemask) & 0xffffu;
  uint64_t mask = 0;
  for (uint32_t bit = 0; bit < 16; ++bit) {
    if ((alloc >> bit) & 1u) {
      mask |= 0xfull << (bit * 4);
    }
  }
  return mask;
}

inline bool InobtIsSparse(uint16_t holemask) { return holemask != 0; }

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

  // Allocates a new, completely free inode chunk in `agno` (xfs_ialloc_ag_alloc
  // without handing out an inode). No-op when the AG already has free inodes.
  // Used before a create so that the create transaction never has to carry a
  // chunk allocation; a crash in between leaves a legal filesystem with more
  // free inodes, never an unreachable inode. Must run in its own transaction.
  Status PreallocateChunk(uint32_t agno);

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
  uint32_t ialloc_inos = 0;      // inodes per full chunk (64)
  uint32_t ialloc_blks = 0;      // blocks per full chunk
  uint32_t ialloc_min_blks = 0;  // smallest sparse allocation (spino_align)
  uint32_t inodes_per_cluster = 0;
  uint32_t blocks_per_cluster = 0;
  uint32_t cluster_align = 0;    // block alignment for a full chunk
  uint32_t spino_align = 0;      // sparse allocation/alignment in blocks
  uint32_t inodes_per_block = 0;
};

XfsInodeGeometry MakeInodeGeometry(const XfsSuperblock& sb,
                                   const XfsGeometry& geo);

}  // namespace xfs
