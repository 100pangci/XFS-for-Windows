// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - file data writes, EOF extension and truncate (RW-2).
//
// Ordering (the same contract Linux XFS provides):
//
//   write:  1. allocate the missing blocks as *unwritten* extents (transaction)
//           2. write the data, flush
//           3. convert the written range to written extents, set the size
//              (transaction)
//   truncate: 1. zero the tail of a partially kept last block, flush
//             2. remove the extents beyond the new EOF, free the blocks, set
//                the size (transaction)
//
// A crash therefore leaves either the old state or the new one; an unfinished
// write has an unwritten extent, which reads as zeros, so uninitialised disk
// contents are never exposed. Every mutation of the extent map, the allocator
// and the inode goes through one transaction (log record, then metadata),
// which is what makes recovery replay the whole change atomically.
#include <algorithm>
#include <cstring>
#include <vector>

#include "xfs/alloc.h"
#include "xfs/bmap.h"
#include "xfs/crash.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/write.h"

namespace xfs {
namespace {

namespace di = ondisk::inode;

constexpr size_t kMaxWriteBytes = 1ull << 30;  // one call, sanity bound

// Stores a timestamp in the legacy 32:32 big-endian encoding.
void StoreTimestamp(uint8_t* field, const XfsTimestamp& ts) {
  StoreBE32(field, static_cast<uint32_t>(ts.sec));
  StoreBE32(field + 4, ts.nsec);
}

// The blocks a write touches.
struct WritePlan {
  uint64_t first_block = 0;
  uint64_t last_block = 0;  // inclusive
  uint64_t new_size = 0;
};

bool FindExtentForBlock(const std::vector<XfsExtent>& extents,
                        uint64_t file_block, const XfsExtent** out) {
  size_t lo = 0, hi = extents.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (extents[mid].startoff <= file_block) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return false;
  const XfsExtent& e = extents[lo - 1];
  if (file_block >= e.EndBlock()) return false;
  *out = &e;
  return true;
}

bool IsWrittenExtentForBlock(const std::vector<XfsExtent>& extents,
                             uint64_t file_block, const XfsExtent** out) {
  const XfsExtent* e = nullptr;
  if (!FindExtentForBlock(extents, file_block, &e)) return false;
  if (e->unwritten) return false;
  *out = e;
  return true;
}

}  // namespace

Result<size_t> XfsFilesystem::WriteFileData(uint64_t ino, uint64_t offset,
                                            const void* buffer, size_t length,
                                            bool append) {
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
  if (length > kMaxWriteBytes) {
    return XFS_ERR(XfsError::kOutOfRange, "write range out of range")
        .Ino(ino)
        .Actual(length)
        .Metadata("write");
  }

  std::lock_guard<std::mutex> lock(mutation_mutex_);

  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  if (inode.value().IsDir()) {
    return XFS_ERR(XfsError::kNotRegularFile,
                   "cannot write directory contents")
        .Ino(ino);
  }
  if (!inode.value().IsRegular()) {
    return XFS_ERR(XfsError::kNotRegularFile,
                   "only regular files can be written")
        .Ino(ino);
  }
  if (inode.value().format != di::kFormatExtents &&
      inode.value().format != di::kFormatBtree) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "inode has no writable data fork")
        .Ino(ino)
        .Actual(inode.value().format);
  }
  if ((inode.value().flags & di::kDiFlagImmutable) != 0) {
    return XFS_ERR(XfsError::kPermissionDenied, "inode is immutable").Ino(ino);
  }
  if ((inode.value().flags & di::kDiFlagAppend) != 0) {
    return XFS_ERR(XfsError::kPermissionDenied, "inode is append-only")
        .Ino(ino);
  }

  // Append: resolve the offset now that the inode is loaded *under the
  // mutation lock*, so two concurrent appends cannot both compute the same
  // EOF (one would overwrite the other's bytes).
  if (append) offset = inode.value().size;
  if (offset > (1ull << 54) || length > (1ull << 54) - offset) {
    return XFS_ERR(XfsError::kOutOfRange, "write range out of range")
        .Ino(ino)
        .Actual(length)
        .Metadata("write");
  }

  auto extents = LoadExtentsCached(inode.value());
  if (!extents.ok()) return extents.error();

  const uint64_t old_size = inode.value().size;
  const uint64_t end = offset + length;
  const uint64_t block_size = geometry_.block_size;
  const WritePlan plan{
      offset >> geometry_.block_log,
      (end - 1) >> geometry_.block_log,
      std::max(old_size, end),
  };

  // Fast path: the whole range is already allocated and written and EOF does
  // not move - the RW-0/1 path (direct writes + a timestamp transaction).
  if (end <= old_size) {
    std::vector<WriteMapping> mappings;
    Status mapped = MapWrittenRange(geometry_, inode.value(), *extents.value(),
                                    offset, length, &mappings);
    if (mapped.ok()) {
      MaybeCrash(CrashPoint::kBeforeDataWrite, "write-path fast");
      const auto* src = static_cast<const uint8_t*>(buffer);
      for (size_t i = 0; i < mappings.size(); ++i) {
        const WriteMapping& mapping = mappings[i];
        if (i > 0) MaybeCrash(CrashPoint::kMidDataWrite, "write-path fast");
        const uint64_t device_offset =
            geometry_.FsbToByte(mapping.fs_block) +
            (mapping.file_offset & (block_size - 1));
        if (!device_->Write(device_offset, src + (mapping.file_offset - offset),
                            static_cast<size_t>(mapping.byte_count))) {
          return XFS_ERR(XfsError::kIoError,
                         "file data write failed: " + device_->LastError())
              .Ino(ino)
              .Offset(device_offset)
              .Metadata("device");
        }
      }
      MaybeCrash(CrashPoint::kAfterDataWrite, "write-path fast");
      InvalidateCaches(CacheInvalidation::DataRange(ino, offset, length));
      const XfsTimestamp now = CurrentTimestamp();
      auto txn = BeginTransaction();
      if (!txn.ok()) return txn.error();
      Status status = UpdateTimesInTransaction(
          txn.value(), ino, inode.value().atime, now, now);
      if (status.ok()) status = CommitTransaction(txn.value());
      if (!status.ok()) return status.error();
      return length;
    }
    if (mapped.error().code != XfsError::kWriteNeedsAllocation &&
        mapped.error().code != XfsError::kWriteNeedsConversion) {
      return mapped.error();
    }
  }

  // ---- general path ----
  // Bytes the caller asked for are written verbatim; every other byte that
  // becomes visible is either preserved old *written* content (below the old
  // EOF) or zero. Freshly allocated blocks therefore never leak disk contents.

  const auto* src = static_cast<const uint8_t*>(buffer);

  // 1. Allocate the holes as unwritten extents, in one transaction.
  {
    auto txn = BeginTransaction();
    if (!txn.ok()) return txn.error();
    BlockAllocator alloc(&txn.value(), device_.get(), &geometry_);
    DataForkEditor editor(&txn.value(), &alloc, device_.get(), &geometry_);
    Status s = editor.Load(inode.value());
    if (!s.ok()) return s.error();

    const uint32_t preferred = geometry_.InoAgno(ino);
    uint64_t block = plan.first_block;
    while (block <= plan.last_block) {
      const XfsExtent* cover = nullptr;
      // Skip blocks already backed by an extent (written or unwritten).
      if (IsWrittenExtentForBlock(*extents.value(), block, &cover)) {
        ++block;
        continue;
      }
      bool covered_by_unwritten = false;
      for (const XfsExtent& e : editor.extents()) {
        if (e.startoff <= block && block < e.EndBlock()) {
          covered_by_unwritten = true;
          break;
        }
      }
      if (covered_by_unwritten) {
        ++block;
        continue;
      }
      // Maximal hole run.
      uint64_t run_end = block;
      while (run_end <= plan.last_block) {
        const XfsExtent* written = nullptr;
        if (IsWrittenExtentForBlock(*extents.value(), run_end, &written)) break;
        bool unwritten_here = false;
        for (const XfsExtent& e : editor.extents()) {
          if (e.startoff <= run_end && run_end < e.EndBlock()) {
            unwritten_here = true;
            break;
          }
        }
        if (unwritten_here) break;
        ++run_end;
      }
      uint64_t remaining = run_end - block;
      uint64_t cursor = block;
      while (remaining > 0) {
        // Ask for the whole run; halve on ENOSPC to keep runs as long as
        // possible (one AG, contiguous).
        uint64_t want = remaining;
        Allocation allocation;
        Status allocated;
        while (true) {
          allocated = alloc.Allocate(preferred, static_cast<uint32_t>(want),
                                     &allocation);
          if (allocated.ok()) break;
          if (allocated.error().code != XfsError::kNoSpace || want == 1) {
            return allocated.error();
          }
          want = std::max<uint64_t>(1, want / 2);
        }
        XfsExtent extent;
        extent.startoff = cursor;
        extent.startblock = allocation.fsb;
        extent.blockcount = allocation.len;
        extent.unwritten = true;
        s = editor.InsertExtent(extent);
        if (!s.ok()) return s.error();
        editor.AddNblocks(static_cast<int64_t>(allocation.len));
        cursor += allocation.len;
        remaining -= allocation.len;
      }
      block = run_end;
    }

    // Write the new fork into the inode (size unchanged in this transaction).
    auto raw = ReadRawInode(ino);
    if (!raw.ok()) return raw.error();
    std::vector<uint8_t> raw_inode = std::move(raw.value());
    XFS_DEBUG("write txn1: nblocks before "
              << GetBE64(raw_inode.data(), di::kNblocks) << " delta "
              << editor.nblocks_delta());
    uint32_t ilog_fields = 0;
    std::vector<uint8_t> fork_region;
    s = editor.Finalize(&raw_inode, &ilog_fields, &fork_region);
    if (!s.ok()) return s.error();
    const XfsTimestamp now = CurrentTimestamp();
    StoreTimestamp(raw_inode.data() + di::kMtime, now);
    StoreTimestamp(raw_inode.data() + di::kCtime, now);
    s = LogPreparedInode(txn.value(), ino, std::move(raw_inode), ilog_fields,
                         fork_region);
    if (!s.ok()) return s.error();
    s = alloc.Commit();
    if (!s.ok()) return s.error();
    s = CommitTransaction(txn.value());
    if (!s.ok()) return s.error();
  }

  // 2. The data itself, block by block, then a flush so the bytes are durable
  //    before the conversion transaction can make them visible.
  {
    std::vector<uint8_t> block_buffer(static_cast<size_t>(block_size));
    auto fresh_inode = OpenInode(ino);
    if (!fresh_inode.ok()) return fresh_inode.error();
    auto fresh_extents = LoadExtentsCached(fresh_inode.value());
    if (!fresh_extents.ok()) return fresh_extents.error();

    MaybeCrash(CrashPoint::kBeforeDataWrite, "write-path allocate");
    for (uint64_t b = plan.first_block; b <= plan.last_block; ++b) {
      const uint64_t block_start = b << geometry_.block_log;
      const uint64_t user_start = std::max(offset, block_start);
      const uint64_t user_end = std::min(end, block_start + block_size);
      const bool user_covers_block =
          user_start == block_start && user_end == block_start + block_size;
      std::memset(block_buffer.data(), 0, block_buffer.size());
      if (user_covers_block) {
        std::memcpy(block_buffer.data(), src + (block_start - offset),
                    static_cast<size_t>(block_size));
      } else {
        // Preserve what the file already shows in this block (zeros for holes
        // and unwritten extents, the device bytes for written extents, and
        // nothing past the old EOF).
        if (block_start < old_size) {
          const uint64_t want =
              std::min<uint64_t>(block_size, old_size - block_start);
          auto read = ReadFileData(*device_, geometry_, fresh_inode.value(),
                                   block_start, block_buffer.data(),
                                   static_cast<size_t>(want),
                                   fresh_extents.value().get());
          if (!read.ok()) return read.error();
        }
        if (user_end > user_start) {
          std::memcpy(block_buffer.data() + (user_start - block_start),
                      src + (user_start - offset),
                      static_cast<size_t>(user_end - user_start));
        }
      }
      if (b == plan.first_block + 1) {
        MaybeCrash(CrashPoint::kMidDataWrite, "write-path general");
      }
      const XfsExtent* extent = nullptr;
      if (!FindExtentForBlock(*fresh_extents.value(), b, &extent)) {
        return XFS_ERR(XfsError::kInternal,
                       "allocated block is not covered by an extent")
            .Ino(ino)
            .Actual(b)
            .Metadata("write");
      }
      const uint64_t fsb = extent->startblock + (b - extent->startoff);
      const uint64_t device_offset = geometry_.FsbToByte(fsb);
      if (!device_->Write(device_offset, block_buffer.data(),
                          block_buffer.size())) {
        return XFS_ERR(XfsError::kIoError,
                       "file data write failed: " + device_->LastError())
            .Ino(ino)
            .Offset(device_offset)
            .Metadata("device");
      }
    }
    MaybeCrash(CrashPoint::kAfterDataWrite, "write-path general");
    if (!device_->Flush()) {
      return XFS_ERR(XfsError::kIoError,
                     "failed to flush file data: " + device_->LastError())
          .Ino(ino)
          .Metadata("device");
    }
    InvalidateCaches(CacheInvalidation::DataRange(ino, offset, length));
  }

  // 3. Convert the written range to written extents and set the new size.
  {
    auto txn = BeginTransaction();
    if (!txn.ok()) return txn.error();
    BlockAllocator alloc(&txn.value(), device_.get(), &geometry_);
    DataForkEditor editor(&txn.value(), &alloc, device_.get(), &geometry_);
    auto fresh_inode = OpenInode(ino);
    if (!fresh_inode.ok()) return fresh_inode.error();
    Status s = editor.Load(fresh_inode.value());
    if (!s.ok()) return s.error();
    std::vector<XfsExtent> current = editor.extents();
    for (const XfsExtent& e : current) {
      if (!e.unwritten) continue;
      const uint64_t first = std::max<uint64_t>(e.startoff, plan.first_block);
      const uint64_t last = std::min<uint64_t>(e.EndBlock(),
                                               plan.last_block + 1);
      if (first >= last) continue;
      const uint64_t fsb = e.startblock + (first - e.startoff);
      s = editor.RemoveRange(first, last);
      if (!s.ok()) return s.error();
      XfsExtent converted;
      converted.startoff = first;
      converted.startblock = fsb;
      converted.blockcount = last - first;
      converted.unwritten = false;
      s = editor.InsertExtent(converted);
      if (!s.ok()) return s.error();
    }
    auto raw = ReadRawInode(ino);
    if (!raw.ok()) return raw.error();
    std::vector<uint8_t> raw_inode = std::move(raw.value());
    uint32_t ilog_fields = 0;
    std::vector<uint8_t> fork_region;
    s = editor.Finalize(&raw_inode, &ilog_fields, &fork_region);
    if (!s.ok()) return s.error();
    const XfsTimestamp now = CurrentTimestamp();
    PutBE64(raw_inode.data(), di::kSize, plan.new_size);
    StoreTimestamp(raw_inode.data() + di::kMtime, now);
    StoreTimestamp(raw_inode.data() + di::kCtime, now);
    s = LogPreparedInode(txn.value(), ino, std::move(raw_inode), ilog_fields,
                         fork_region);
    if (!s.ok()) return s.error();
    s = alloc.Commit();
    if (!s.ok()) return s.error();
    s = CommitTransaction(txn.value());
    if (!s.ok()) return s.error();
  }

  InvalidateCaches(CacheInvalidation::Extents(ino));
  XFS_EXT_DEBUG("wrote " << length << " bytes at " << offset << " in inode "
                         << ino << " (size " << old_size << " -> "
                         << plan.new_size << ")");
  return length;
}

Result<XfsFileInfo> XfsFilesystem::SetFileSize(uint64_t ino, uint64_t new_size) {
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume,
                   "filesystem is mounted read-only")
        .Ino(ino)
        .Metadata("truncate");
  }
  if (new_size > (1ull << 54)) {
    return XFS_ERR(XfsError::kOutOfRange, "file size out of range")
        .Ino(ino)
        .Actual(new_size)
        .Metadata("truncate");
  }
  std::lock_guard<std::mutex> lock(mutation_mutex_);
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  if (!inode.value().IsRegular()) {
    return XFS_ERR(XfsError::kNotRegularFile,
                   "only regular files can be resized")
        .Ino(ino);
  }
  if (inode.value().format != di::kFormatExtents &&
      inode.value().format != di::kFormatBtree) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "inode has no resizable data fork")
        .Ino(ino)
        .Actual(inode.value().format);
  }
  if ((inode.value().flags & di::kDiFlagImmutable) != 0) {
    return XFS_ERR(XfsError::kPermissionDenied, "inode is immutable").Ino(ino);
  }

  const uint64_t old_size = inode.value().size;
  if (new_size == old_size) return GetFileInfo(ino);

  if (new_size > old_size) {
    // Growing only moves the logical EOF. Bytes that an existing *written*
    // extent would expose must be zeroed first (Linux does the same through
    // xfs_zero_range), otherwise a later read could see stale file data.
    auto extents = LoadExtentsCached(inode.value());
    if (!extents.ok()) return extents.error();
    std::vector<WriteMapping> zero_ranges;
    for (const XfsExtent& e : *extents.value()) {
      if (e.unwritten) continue;
      const uint64_t start_byte = e.startoff << geometry_.block_log;
      const uint64_t end_byte = e.EndBlock() << geometry_.block_log;
      const uint64_t z0 = std::max(start_byte, old_size);
      const uint64_t z1 = std::min(end_byte, new_size);
      if (z0 >= z1) continue;
      WriteMapping mapping;
      mapping.file_offset = z0;
      mapping.fs_block =
          e.startblock + ((z0 - start_byte) >> geometry_.block_log);
      mapping.byte_count = z1 - z0;
      zero_ranges.push_back(mapping);
    }
    if (!zero_ranges.empty()) {
      std::vector<uint8_t> zeros(static_cast<size_t>(geometry_.block_size), 0);
      for (const WriteMapping& mapping : zero_ranges) {
        uint64_t remaining = mapping.byte_count;
        uint64_t device_offset =
            geometry_.FsbToByte(mapping.fs_block) +
            (mapping.file_offset & (geometry_.block_size - 1));
        while (remaining > 0) {
          const size_t chunk = static_cast<size_t>(
              std::min<uint64_t>(remaining, zeros.size()));
          if (!device_->Write(device_offset, zeros.data(), chunk)) {
            return XFS_ERR(XfsError::kIoError,
                           "failed to zero extended file data: " +
                               device_->LastError())
                .Ino(ino)
                .Offset(device_offset)
                .Metadata("device");
          }
          device_offset += chunk;
          remaining -= chunk;
        }
      }
      if (!device_->Flush()) {
        return XFS_ERR(XfsError::kIoError,
                       "failed to flush zeroed file data: " +
                           device_->LastError())
            .Ino(ino)
            .Metadata("device");
      }
      InvalidateCaches(CacheInvalidation::DataRange(
          ino, old_size, new_size - old_size));
    }
    auto txn = BeginTransaction();
    if (!txn.ok()) return txn.error();
    const XfsTimestamp now = CurrentTimestamp();
    Status status = UpdateInodeInTransaction(
        txn.value(), ino, 0, {},
        [&](uint8_t* raw) {
          PutBE64(raw, di::kSize, new_size);
          StoreTimestamp(raw + di::kMtime, now);
          StoreTimestamp(raw + di::kCtime, now);
        });
    if (status.ok()) status = CommitTransaction(txn.value());
    if (!status.ok()) return status.error();
    InvalidateCaches(CacheInvalidation::Inode(ino));
    return GetFileInfo(ino);
  }

  // Shrinking. The last partially kept block keeps its contents below
  // new_size; the part beyond it is zeroed (xfs_truncate_page) so a later
  // extension cannot expose it. Blocks from ceil(new_size / bs) on are freed.
  const uint64_t keep_blocks = (new_size + geometry_.block_size - 1) >>
                               geometry_.block_log;
  if (new_size % geometry_.block_size != 0 && keep_blocks > 0) {
    auto extents = LoadExtentsCached(inode.value());
    if (!extents.ok()) return extents.error();
    const uint64_t last_block = keep_blocks - 1;
    const XfsExtent* extent = nullptr;
    if (IsWrittenExtentForBlock(*extents.value(), last_block, &extent)) {
      const uint64_t start_byte =
          (extent->startoff + (last_block - extent->startoff))
          << geometry_.block_log;
      const uint64_t zero_from = new_size;
      const uint64_t zero_to = start_byte + geometry_.block_size;
      std::vector<uint8_t> zeros(static_cast<size_t>(geometry_.block_size), 0);
      uint64_t pos = zero_from;
      const uint64_t device_base =
          geometry_.FsbToByte(extent->startblock) +
          ((last_block - extent->startoff) << geometry_.block_log);
      while (pos < zero_to) {
        const size_t chunk =
            static_cast<size_t>(std::min<uint64_t>(zero_to - pos, zeros.size()));
        if (!device_->Write(device_base + (pos - start_byte), zeros.data(),
                            chunk)) {
          return XFS_ERR(XfsError::kIoError,
                         "failed to zero the truncated file tail: " +
                             device_->LastError())
              .Ino(ino)
              .Offset(device_base + (pos - start_byte))
              .Metadata("device");
        }
        pos += chunk;
      }
      if (!device_->Flush()) {
        return XFS_ERR(XfsError::kIoError,
                       "failed to flush the zeroed file tail: " +
                           device_->LastError())
            .Ino(ino)
            .Metadata("device");
      }
      InvalidateCaches(
          CacheInvalidation::DataRange(ino, zero_from, zero_to - zero_from));
    }
  }

  // Remove the extents beyond the new EOF, in bounded transactions (like
  // xfs_itruncate_extents, which rolls its transaction): a crash in the middle
  // leaves a smaller extent map with the old size, a legal XFS state.
  constexpr size_t kExtentsPerTransaction = 48;
  bool first = true;
  while (true) {
    auto fresh = OpenInode(ino);
    if (!fresh.ok()) return fresh.error();
    Status s = OkStatus();
    bool more = false;
    {
      auto txn = BeginTransaction();
      if (!txn.ok()) return txn.error();
      BlockAllocator alloc(&txn.value(), device_.get(), &geometry_);
      DataForkEditor editor(&txn.value(), &alloc, device_.get(), &geometry_);
      s = editor.Load(fresh.value());
      if (!s.ok()) return s.error();
      editor.SetFreeRemovedBlocks(true);
      std::vector<XfsExtent> current = editor.extents();
      size_t removed = 0;
      for (const XfsExtent& e : current) {
        if (e.EndBlock() <= keep_blocks) continue;
        if (removed >= kExtentsPerTransaction) {
          more = true;
          break;
        }
        const uint64_t from = std::max<uint64_t>(e.startoff, keep_blocks);
        s = editor.RemoveRange(from, e.EndBlock());
        if (!s.ok()) return s.error();
        ++removed;
      }
      auto raw = ReadRawInode(ino);
      if (!raw.ok()) return raw.error();
      std::vector<uint8_t> raw_inode = std::move(raw.value());
      uint32_t ilog_fields = 0;
      std::vector<uint8_t> fork_region;
      s = editor.Finalize(&raw_inode, &ilog_fields, &fork_region);
      if (!s.ok()) return s.error();
      XFS_DEBUG("truncate: nblocks "
                << GetBE64(raw_inode.data(), di::kNblocks) << " after "
                << "delta " << editor.nblocks_delta() << " extents "
                << editor.nextents());
      const XfsTimestamp now = CurrentTimestamp();
      if (!more) {
        PutBE64(raw_inode.data(), di::kSize, new_size);
      }
      StoreTimestamp(raw_inode.data() + di::kMtime, now);
      StoreTimestamp(raw_inode.data() + di::kCtime, now);
      s = LogPreparedInode(txn.value(), ino, std::move(raw_inode), ilog_fields,
                           fork_region);
      if (!s.ok()) return s.error();
      s = alloc.Commit();
      if (!s.ok()) return s.error();
      s = CommitTransaction(txn.value());
      if (!s.ok()) return s.error();
    }
    InvalidateCaches(CacheInvalidation::Extents(ino));
    if (!more) break;
    if (first) {
      XFS_DEBUG("truncate is running in multiple transactions");
      first = false;
    }
  }
  InvalidateCaches(CacheInvalidation::Extents(ino));
  return GetFileInfo(ino);
}

}  // namespace xfs
