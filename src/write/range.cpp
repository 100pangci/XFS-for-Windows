// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - in-place write range mapping.
//
// A write may only overwrite blocks that are already allocated and written,
// so before anything is written to the device the whole range is mapped
// against the file's extent list and every byte must be covered by a written
// extent. Anything else (a hole, an unwritten/preallocated extent, EOF) is
// refused instead of being "made to work": allocating a block, converting an
// unwritten extent and extending the inode are all metadata transactions,
// which this layer cannot perform yet.
#include "xfs/write.h"

#include <algorithm>

#include "xfs/ondisk.h"

namespace xfs {
namespace {

namespace di = ondisk::inode;

// Largest extent whose startoff is <= file_block.
size_t FindExtent(const std::vector<XfsExtent>& extents, uint64_t file_block) {
  size_t lo = 0, hi = extents.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (extents[mid].startoff <= file_block) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

}  // namespace

Status MapWrittenRange(const XfsGeometry& geo, const XfsInode& inode,
                       const std::vector<XfsExtent>& extents, uint64_t offset,
                       uint64_t length, std::vector<WriteMapping>* out) {
  out->clear();
  if (length == 0) return OkStatus();
  if (geo.block_size == 0 || geo.block_log == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "geometry not initialized")
        .Metadata("write");
  }
  if (inode.IsDir()) {
    return XFS_ERR(XfsError::kNotRegularFile,
                   "cannot write directory contents")
        .Ino(inode.ino);
  }
  if (!inode.IsRegular()) {
    return XFS_ERR(XfsError::kNotRegularFile,
                   "only regular files can be written")
        .Ino(inode.ino);
  }
  if (inode.format != di::kFormatExtents && inode.format != di::kFormatBtree) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "inode has no writable data fork")
        .Ino(inode.ino)
        .Actual(inode.format);
  }
  if ((inode.flags & di::kDiFlagImmutable) != 0) {
    return XFS_ERR(XfsError::kPermissionDenied, "inode is immutable")
        .Ino(inode.ino);
  }
  if ((inode.flags & di::kDiFlagAppend) != 0) {
    return XFS_ERR(XfsError::kPermissionDenied, "inode is append-only")
        .Ino(inode.ino);
  }
  if (offset > inode.size || length > inode.size - offset) {
    return XFS_ERR(XfsError::kWriteExtendsFile,
                   "write would move EOF (in-place writes only; growing a "
                   "file needs an allocation transaction)")
        .Ino(inode.ino)
        .Expected(inode.size)
        .Actual(offset + length);
  }
  if (length > (1ull << 40)) {
    return XFS_ERR(XfsError::kInvalidArgument, "write length out of range")
        .Ino(inode.ino)
        .Actual(length);
  }

  const uint64_t block_size = geo.block_size;
  uint64_t pos = offset;
  uint64_t remaining = length;
  while (remaining > 0) {
    const uint64_t file_block = pos >> geo.block_log;
    const size_t index = FindExtent(extents, file_block);
    const XfsExtent* extent = index ? &extents[index - 1] : nullptr;
    if (!extent || file_block >= extent->EndBlock()) {
      return XFS_ERR(XfsError::kWriteNeedsAllocation,
                     "write range covers a hole; filling a hole needs an "
                     "allocation transaction")
          .Ino(inode.ino)
          .Offset(pos)
          .Actual(file_block);
    }
    if (extent->unwritten) {
      return XFS_ERR(XfsError::kWriteNeedsConversion,
                     "write range covers an unwritten (preallocated) extent; "
                     "converting it needs a metadata transaction")
          .Ino(inode.ino)
          .Offset(pos)
          .Actual(file_block);
    }
    // Bytes from `pos` to the end of this extent (bounded by what is left).
    const uint64_t extent_end_byte = extent->EndBlock() * block_size;
    const uint64_t run = std::min<uint64_t>(remaining, extent_end_byte - pos);
    const uint64_t fsb = extent->startblock + (file_block - extent->startoff);
    // Merge adjacent extents when both the file range and the physical range
    // are contiguous. A conversion or allocation can leave several physically
    // adjacent bmbt records; those are still one positioned write.
    if (!out->empty()) {
      WriteMapping& last = out->back();
      const uint64_t last_file_end = last.file_offset + last.byte_count;
      const uint64_t last_device_end =
          last.fs_block * block_size +
          (last.file_offset & (block_size - 1)) + last.byte_count;
      const uint64_t this_device_start =
          fsb * block_size + (pos & (block_size - 1));
      if (last_file_end == pos && last_device_end == this_device_start) {
        last.byte_count += run;
        pos += run;
        remaining -= run;
        continue;
      }
    }
    WriteMapping mapping;
    mapping.file_offset = pos;
    mapping.fs_block = fsb;
    mapping.byte_count = run;
    out->push_back(mapping);
    pos += run;
    remaining -= run;
  }
  return OkStatus();
}

}  // namespace xfs
