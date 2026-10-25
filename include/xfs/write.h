// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - read-write layer: policy, range mapping, mutation API.
//
// Layering (see docs/DESIGN.md):
//
//   WinFsp / CLI
//        |
//        v
//   XFS high-level mutation API      (XfsFilesystem::OverwriteFileData, ...)
//        |
//        v
//   transaction layer                (transactions/log, once metadata changes)
//        |
//        v
//   allocator / inode / bmap / directory
//        |
//        v
//   BlockDevice
//
// Everything in this header is about mutations. Read paths stay in
// xfs/filesystem.h, xfs/extent.h and xfs/directory.h.
#pragma once

#include <cstdint>
#include <vector>

#include "xfs/error.h"
#include "xfs/extent.h"
#include "xfs/inode.h"
#include "xfs/superblock.h"

namespace xfs {

// ---------------------------------------------------------------------------
// Writable mount policy
// ---------------------------------------------------------------------------
// Checks that the filesystem uses a feature profile this build can modify.
// RW-6A maintains finobt, inobtcount, sparse inode chunks, bigtime and
// NREXT64.  RW-6B maintains rmapbt, and RW-6C maintains reflink/refcountbt/
// CoW.  Parent pointers, quotas, realtime, external log, metadir and zoned
// XFS remain read-only (or are refused for writing).  The four trailing
// booleans are legacy overrides retained for source compatibility; they are
// accepted as no-ops.
Status CheckWritableProfile(const XfsSuperblock& sb, const XfsGeometry& geo,
                            bool allow_rmapbt_recovery = false,
                            bool allow_rmapbt_qualification = false,
                            bool allow_reflink_recovery = false,
                            bool allow_reflink_qualification = false);

// Human readable one-line profile description for logs/diagnostics.
std::string WritableProfileSummary(const XfsSuperblock& sb);

// ---------------------------------------------------------------------------
// Range mapping for in-place writes
// ---------------------------------------------------------------------------
// One contiguous run of file bytes that lives in contiguous filesystem blocks.
// The corresponding device byte offset is
//   geo.FsbToByte(fs_block) + (file_offset & (block_size - 1))
struct WriteMapping {
  uint64_t file_offset = 0;  // first file byte of the run
  uint64_t fs_block = 0;     // filesystem block number (fsb) backing it
  uint64_t byte_count = 0;
};

// Verifies that [offset, offset + length) lies entirely inside *written* (not
// unwritten, not hole) extents of `inode` and returns the device ranges to
// write, in file order. This is the RW-0 restriction: a write may only
// overwrite blocks that are already allocated and initialized, so it never
// has to allocate a block, convert an unwritten extent or change the inode
// size. Fails with:
//   kWriteExtendsFile      the range would move EOF
//   kWriteNeedsAllocation  the range covers a hole
//   kWriteNeedsConversion  the range covers an unwritten extent
//   kOutOfRange / kInvalidArgument for nonsense ranges
Status MapWrittenRange(const XfsGeometry& geo, const XfsInode& inode,
                       const std::vector<XfsExtent>& extents, uint64_t offset,
                       uint64_t length, std::vector<WriteMapping>* out);

}  // namespace xfs
