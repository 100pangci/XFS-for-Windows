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
// Checks that the filesystem uses the exact feature profile this build can
// modify. Anything else must be mounted read-only: a writer that only
// *understands* a feature for reading (finobt, rmapbt, reflink, sparse inodes,
// bigtime, NREXT64, parent pointers, quotas, realtime, external log, metadir,
// zoned) cannot update it consistently, and a best-effort update would corrupt
// the filesystem.
Status CheckWritableProfile(const XfsSuperblock& sb, const XfsGeometry& geo);

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
