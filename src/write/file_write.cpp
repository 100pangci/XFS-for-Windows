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
#include <map>
#include <vector>

#include "xfs/alloc.h"
#include "xfs/bmap.h"
#include "xfs/crash.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/refcount.h"
#include "xfs/write.h"

namespace xfs {
namespace {

namespace di = ondisk::inode;

constexpr size_t kMaxWriteBytes = 1ull << 30;  // one call, sanity bound

Status StoreModifiedTimes(uint8_t* raw, const XfsTimestamp& now) {
  Status s = StoreInodeTimestamp(raw, di::kMtime, now);
  if (!s.ok()) return s;
  return StoreInodeTimestamp(raw, di::kCtime, now);
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

// True when any written file-data extent in [first_block, last_block] has a
// shared refcountbt record.  Reads the typed trees directly (the caller has no
// open transaction yet); an unreadable tree is an error, never "not shared".
Result<bool> RangeHasSharedData(BlockDevice& dev, const XfsGeometry& geo,
                                const std::vector<XfsExtent>& extents,
                                uint64_t first_block, uint64_t last_block) {
  if (!geo.features.reflink || last_block < first_block) return false;
  std::map<uint32_t, RefcountTree> trees;
  for (const XfsExtent& e : extents) {
    if (e.unwritten) continue;
    if (e.EndBlock() <= first_block || e.startoff > last_block) continue;
    const uint64_t b0 = std::max(e.startoff, first_block);
    const uint64_t b1 = std::min(e.EndBlock(), last_block + 1);
    if (b0 >= b1) continue;
    const uint64_t fsb = e.startblock + (b0 - e.startoff);
    if (!geo.FsbRangeValid(fsb, b1 - b0)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "file extent leaves one AG while checking sharing")
          .Actual(fsb)
          .Metadata("refcountbt");
    }
    const uint32_t agno = static_cast<uint32_t>(fsb >> geo.agblklog);
    const uint32_t agbno =
        static_cast<uint32_t>(fsb & ((1ULL << geo.agblklog) - 1));
    auto it = trees.find(agno);
    if (it == trees.end()) {
      auto tree = LoadRefcountTree(dev, geo, agno, 0);
      if (!tree.ok()) return tree.error();
      it = trees.emplace(agno, std::move(tree.value())).first;
    }
    if (it->second.OverlapsShared(agbno, static_cast<uint32_t>(b1 - b0))) {
      return true;
    }
  }
  return false;
}

}  // namespace

Status XfsFilesystem::CleanupCowStagingLocked(
    const std::vector<CowRun>& runs) {
  if (runs.empty()) return OkStatus();
  auto txn = BeginTransaction();
  if (!txn.ok()) return txn.error();
  BlockAllocator alloc(&txn.value(), device_.get(), &geometry_);
  for (const CowRun& run : runs) {
    Status s = alloc.ReleaseCowStaging(run.fsb, run.len);
    if (!s.ok()) return s;
  }
  Status s = alloc.Commit();
  if (!s.ok()) return s;
  return CommitTransaction(txn.value());
}

Result<size_t> XfsFilesystem::CowReplaceRangeLocked(
    uint64_t ino, const XfsInode& inode,
    const std::vector<XfsExtent>& extents, uint64_t offset, uint64_t length,
    const void* buffer, uint64_t new_size) {
  if (length == 0) return size_t{0};
  if (offset > (1ull << 54) || length > (1ull << 54) - offset) {
    return XFS_ERR(XfsError::kOutOfRange, "COW write range out of range")
        .Ino(ino)
        .Actual(length)
        .Metadata("reflink");
  }
  const uint64_t block_size = geometry_.block_size;
  const uint64_t first_block = offset >> geometry_.block_log;
  const uint64_t last_block = (offset + length - 1) >> geometry_.block_log;
  const uint64_t block_count = last_block - first_block + 1;
  const uint64_t old_size = inode.size;

  std::vector<CowRun> runs;
  {
    auto txn = BeginTransaction();
    if (!txn.ok()) return txn.error();
    BlockAllocator alloc(&txn.value(), device_.get(), &geometry_);
    const uint32_t preferred = geometry_.InoAgno(ino);
    uint64_t cursor = first_block;
    uint64_t remaining = block_count;
    while (remaining > 0) {
      uint64_t want = remaining;
      Allocation allocation;
      Status allocated = XFS_ERR(XfsError::kInternal, "unset allocation");
      while (true) {
        allocated = alloc.AllocateCowStaging(
            preferred, static_cast<uint32_t>(want), &allocation);
        if (allocated.ok()) break;
        if (allocated.error().code != XfsError::kNoSpace || want == 1) {
          return allocated.error();
        }
        want = std::max<uint64_t>(1, want / 2);
      }
      runs.push_back(CowRun{cursor, allocation.fsb, allocation.len});
      cursor += allocation.len;
      remaining -= allocation.len;
    }
    Status s = alloc.Commit();
    if (!s.ok()) return s.error();
    MaybeCrash(CrashPoint::kCowStaged, "cow staging");
    s = CommitTransaction(txn.value());
    if (!s.ok()) return s.error();
  }

  auto cleanup = [&]() {
    Status cleaned = CleanupCowStagingLocked(runs);
    if (!cleaned.ok()) {
      XFS_INFO("COW staging cleanup deferred to mount recovery: "
               << cleaned.error().Format());
    }
  };

  const auto* src = static_cast<const uint8_t*>(buffer);
  std::vector<uint8_t> block(static_cast<size_t>(block_size));
  MaybeCrash(CrashPoint::kBeforeCowDataWrite, "cow data");
  for (const CowRun& run : runs) {
    for (uint32_t i = 0; i < run.len; ++i) {
      const uint64_t b = run.file_block + i;
      const uint64_t block_start = b << geometry_.block_log;
      const uint64_t block_end = block_start + block_size;
      const uint64_t user_start = std::max(offset, block_start);
      const uint64_t user_end = std::min(offset + length, block_end);
      const bool full_user =
          user_start == block_start && user_end == block_end;
      if (full_user) {
        if (src != nullptr) {
          std::memcpy(block.data(), src + (block_start - offset),
                      static_cast<size_t>(block_size));
        } else {
          std::memset(block.data(), 0, block.size());
        }
      } else {
        std::memset(block.data(), 0, block.size());
        if (block_start < old_size) {
          const uint64_t want =
              std::min<uint64_t>(block_size, old_size - block_start);
          auto read = ReadFileData(*device_, geometry_, inode, block_start,
                                   block.data(), static_cast<size_t>(want),
                                   &extents);
          if (!read.ok()) {
            cleanup();
            return read.error();
          }
        }
        if (user_end > user_start) {
          if (src != nullptr) {
            std::memcpy(block.data() + (user_start - block_start),
                        src + (user_start - offset),
                        static_cast<size_t>(user_end - user_start));
          } else {
            std::memset(block.data() + (user_start - block_start), 0,
                        static_cast<size_t>(user_end - user_start));
          }
        }
      }
      if (i > 0) {
        MaybeCrash(CrashPoint::kMidCowDataWrite, "cow data");
      }
      const uint64_t device_offset = geometry_.FsbToByte(run.fsb + i);
      if (!device_->Write(device_offset, block.data(), block.size())) {
        cleanup();
        return XFS_ERR(XfsError::kIoError,
                       "COW file data write failed: " + device_->LastError())
            .Ino(ino)
            .Offset(device_offset)
            .Metadata("device");
      }
    }
  }
  MaybeCrash(CrashPoint::kAfterCowDataWrite, "cow data");
  if (!device_->Flush()) {
    cleanup();
    return XFS_ERR(XfsError::kIoError,
                   "failed to flush COW file data: " + device_->LastError())
        .Ino(ino)
        .Metadata("device");
  }
  InvalidateCaches(CacheInvalidation::DataRange(ino, offset, length));

  // Remap: replace the whole block range with the staged private blocks and
  // drop the old shared/private mappings in one atomic metadata transaction.
  {
    auto txn = BeginTransaction();
    if (!txn.ok()) {
      cleanup();
      return txn.error();
    }
    BlockAllocator alloc(&txn.value(), device_.get(), &geometry_);
    auto fresh = OpenInode(ino);
    if (!fresh.ok()) {
      cleanup();
      return fresh.error();
    }
    DataForkEditor editor(&txn.value(), &alloc, device_.get(), &geometry_);
    Status s = editor.Load(fresh.value());
    if (!s.ok()) {
      cleanup();
      return s.error();
    }
    editor.SetFreeRemovedBlocks(true);
    s = editor.RemoveRange(first_block, last_block + 1);
    if (!s.ok()) {
      cleanup();
      return s.error();
    }
    for (const CowRun& run : runs) {
      // Staging accounting becomes the final private inode owner without any
      // free-space change; the blocks were already allocated.
      s = alloc.CommitCowStaging(
          run.fsb, run.len, RmapOwner::Data(ino, run.file_block, false));
      if (!s.ok()) {
        cleanup();
        return s.error();
      }
      XfsExtent mapping;
      mapping.startoff = run.file_block;
      mapping.startblock = run.fsb;
      mapping.blockcount = run.len;
      mapping.unwritten = false;
      s = editor.InsertExtent(mapping);
      if (!s.ok()) {
        cleanup();
        return s.error();
      }
      editor.AddNblocks(static_cast<int64_t>(run.len));
    }
    auto raw = ReadRawInode(ino);
    if (!raw.ok()) {
      cleanup();
      return raw.error();
    }
    std::vector<uint8_t> raw_inode = std::move(raw.value());
    uint32_t ilog_fields = 0;
    std::vector<uint8_t> fork_region;
    s = editor.Finalize(&raw_inode, &ilog_fields, &fork_region);
    if (!s.ok()) {
      cleanup();
      return s.error();
    }
    PutBE64(raw_inode.data(), di::kSize, new_size);
    PutBE64(raw_inode.data(), di::kFlags2,
            GetBE64(raw_inode.data(), di::kFlags2) | di::kDiFlag2Reflink);
    const XfsTimestamp now = CurrentTimestamp();
    s = StoreModifiedTimes(raw_inode.data(), now);
    if (!s.ok()) {
      cleanup();
      return s.error();
    }
    s = LogPreparedInode(txn.value(), ino, std::move(raw_inode), ilog_fields,
                         fork_region);
    if (!s.ok()) {
      cleanup();
      return s.error();
    }
    s = alloc.Commit();
    if (!s.ok()) {
      cleanup();
      return s.error();
    }
    MaybeCrash(CrashPoint::kBeforeCowRemapCommit, "cow remap");
    s = CommitTransaction(txn.value());
    if (!s.ok()) {
      // Once commit was attempted the log may already be durable. Do not try
      // to cancel staging here; recovery resolves the committed state.
      return s.error();
    }
  }
  MaybeCrash(CrashPoint::kAfterCowRemap, "cow remap");
  InvalidateCaches(CacheInvalidation::Extents(ino));
  return static_cast<size_t>(length);
}

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

  // Reflink sharing changes the write path entirely: a write to a shared
  // physical extent must be out-of-place.  The preflight reads the typed
  // refcount trees; an unreadable tree refuses before any data write.
  auto shared = RangeHasSharedData(*device_, geometry_, *extents.value(),
                                   plan.first_block, plan.last_block);
  if (!shared.ok()) return shared.error();
  if (shared.value()) {
    constexpr uint64_t kCowChunkBlocks = 64;
    const auto* src = static_cast<const uint8_t*>(buffer);
    for (uint64_t b = plan.first_block; b <= plan.last_block;) {
      const uint64_t b_last =
          std::min<uint64_t>(plan.last_block, b + kCowChunkBlocks - 1);
      const uint64_t chunk_start = std::max(offset, b << geometry_.block_log);
      const uint64_t chunk_end =
          std::min(end, (b_last + 1) << geometry_.block_log);
      if (chunk_start >= chunk_end) break;
      const uint64_t stage_size =
          b_last == plan.last_block
              ? plan.new_size
              : std::max<uint64_t>(old_size,
                                   (b_last + 1) << geometry_.block_log);
      auto wrote = CowReplaceRangeLocked(
          ino, inode.value(), *extents.value(), chunk_start,
          chunk_end - chunk_start, src + (chunk_start - offset), stage_size);
      if (!wrote.ok()) return wrote.error();
      b = b_last + 1;
    }
    InvalidateCaches(CacheInvalidation::Extents(ino));
    XFS_EXT_DEBUG("COW wrote " << length << " bytes at " << offset
                               << " in inode " << ino);
    return length;
  }

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
          allocated = alloc.AllocateOwned(
              preferred, static_cast<uint32_t>(want),
              RmapOwner::Data(ino, cursor, true), &allocation);
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
    s = StoreModifiedTimes(raw_inode.data(), now);
    if (!s.ok()) return s.error();
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
      s = alloc.ConvertOwned(
          fsb, static_cast<uint32_t>(last - first),
          RmapOwner::Data(ino, first, true), /*old_unwritten=*/true);
      if (!s.ok()) return s.error();
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
    s = StoreModifiedTimes(raw_inode.data(), now);
    if (!s.ok()) return s.error();
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

Result<XfsFileInfo> XfsFilesystem::AllocateUnwritten(uint64_t ino,
                                                   uint64_t offset,
                                                   uint64_t length) {
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume,
                   "filesystem is mounted read-only")
        .Ino(ino)
        .Metadata("fallocate");
  }
  if (length == 0) return GetFileInfo(ino);
  if (length > kMaxWriteBytes || offset > (1ull << 54) ||
      length > (1ull << 54) - offset) {
    return XFS_ERR(XfsError::kOutOfRange, "fallocate range out of range")
        .Ino(ino)
        .Actual(length)
        .Metadata("fallocate");
  }

  std::lock_guard<std::mutex> lock(mutation_mutex_);
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  if (!inode.value().IsRegular()) {
    return XFS_ERR(XfsError::kNotRegularFile,
                   "only regular files can be preallocated")
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

  const uint64_t end = offset + length;
  const uint64_t first_block = offset >> geometry_.block_log;
  const uint64_t last_block = (end - 1) >> geometry_.block_log;
  const uint64_t old_size = inode.value().size;
  const uint64_t new_size = std::max(old_size, end);

  auto txn = BeginTransaction();
  if (!txn.ok()) return txn.error();
  BlockAllocator alloc(&txn.value(), device_.get(), &geometry_);
  DataForkEditor editor(&txn.value(), &alloc, device_.get(), &geometry_);
  Status s = editor.Load(inode.value());
  if (!s.ok()) return s.error();

  const uint32_t preferred = geometry_.InoAgno(ino);
  uint64_t block = first_block;
  bool changed = false;
  while (block <= last_block) {
    const XfsExtent* cover = nullptr;
    if (FindExtentForBlock(editor.extents(), block, &cover)) {
      ++block;
      continue;
    }
    uint64_t run_end = block;
    while (run_end <= last_block &&
           !FindExtentForBlock(editor.extents(), run_end, &cover)) {
      ++run_end;
    }
    uint64_t remaining = run_end - block;
    uint64_t cursor = block;
    while (remaining > 0) {
      uint64_t want = remaining;
      Allocation allocation;
      Status allocated;
      while (true) {
        allocated = alloc.AllocateOwned(
            preferred, static_cast<uint32_t>(want),
            RmapOwner::Data(ino, cursor, true), &allocation);
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
      changed = true;
    }
    block = run_end;
  }

  if (!changed && new_size == old_size) return GetFileInfo(ino);

  auto raw = ReadRawInode(ino);
  if (!raw.ok()) return raw.error();
  std::vector<uint8_t> raw_inode = std::move(raw.value());
  uint32_t ilog_fields = 0;
  std::vector<uint8_t> fork_region;
  s = editor.Finalize(&raw_inode, &ilog_fields, &fork_region);
  if (!s.ok()) return s.error();
  if (new_size != old_size) {
    PutBE64(raw_inode.data(), di::kSize, new_size);
  }
  const XfsTimestamp now = CurrentTimestamp();
  s = StoreModifiedTimes(raw_inode.data(), now);
  if (!s.ok()) return s.error();
  s = LogPreparedInode(txn.value(), ino, std::move(raw_inode), ilog_fields,
                       fork_region);
  if (!s.ok()) return s.error();
  s = alloc.Commit();
  if (!s.ok()) return s.error();
  s = CommitTransaction(txn.value());
  if (!s.ok()) return s.error();

  InvalidateCaches(CacheInvalidation::Extents(ino));
  XFS_EXT_DEBUG("preallocated " << length << " bytes at " << offset
                                << " in inode " << ino << " (size " << old_size
                                << " -> " << new_size << ")");
  return GetFileInfo(ino);
}

Result<XfsFileInfo> XfsFilesystem::ConvertUnwritten(uint64_t ino,
                                                   uint64_t offset,
                                                   uint64_t length) {
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume,
                   "filesystem is mounted read-only")
        .Ino(ino)
        .Metadata("convert-unwritten");
  }
  if (length == 0) return GetFileInfo(ino);
  if (length > kMaxWriteBytes || offset > (1ull << 54) ||
      length > (1ull << 54) - offset) {
    return XFS_ERR(XfsError::kOutOfRange,
                   "convert-unwritten range out of range")
        .Ino(ino)
        .Actual(length)
        .Metadata("convert-unwritten");
  }

  std::lock_guard<std::mutex> lock(mutation_mutex_);
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  if (!inode.value().IsRegular()) {
    return XFS_ERR(XfsError::kNotRegularFile,
                   "only regular files can be converted")
        .Ino(ino);
  }

  const uint64_t first_block = offset >> geometry_.block_log;
  const uint64_t last_block =
      ((offset + length - 1) >> geometry_.block_log) + 1;

  // The conversion transaction only changes the rmap and bmap state; it does
  // not write file data.  Converting an unwritten range that the caller has
  // not written would expose stale disk bytes, so fill it with zeros and make
  // that durable *before* the state change becomes visible.  (A crash before
  // the conversion still reads zeros because the extent is unwritten.)
  auto extents = LoadExtentsCached(inode.value());
  if (!extents.ok()) return extents.error();
  std::vector<uint8_t> zeros(static_cast<size_t>(geometry_.block_size), 0);
  bool zeroed = false;
  for (const XfsExtent& e : *extents.value()) {
    if (!e.unwritten) continue;
    const uint64_t first = std::max<uint64_t>(e.startoff, first_block);
    const uint64_t last = std::min<uint64_t>(e.EndBlock(), last_block);
    if (first >= last) continue;
    uint64_t remaining = (last - first) << geometry_.block_log;
    uint64_t device_offset =
        geometry_.FsbToByte(e.startblock + (first - e.startoff));
    while (remaining > 0) {
      const size_t chunk = static_cast<size_t>(
          std::min<uint64_t>(remaining, zeros.size()));
      if (!device_->Write(device_offset, zeros.data(), chunk)) {
        return XFS_ERR(XfsError::kIoError,
                       "failed to zero an unwritten range: " +
                           device_->LastError())
            .Ino(ino)
            .Offset(device_offset)
            .Metadata("device");
      }
      device_offset += chunk;
      remaining -= chunk;
    }
    zeroed = true;
  }
  if (zeroed && !device_->Flush()) {
    return XFS_ERR(XfsError::kIoError,
                   "failed to flush zeroed unwritten data: " +
                       device_->LastError())
        .Ino(ino)
        .Metadata("device");
  }
  if (zeroed) {
    InvalidateCaches(CacheInvalidation::DataRange(ino, offset, length));
  }

  auto txn = BeginTransaction();
  if (!txn.ok()) return txn.error();
  BlockAllocator alloc(&txn.value(), device_.get(), &geometry_);
  DataForkEditor editor(&txn.value(), &alloc, device_.get(), &geometry_);
  Status s = editor.Load(inode.value());
  if (!s.ok()) return s.error();
  bool changed = false;
  const std::vector<XfsExtent> current = editor.extents();
  for (const XfsExtent& e : current) {
    if (!e.unwritten) continue;
    const uint64_t first = std::max<uint64_t>(e.startoff, first_block);
    const uint64_t last = std::min<uint64_t>(e.EndBlock(), last_block);
    if (first >= last) continue;
    const uint64_t fsb = e.startblock + (first - e.startoff);
    s = alloc.ConvertOwned(fsb, static_cast<uint32_t>(last - first),
                           RmapOwner::Data(ino, first, true),
                           /*old_unwritten=*/true);
    if (!s.ok()) return s.error();
    s = editor.RemoveRange(first, last);
    if (!s.ok()) return s.error();
    XfsExtent converted;
    converted.startoff = first;
    converted.startblock = fsb;
    converted.blockcount = last - first;
    converted.unwritten = false;
    s = editor.InsertExtent(converted);
    if (!s.ok()) return s.error();
    changed = true;
  }
  if (!changed) return GetFileInfo(ino);

  auto raw = ReadRawInode(ino);
  if (!raw.ok()) return raw.error();
  std::vector<uint8_t> raw_inode = std::move(raw.value());
  uint32_t ilog_fields = 0;
  std::vector<uint8_t> fork_region;
  s = editor.Finalize(&raw_inode, &ilog_fields, &fork_region);
  if (!s.ok()) return s.error();
  const XfsTimestamp now = CurrentTimestamp();
  s = StoreModifiedTimes(raw_inode.data(), now);
  if (!s.ok()) return s.error();
  s = LogPreparedInode(txn.value(), ino, std::move(raw_inode), ilog_fields,
                       fork_region);
  if (!s.ok()) return s.error();
  s = alloc.Commit();
  if (!s.ok()) return s.error();
  s = CommitTransaction(txn.value());
  if (!s.ok()) return s.error();

  InvalidateCaches(CacheInvalidation::Extents(ino));
  XFS_EXT_DEBUG("converted " << length << " unwritten bytes at " << offset
                             << " in inode " << ino);
  return GetFileInfo(ino);
}

Result<std::vector<XfsExtent>> XfsFilesystem::GetDataExtents(
    uint64_t ino) const {
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  auto extents = LoadExtentsCached(inode.value());
  if (!extents.ok()) return extents.error();
  return *extents.value();
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
        const uint64_t first_block =
            mapping.file_offset >> geometry_.block_log;
        const uint64_t last_block =
            (mapping.file_offset + mapping.byte_count - 1) >>
            geometry_.block_log;
        auto shared = RangeHasSharedData(*device_, geometry_, *extents.value(),
                                         first_block, last_block);
        if (!shared.ok()) return shared.error();
        if (shared.value()) {
          // Zeroing an already existing shared extent is a write: it must be
          // out-of-place and preserve the bytes before z0 in a partial block.
          for (uint64_t b = first_block; b <= last_block;) {
            const uint64_t b_last = std::min<uint64_t>(last_block, b + 63);
            const uint64_t chunk_end =
                std::min(mapping.file_offset + mapping.byte_count,
                         (b_last + 1) << geometry_.block_log);
            const uint64_t chunk_start =
                std::max(mapping.file_offset, b << geometry_.block_log);
            if (chunk_start >= chunk_end) break;
            const uint64_t stage_size =
                b_last == last_block
                    ? new_size
                    : std::min<uint64_t>(
                          new_size,
                          std::max<uint64_t>(
                              old_size,
                              (b_last + 1) << geometry_.block_log));
            auto zeroed = CowReplaceRangeLocked(
                ino, inode.value(), *extents.value(), chunk_start,
                chunk_end - chunk_start, nullptr, stage_size);
            if (!zeroed.ok()) return zeroed.error();
            b = b_last + 1;
          }
          continue;
        }
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
        [&](uint8_t* raw) -> Status {
          PutBE64(raw, di::kSize, new_size);
          return StoreModifiedTimes(raw, now);
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
      auto shared = RangeHasSharedData(*device_, geometry_, *extents.value(),
                                       last_block, last_block);
      if (!shared.ok()) return shared.error();
      if (shared.value()) {
        const uint64_t block_end =
            (last_block + 1) << geometry_.block_log;
        auto zeroed = CowReplaceRangeLocked(
            ino, inode.value(), *extents.value(), new_size,
            block_end - new_size, nullptr, new_size);
        if (!zeroed.ok()) return zeroed.error();
        InvalidateCaches(CacheInvalidation::Extents(ino));
      } else {
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
      s = StoreModifiedTimes(raw_inode.data(), now);
      if (!s.ok()) return s.error();
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
