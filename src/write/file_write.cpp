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
#include <functional>
#include <map>
#include <vector>

#include "xfs/alloc.h"
#include "xfs/bmap.h"
#include "xfs/crash.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/perf_stats.h"
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

// Writes the block range [offset, offset+length) through map, which returns
// the physical filesystem block backing a file block. Fully covered blocks
// that are physically contiguous are transferred with one device write
// directly from the caller's buffer; only the first/last partial block needs a
// read-modify-write scratch block. This is the PERF-1 fix for the old
// block-by-block loop (a 1 MiB write used to issue 256 4 KiB WriteFiles).
//
// old_inode/old_extents/old_size describe the file before this write and
// are used only to preserve bytes outside the caller's range in a partial
// block. src == nullptr means a pure zero-fill (COW zero range).
Status WriteBlockRange(
    BlockDevice& device, const XfsGeometry& geo, uint64_t ino, uint64_t offset,
    uint64_t length, const void* src, const XfsInode& old_inode,
    const std::vector<XfsExtent>& old_extents, uint64_t old_size,
    const std::function<bool(uint64_t, uint64_t*)>& map,
    CrashPoint mid_point, const char* phase) {
  const uint64_t block_size = geo.block_size;
  const uint64_t end = offset + length;
  const uint64_t first_block = offset >> geo.block_log;
  const uint64_t last_block = (end - 1) >> geo.block_log;
  const auto* input = static_cast<const uint8_t*>(src);
  // Crash tests historically observed a "mid data write" point after the
  // first block. A coalesced multi-block run would skip that occurrence, so
  // cap the first run to one block only while that injection point is armed.
  const bool crash_mid = CrashPointArmed(mid_point);

  std::vector<uint8_t> partial;
  std::vector<uint8_t> zeros;
  bool wrote_any = false;

  for (uint64_t block = first_block; block <= last_block;) {
    const uint64_t block_start = block << geo.block_log;
    const uint64_t block_end = block_start + block_size;
    uint64_t fsb = 0;
    if (!map(block, &fsb)) {
      return XFS_ERR(XfsError::kInternal,
                     "write block is not covered by an extent")
          .Ino(ino)
          .Actual(block)
          .Metadata(phase);
    }

    const bool fully_covered =
        block_start >= offset && block_end <= end;
    if (!fully_covered) {
      if (wrote_any) MaybeCrash(mid_point, phase);
      partial.assign(static_cast<size_t>(block_size), 0);
      if (block_start < old_size) {
        const uint64_t want = std::min<uint64_t>(block_size,
                                                 old_size - block_start);
        auto read = ReadFileData(device, geo, old_inode, block_start,
                                 partial.data(), static_cast<size_t>(want),
                                 &old_extents);
        if (!read.ok()) return read.error();
      }
      const uint64_t user_start = std::max(offset, block_start);
      const uint64_t user_end = std::min(end, block_end);
      if (user_end > user_start) {
        auto* at = partial.data() + (user_start - block_start);
        const size_t count = static_cast<size_t>(user_end - user_start);
        if (input != nullptr) {
          std::memcpy(at, input + (user_start - offset), count);
        } else {
          std::memset(at, 0, count);
        }
      }
      const uint64_t device_offset = geo.FsbToByte(fsb);
      if (!device.Write(device_offset, partial.data(), partial.size())) {
        return XFS_ERR(XfsError::kIoError,
                       "file data write failed: " + device.LastError())
            .Ino(ino)
            .Offset(device_offset)
            .Metadata("device");
      }
      wrote_any = true;
      ++block;
      continue;
    }

    // Maximal run of full blocks whose physical blocks are contiguous.
    uint64_t run_blocks = 1;
    uint64_t next_fsb = fsb + 1;
    while (block + run_blocks <= last_block) {
      const uint64_t candidate = block + run_blocks;
      const uint64_t candidate_start = candidate << geo.block_log;
      const uint64_t candidate_end = candidate_start + block_size;
      if (candidate_start < offset || candidate_end > end) break;
      uint64_t candidate_fsb = 0;
      if (!map(candidate, &candidate_fsb) || candidate_fsb != next_fsb) break;
      ++run_blocks;
      ++next_fsb;
    }

    if (wrote_any) MaybeCrash(mid_point, phase);
    if (crash_mid && !wrote_any && run_blocks > 1) run_blocks = 1;
    const uint64_t device_offset = geo.FsbToByte(fsb);
    const uint64_t run_bytes = run_blocks * block_size;
    if (input != nullptr) {
      if (!device.Write(device_offset, input + (block_start - offset),
                        static_cast<size_t>(run_bytes))) {
        return XFS_ERR(XfsError::kIoError,
                       "file data write failed: " + device.LastError())
            .Ino(ino)
            .Offset(device_offset)
            .Metadata("device");
      }
    } else {
      if (zeros.empty()) zeros.assign(static_cast<size_t>(std::min<uint64_t>(
                                          block_size * 256, 1u << 20)),
                                      0);
      uint64_t pos = device_offset;
      uint64_t remaining = run_bytes;
      while (remaining > 0) {
        const size_t chunk =
            static_cast<size_t>(std::min<uint64_t>(remaining, zeros.size()));
        if (!device.Write(pos, zeros.data(), chunk)) {
          return XFS_ERR(XfsError::kIoError,
                         "file data write failed: " + device.LastError())
              .Ino(ino)
              .Offset(pos)
              .Metadata("device");
        }
        pos += chunk;
        remaining -= chunk;
      }
    }
    wrote_any = true;
    block += run_blocks;
  }
  return OkStatus();
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
  const uint64_t first_block = offset >> geometry_.block_log;
  const uint64_t last_block = (offset + length - 1) >> geometry_.block_log;
  const uint64_t block_count = last_block - first_block + 1;
  const uint64_t end = offset + length;
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
  MaybeCrash(CrashPoint::kBeforeCowDataWrite, "cow data");
  for (const CowRun& run : runs) {
    auto map = [&](uint64_t block, uint64_t* fsb) -> bool {
      if (block < run.file_block || block >= run.file_block + run.len) {
        return false;
      }
      *fsb = run.fsb + (block - run.file_block);
      return true;
    };
    const uint64_t run_start_byte =
        run.file_block << geometry_.block_log;
    const uint64_t run_end_byte =
        std::min(end, (run.file_block + run.len) << geometry_.block_log);
    const uint64_t helper_start = std::max(offset, run_start_byte);
    const uint64_t helper_end = std::min(end, run_end_byte);
    if (helper_start >= helper_end) continue;
    const uint8_t* helper_src =
        src != nullptr ? src + (helper_start - offset) : nullptr;
    Status write_status = WriteBlockRange(
        *device_, geometry_, ino, helper_start, helper_end - helper_start,
        helper_src, inode, extents, old_size, map,
        CrashPoint::kMidCowDataWrite, "cow data");
    if (!write_status.ok()) {
      cleanup();
      return write_status.error();
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
                                            bool append,
                                            bool defer_timestamps) {
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

  PerfStats& perf = PerfStats::Get();
  const bool perf_on = perf.enabled();
  if (perf_on) perf.write_calls.fetch_add(1, std::memory_order_relaxed);

  // Ordinary file-data I/O is concurrent across files. The shared barrier lets
  // independent writes proceed in parallel and excludes namespace changes,
  // truncate/reflink and Flush while the data range is being transferred. The
  // inode stripe serializes writes to the same file (including append EOF
  // allocation). Metadata transactions still take mutation_mutex_ below, but
  // only around the transaction, never across the data transfer.
  std::shared_lock<std::shared_mutex> barrier(write_barrier_);
  TimedLockGuard inode_lock(InodeLock(ino));
  // Count actual in-flight writes, not callers queued on the inode lock.
  PerfStats::WriteScope write_scope;

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
  // Sequential writers (Windows copy, archive extraction) issue many small
  // appends. Allocate a bounded unwritten lookahead beyond EOF on the first
  // large append so the following writes only convert the range they touch
  // instead of paying an allocation transaction each time. The extra blocks
  // remain unwritten and invisible: EOF does not move and reads return zero.
  uint64_t alloc_last_block = plan.last_block;
  if (offset >= old_size && length >= 64 * 1024) {
    // Only start a new lookahead window when the existing unwritten
    // preallocation no longer reaches past this write. Otherwise every 1 MiB
    // append would extend the window by another 8 MiB and still pay an
    // allocation transaction.
    bool unwritten_ahead = false;
    for (const XfsExtent& e : *extents.value()) {
      if (e.unwritten && e.EndBlock() > plan.last_block + 1) {
        unwritten_ahead = true;
        break;
      }
    }
    if (!unwritten_ahead) {
      constexpr uint64_t kAllocationLookaheadBytes = 8ull << 20;
      const uint64_t extra_blocks =
          (kAllocationLookaheadBytes + block_size - 1) >> geometry_.block_log;
      const uint64_t max_block = ((1ull << 54) >> geometry_.block_log) - 1;
      alloc_last_block =
          std::min<uint64_t>(plan.last_block + extra_blocks, max_block);
    }
  }

  auto shared = RangeHasSharedData(*device_, geometry_, *extents.value(),
                                   plan.first_block, plan.last_block);
  if (!shared.ok()) return shared.error();
  if (shared.value()) {
    // COW staging/remap is a multi-transaction operation with ordering
    // dependencies; keep it serialized for now (the ordinary overwrite path is
    // the one that must scale).
    std::lock_guard<std::mutex> meta_lock(mutation_mutex_);
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
    if (perf_on) perf.write_bytes.fetch_add(length, std::memory_order_relaxed);
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
      {
        const XfsTimestamp now = CurrentTimestamp();
        if (defer_timestamps) {
          // Adapter fast path: publish the new mtime/ctime in the in-memory
          // inode now, and let Flush/Cleanup write the metadata transaction.
          // A crash before that boundary may lose the timestamp (the data is
          // not durable before Flush either); the ordering of data before
          // metadata is unchanged.
          MarkDeferredTimes(ino, now);
        } else {
          // Only the timestamp transaction is serialized with metadata work;
          // the data transfer above was concurrent across files.
          std::lock_guard<std::mutex> txn_lock(mutation_mutex_);
          auto txn = BeginTransaction();
          if (!txn.ok()) return txn.error();
          Status status = UpdateTimesInTransaction(
              txn.value(), ino, inode.value().atime, now, now);
          if (status.ok()) status = CommitTransaction(txn.value());
          if (!status.ok()) return status.error();
        }
      }
      if (perf_on) perf.write_bytes.fetch_add(length, std::memory_order_relaxed);
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
    std::lock_guard<std::mutex> txn_lock(mutation_mutex_);
    auto txn = BeginTransaction();
    if (!txn.ok()) return txn.error();
    BlockAllocator alloc(&txn.value(), device_.get(), &geometry_);
    DataForkEditor editor(&txn.value(), &alloc, device_.get(), &geometry_);
    Status s = editor.Load(inode.value());
    if (!s.ok()) return s.error();

    const uint32_t preferred = geometry_.InoAgno(ino);
    uint64_t block = plan.first_block;
    bool allocated_any = false;
    while (block <= alloc_last_block) {
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
      while (run_end <= alloc_last_block) {
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
        allocated_any = true;
        cursor += allocation.len;
        remaining -= allocation.len;
      }
      block = run_end;
    }

    if (allocated_any) {
      // Write the new fork into the inode (size unchanged in this
      // transaction). When the range was already covered by an unwritten
      // lookahead extent there is nothing to log or flush here.
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
  }
  // 2. Data transfer. Physically contiguous full blocks are coalesced into
  //    large positioned writes; only a partial head/tail block needs a
  //    read-modify-write scratch buffer. Then one flush makes the bytes
  //    durable before the conversion transaction can expose them.
  {
    auto fresh_inode = OpenInode(ino);
    if (!fresh_inode.ok()) return fresh_inode.error();
    auto fresh_extents = LoadExtentsCached(fresh_inode.value());
    if (!fresh_extents.ok()) return fresh_extents.error();

    auto map = [&](uint64_t file_block, uint64_t* fsb) -> bool {
      const XfsExtent* extent = nullptr;
      if (!FindExtentForBlock(*fresh_extents.value(), file_block, &extent)) {
        return false;
      }
      *fsb = extent->startblock + (file_block - extent->startoff);
      return true;
    };

    MaybeCrash(CrashPoint::kBeforeDataWrite, "write-path allocate");
    Status write_status = WriteBlockRange(
        *device_, geometry_, ino, offset, length, src, fresh_inode.value(),
        *fresh_extents.value(), old_size, map, CrashPoint::kMidDataWrite,
        "write-path general");
    if (!write_status.ok()) return write_status.error();
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
    std::lock_guard<std::mutex> txn_lock(mutation_mutex_);
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
  if (perf_on) perf.write_bytes.fetch_add(length, std::memory_order_relaxed);
  XFS_EXT_DEBUG("wrote " << length << " bytes at " << offset << " in inode "
                         << ino << " (size " << old_size << " -> "
                         << plan.new_size << ")");
  return length;
}

Result<XfsFileInfo> XfsFilesystem::AllocateUnwritten(uint64_t ino,
                                                   uint64_t offset,
                                                   uint64_t length,
                                                   bool keep_size) {
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

  // Changing the extent map must not overlap a data transfer to this inode.
  std::unique_lock<std::shared_mutex> barrier(write_barrier_);
  TimedLockGuard inode_lock(InodeLock(ino));
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
  // Windows SetAllocationSize wants allocation without moving EOF. Fallocate
  // mode 0 (the CLI path) keeps the historical "grow the file" semantics.
  const uint64_t new_size = keep_size ? old_size : std::max(old_size, end);

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
  if (!keep_size && new_size != old_size) {
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

  std::unique_lock<std::shared_mutex> barrier(write_barrier_);
  TimedLockGuard inode_lock(InodeLock(ino));
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
  std::unique_lock<std::shared_mutex> barrier(write_barrier_);
  TimedLockGuard inode_lock(InodeLock(ino));
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
    }
    // A cached data chunk was sized to the old EOF.  Growing the file can make
    // that chunk short even when no written extent had to be zeroed, so the
    // invalidation belongs outside the zero-range branch.
    InvalidateCaches(CacheInvalidation::DataRange(
        ino, old_size, new_size - old_size));
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
