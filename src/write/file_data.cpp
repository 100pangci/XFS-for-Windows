// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - in-place file data writes.
//
// RW-0 scope: overwrite bytes that already live in allocated, written extents.
// The range is mapped and validated *before* anything is written (see
// MapWrittenRange), the device write happens through the block device, and the
// affected cache entries are dropped through the central invalidation path.
//
// No metadata is touched, so this write needs no transaction and no journal
// record: XFS never journals file data. That is also what keeps the log clean,
// which is why Linux needs no replay after this kind of write.
#include "xfs/filesystem.h"

#include "xfs/crash.h"
#include "xfs/log.h"
#include "xfs/write.h"

namespace xfs {
namespace {

std::string RangeText(uint64_t ino, uint64_t offset, uint64_t length) {
  return "ino=" + std::to_string(ino) + " offset=" + std::to_string(offset) +
         " length=" + std::to_string(length);
}

}  // namespace

Result<size_t> XfsFilesystem::OverwriteFileData(uint64_t ino, uint64_t offset,
                                               const void* buffer,
                                               size_t length) {
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume,
                   "filesystem is mounted read-only")
        .Ino(ino)
        .Metadata("write");
  }
  if (length == 0) return size_t{0};
  if (buffer == nullptr) {
    return XFS_ERR(XfsError::kInvalidArgument, "null write buffer").Ino(ino);
  }

  // One mutation at a time. This also orders the device write against the
  // cache publication for concurrent readers.
  std::lock_guard<std::mutex> lock(mutation_mutex_);

  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  auto extents = LoadExtentsCached(inode.value());
  if (!extents.ok()) return extents.error();

  std::vector<WriteMapping> mappings;
  Status s = MapWrittenRange(geometry_, inode.value(), *extents.value(), offset,
                             length, &mappings);
  if (!s.ok()) return s.error();

  MaybeCrash(CrashPoint::kBeforeDataWrite, RangeText(ino, offset, length));

  const auto* src = static_cast<const uint8_t*>(buffer);
  size_t written = 0;
  for (size_t i = 0; i < mappings.size(); ++i) {
    const WriteMapping& mapping = mappings[i];
    if (i > 0) {
      MaybeCrash(CrashPoint::kMidDataWrite, RangeText(ino, offset, length));
    }
    const uint64_t device_offset =
        geometry_.FsbToByte(mapping.fs_block) +
        (mapping.file_offset & (geometry_.block_size - 1));
    if (!device_->Write(device_offset, src + (mapping.file_offset - offset),
                        static_cast<size_t>(mapping.byte_count))) {
      return XFS_ERR(XfsError::kIoError,
                     "file data write failed: " + device_->LastError())
          .Ino(ino)
          .Offset(device_offset)
          .Metadata("device");
    }
    written += static_cast<size_t>(mapping.byte_count);
  }

  MaybeCrash(CrashPoint::kAfterDataWrite, RangeText(ino, offset, length));

  // Order matters (see InsertDataChunk): the device holds the new bytes now, so
  // the generation bump makes every chunk read before this point unusable.
  InvalidateCaches(CacheInvalidation::DataRange(ino, offset, length));
  XFS_EXT_DEBUG("wrote " << written << " bytes at " << offset << " in inode "
                         << ino << " (" << mappings.size() << " extents)");
  return written;
}

}  // namespace xfs
