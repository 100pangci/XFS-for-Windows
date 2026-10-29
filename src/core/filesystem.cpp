// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/filesystem.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>

#include "xfs/ag.h"
#include "xfs/btree.h"
#include "xfs/crc32c.h"
#include "xfs/crash.h"
#include "xfs/endian.h"
#include "xfs/extent.h"
#include "xfs/ialloc.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/perf_stats.h"
#include "xfs/refcount.h"
#include "xfs/rmap.h"
#include "xfs/symlink.h"
#include "xfs/verifier.h"
#include "xfs/write.h"

namespace xfs {
namespace {

namespace bt = ondisk::btree;
namespace di = ondisk::inode;
namespace sym = ondisk::symlink;

constexpr uint32_t kNullAgBlock = 0xffffffffu;

std::string Hex(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

// Looks up the inobt record that contains `agino`. Sparse filesystems use
// this on the read path so an inode number in a hole is reported as "not
// allocated" instead of reading whatever bytes happen to be on the device.
Result<InobtRecord> LookupInodeRecord(BlockDevice& dev, const XfsGeometry& geo,
                                      uint32_t agno, uint32_t agino) {
  auto agi = ReadAgi(dev, geo, agno);
  if (!agi.ok()) return agi.error();
  BtreeSpec spec;
  spec.name = "inobt";
  spec.format = BtreeFormat::kShort;
  spec.block_size = geo.block_size;
  spec.key_size = bt::kInobtKeySize;
  spec.ptr_size = bt::kInobtPtrSize;
  spec.rec_size = bt::kInobtRecSize;
  spec.magic = ondisk::kIbtCrcMagic;
  spec.null_ptr = kNullAgBlock;
  spec.expected_owner = agno;
  spec.owner_size = 4;
  spec.uuid = geo.meta_uuid;
  spec.ptr_to_daddr = [&](uint64_t agbno, uint64_t* daddr) {
    if (agbno >= geo.AgBlocksFor(agno)) return false;
    *daddr = geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))) /
             512;
    return true;
  };
  InobtRecord found;
  bool have = false;
  BtreeWalkCallbacks cb;
  cb.read_block = [&](uint64_t agbno, std::vector<uint8_t>* buf) {
    if (agbno >= geo.AgBlocksFor(agno)) return false;
    buf->resize(geo.block_size);
    return dev.Read(geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))),
                    buf->data(), buf->size());
  };
  cb.ptr_in_range = [&](uint64_t agbno) {
    return agbno < geo.AgBlocksFor(agno);
  };
  cb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                 const uint8_t* buf) -> Status {
    if (meta.level != 0) return OkStatus();
    for (uint16_t i = 0; i < meta.numrecs; ++i) {
      const uint8_t* rec = BtreeLeafRec(buf, meta, spec, i);
      InobtRecord r;
      r.startino = GetBE32(rec);
      r.free = GetBE64(rec + 8);
      if (geo.features.spinodes) {
        r.holemask = GetBE16(rec + 4);
        r.count = GetU8(rec + 6);
        r.freecount = GetU8(rec + 7);
      } else {
        r.holemask = 0;
        r.count = ondisk::kInodesPerChunk;
        r.freecount = GetBE32(rec + 4);
      }
      if (agino >= r.startino &&
          agino < r.startino + ondisk::kInodesPerChunk) {
        found = r;
        have = true;
      }
    }
    return OkStatus();
  };
  Status s = WalkBtreePath(spec, agi.value().root, agino, cb);
  if (!s.ok()) return s.error();
  if (!have) {
    return XFS_ERR(XfsError::kNotFound,
                   "inode number has no inobt record")
        .Ag(agno)
        .Metadata("inobt")
        .Actual(agino);
  }
  return found;
}

}  // namespace

const char* ToString(CheckStatus status) {
  switch (status) {
    case CheckStatus::kOk: return "OK";
    case CheckStatus::kWarning: return "WARNING";
    case CheckStatus::kCorrupt: return "CORRUPT";
    case CheckStatus::kUnsupported: return "UNSUPPORTED";
  }
  return "?";
}

void CheckReport::Add(std::string name, CheckStatus status,
                      std::string message) {
  items.push_back({std::move(name), status, std::move(message)});
}

CheckStatus CheckReport::Overall() const {
  CheckStatus worst = CheckStatus::kOk;
  auto rank = [](CheckStatus s) {
    switch (s) {
      case CheckStatus::kOk: return 0;
      case CheckStatus::kWarning: return 1;
      case CheckStatus::kUnsupported: return 2;
      case CheckStatus::kCorrupt: return 3;
    }
    return 0;
  };
  for (const auto& item : items) {
    if (rank(item.status) > rank(worst)) worst = item.status;
  }
  return worst;
}

std::string CheckReport::Summary() const {
  int ok = 0, warn = 0, corrupt = 0, unsupported = 0;
  for (const auto& item : items) {
    switch (item.status) {
      case CheckStatus::kOk: ++ok; break;
      case CheckStatus::kWarning: ++warn; break;
      case CheckStatus::kCorrupt: ++corrupt; break;
      case CheckStatus::kUnsupported: ++unsupported; break;
    }
  }
  std::string s = std::string(ToString(Overall())) + ": " +
                  std::to_string(ok) + " ok, " + std::to_string(warn) +
                  " warnings, " + std::to_string(corrupt) + " corrupt, " +
                  std::to_string(unsupported) + " unsupported";
  return s;
}

Result<std::unique_ptr<XfsFilesystem>> XfsFilesystem::Mount(
    std::unique_ptr<BlockDevice> device, const MountOptions& options) {
  if (options.perf_stats) PerfStats::Get().SetEnabled(true);
  if (!device) {
    return XFS_ERR(XfsError::kInvalidArgument, "no block device supplied");
  }
  auto sb_result = ReadSuperblock(*device, 0, options.verify_crc);
  if (!sb_result.ok()) return sb_result.error();
  XfsSuperblock sb = std::move(sb_result.value());

  Status mountable = CheckMountable(sb);
  if (!mountable.ok()) return mountable.error();

  XfsGeometry geo = MakeGeometry(sb);
  if (geo.dblocks * geo.block_size > device->Size()) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "filesystem larger than the device")
        .Expected(device->Size())
        .Actual(geo.dblocks * geo.block_size);
  }

  auto fs = std::unique_ptr<XfsFilesystem>(new XfsFilesystem());
  fs->superblock_ = std::move(sb);
  fs->geometry_ = std::move(geo);
  fs->options_ = options;
  fs->device_ = std::move(device);
  fs->writable_ = false;

  // Whitespace in the label: pad with spaces, not NULs (mkfs does this).
  XFS_SB_INFO("XFS v5 filesystem, uuid=" << FormatUuid(fs->superblock_.uuid));
  XFS_SB_INFO("blocksize=" << fs->geometry_.block_size
                           << " sectorsize=" << fs->geometry_.sector_size
                           << " inode size=" << fs->geometry_.inode_size);
  XFS_SB_INFO("agcount=" << fs->geometry_.agcount
                         << " agblocks=" << fs->geometry_.agblocks
                         << " dblocks=" << fs->geometry_.dblocks);
  XFS_SB_INFO("logstart=" << fs->geometry_.logstart
                          << " logblocks=" << fs->geometry_.logblocks);
  XFS_SB_INFO("root inode=" << fs->geometry_.rootino);

  if (fs->superblock_.features.ro_compat_unknown) {
    fs->warnings_.push_back(
        "superblock has unknown read-only compatible features (" +
        Hex(fs->superblock_.features.ro_compat_unknown) +
        "); read-only access proceeds");
  }
  if (fs->superblock_.features.compat_unknown) {
    fs->warnings_.push_back(
        "superblock has unknown compatible features (" +
        Hex(fs->superblock_.features.compat_unknown) + ")");
  }
  if (HasLogIncompatFeatures(fs->superblock_)) {
    fs->warnings_.push_back(
        "log-incompatible features are set; the log may contain records "
        "this build does not understand");
  }

  // Read the root inode before declaring success.
  auto root = fs->OpenInode(fs->geometry_.rootino);
  if (!root.ok()) return root.error();
  if (!root.value().IsDir()) {
    return XFS_ERR(XfsError::kCorruptInode, "root inode is not a directory")
        .Ino(fs->geometry_.rootino);
  }

  // Log state: scanned and validated with the recovery engine (RW-5). A
  // read-write mount replays a dirty log; a read-only mount refuses unless the
  // caller explicitly asked for the unsafe pre-recovery view.
  LogState log_state = LogState::kUnknown;
  bool log_checked = false;
  bool recovered = false;
  if (options.check_log) {
    auto scan = ScanLog(*fs->device_, fs->superblock_, fs->geometry_);
    if (!scan.ok()) {
      if (!options.force_unsafe_ro) return scan.error();
      fs->warnings_.push_back("log check failed: " + scan.error().message);
    } else {
      const LogScan& log = scan.value();
      log_checked = true;
      log_state = log.state;
      if (log.state == LogState::kClean || log.state == LogState::kEmpty) {
        XFS_SB_INFO("log is " << ToString(log.state) << " (" << log.detail
                              << ")");
        // An explicit recovery still has to finish the log: it adopts the
        // scanned position, runs the completion phases (the kernel processes
        // the AGI unlinked lists on every mount, clean log or not) and closes
        // the log with an unmount record when it does not have one. Without
        // this the close path would classify the log a second time, and a torn
        // record behind the newest good one reads as clean to the scanner but
        // not to the record-latest classifier.
        if (options.read_write && options.clean_log_after_recovery) {
          auto report = fs->RecoverScannedLog(log, RecoveryFinish::kCleanLog);
          if (!report.ok()) return report.error();
          recovered = true;
          fs->recovery_report_ = report.value();
          XFS_SB_INFO("clean log finished: unlinked="
                      << report.value().unlinked_inodes_processed);
        }
      } else {
        const bool can_recover = options.read_write &&
                                 options.recover_dirty_log;
        if (can_recover && !fs->device_->Info().writable) {
          return XFS_ERR(XfsError::kReadOnlyVolume,
                         "the log needs replay but the device was opened "
                         "read-only")
              .Metadata("log");
        }
        if (can_recover) {
          // Never recover a filesystem this build would then refuse to
          // modify: check the profile first.
          Status profile = CheckWritableProfile(
              fs->superblock_, fs->geometry_, options.recovery_only,
              options.allow_rmapbt_qualification, options.recovery_only,
              options.allow_reflink_qualification,
              options.allow_parent_qualification);
          if (!profile.ok()) {
            auto error = profile.error();
            error.message +=
                " (read-only access is unaffected; mount without the "
                "read-write option)";
            return error;
          }
          auto report = fs->RecoverScannedLog(
              log, options.clean_log_after_recovery
                       ? RecoveryFinish::kCleanLog
                       : RecoveryFinish::kMount);
          if (!report.ok()) return report.error();
          recovered = true;
          fs->recovery_report_ = report.value();
          XFS_SB_INFO("log recovered: transactions="
                      << report.value().replay.transactions_replayed
                      << " items=" << report.value().replay.items_replayed
                      << " buffers=" << report.value().replay.buffers_replayed
                      << " inodes=" << report.value().replay.inodes_replayed
                      << " intents=" << report.value().intents_completed
                      << " unlinked="
                      << report.value().unlinked_inodes_processed);
        } else if (!options.force_unsafe_ro) {
          std::string detail =
              "recoverable transactions=" + std::to_string(log.transactions.size());
          if (!log.unsupported_items.empty()) {
            detail += ", unsupported items=" +
                      std::to_string(log.unsupported_items.size()) + " (" +
                      log.unsupported_items.front() + ")";
          }
          return XFS_ERR(XfsError::kDirtyLog,
                         "log is not clean (" + log.detail + "; " + detail +
                             "); refusing to mount. Use --force-unsafe-ro to "
                             "mount read-only with the pre-recovery metadata, "
                             "or mount read-write (or run `xfswin recover`) to "
                             "replay the journal.")
              .Metadata("log");
        } else {
          fs->warnings_.push_back(
              "log is " + std::string(ToString(log.state)) + " (" +
              log.detail +
              "); mounting read-only with --force-unsafe-ro: metadata may "
              "reflect a pre-recovery state");
        }
      }
    }
  }

  // Read-write mounts are an allow-list: writable device, clean or recovered
  // log, and exactly the feature profile this build can modify. A refusal is
  // an error (never a silent read-only mount), so the caller decides.
  if (options.read_write) {
    if (!fs->device_->Info().writable) {
      return XFS_ERR(XfsError::kReadOnlyVolume,
                     "read-write mount requested but the device was opened "
                     "read-only")
          .Metadata("mount");
    }
    if (!log_checked) {
      return XFS_ERR(XfsError::kDirtyLog,
                     "read-write mounts require the log to be checked")
          .Metadata("log");
    }
    if (!recovered && log_state != LogState::kClean &&
        log_state != LogState::kEmpty) {
      return XFS_ERR(XfsError::kDirtyLog,
                     "read-write mounts require a clean or recoverable log "
                     "(recovery was disabled with recover_dirty_log=false)")
          .Metadata("log");
    }
    Status profile = CheckWritableProfile(
        fs->superblock_, fs->geometry_, options.recovery_only,
        options.allow_rmapbt_qualification, options.recovery_only,
        options.allow_reflink_qualification,
        options.allow_parent_qualification);
    if (!profile.ok()) {
      auto error = profile.error();
      error.message +=
          " (read-only access is unaffected; mount without the read-write "
          "option)";
      return error;
    }
    // A writable mount must never proceed on a namespace whose forward
    // dirents and reverse parent pointers already disagree.  Recovery has
    // finished at this point, so this is a clean preflight over committed
    // metadata, not a pre-replay snapshot.
    // Recovery-only mounts complete surviving intents first; they are not a
    // long-lived writable namespace and must be allowed to converge even when
    // the pre-recovery view was inconsistent.  The follow-up check is the
    // caller's gate before exposing such an image read-write.
    if (fs->superblock_.features.parent && !options.recovery_only) {
      auto audit = fs->VerifyParentPointers();
      if (!audit.ok() || !audit.value().clean()) {
        std::string detail =
            audit.ok() ? audit.value().Summary() : audit.error().Format();
        return XFS_ERR(XfsError::kNeedsRepair,
                       "parent-pointer namespace is inconsistent (" + detail +
                           "); run xfswin check, then repair or restore the "
                           "image")
            .Metadata("parent pointers");
      }
    }
    fs->writable_ = true;
    XFS_SB_INFO("mounted read-write: " << WritableProfileSummary(fs->superblock_));
    if (!recovered) {
      // A clean log can still carry zero-link inodes on the AGI unlinked
      // lists (a crash after the unlink transaction was flushed). Linux
      // processes them on every mount; so do we, before the volume is
      // visible (xlog_recover_process_iunlinks).
      uint64_t cleared = 0;
      auto processed = fs->ProcessAgiUnlinkedLists(&cleared);
      if (!processed.ok()) return processed.error();
      if (processed.value() != 0 || cleared != 0) {
        XFS_SB_INFO("unlinked inode recovery: freed="
                    << processed.value() << " cleared_buckets=" << cleared);
      }
    }
    if (fs->geometry_.features.reflink) {
      // xfs_reflink_recover_cow runs on every writable mount, including a
      // clean one.  A crash between CoW allocation and remap leaves COW
      // staging records that no inode references any more.
      auto cow = fs->RecoverCowStaging();
      if (!cow.ok()) {
        auto error = cow.error();
        error.message +=
            " (CoW staging recovery is required before a reflink mount)";
        return error;
      }
      if (fs->recovery_report_.has_value()) {
        fs->recovery_report_->cow_staging_freed = cow.value();
      }
      if (cow.value() != 0) {
        XFS_SB_INFO("CoW staging recovery: freed " << cow.value()
                                                   << " orphan extents");
      }
    }
    if (options.recovery_only) {
      // Recovery already completed (or the log was clean).  Do not leave a
      // recovery-only mount usable as a general writable filesystem.
      fs->writable_ = false;
    }
  }

  for (const auto& w : fs->warnings_) XFS_WARN(w);
  return fs;
}

Result<XfsInode> XfsFilesystem::OpenInode(uint64_t ino) const {
  PerfStats& perf = PerfStats::Get();
  const bool perf_on = perf.enabled();
  if (perf_on) perf.inode_opens.fetch_add(1, std::memory_order_relaxed);
  auto overlay_deferred = [&](XfsInode& inode) {
    std::lock_guard<std::mutex> deferred_lock(deferred_mutex_);
    auto it = deferred_times_.find(ino);
    if (it != deferred_times_.end()) {
      inode.mtime = it->second;
      inode.ctime = it->second;
    }
  };
  auto finish = [&](XfsInode inode) -> Result<XfsInode> {
    if (inode.OnUnlinkedList() && !writable_) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "inode is on the unlinked list; reading it read-only is "
                     "not safe without journal replay")
          .Ino(ino)
          .Metadata("inode");
    }
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      constexpr size_t kMaxCachedInodes = 32768;
      if (inode_cache_.size() >= kMaxCachedInodes) inode_cache_.clear();
      inode_cache_.emplace(ino, inode);
    }
    overlay_deferred(inode);
    return inode;
  };

  bool cache_hit = false;
  XfsInode cached;
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = inode_cache_.find(ino);
    if (it != inode_cache_.end()) {
      cached = it->second;
      cache_hit = true;
    }
  }
  if (cache_hit) {
    if (perf_on) perf.inode_cache_hits.fetch_add(1, std::memory_order_relaxed);
    // Overlay after dropping cache_mutex_: deferred_mutex_ is a leaf and must
    // not extend the inode-cache critical section.
    overlay_deferred(cached);
    return cached;
  }
  if (perf_on) perf.inode_cache_misses.fetch_add(1, std::memory_order_relaxed);

  if (!geometry_.InoValid(ino)) {
    return XFS_ERR(XfsError::kOutOfRange, "inode number out of range")
        .Ino(ino)
        .Metadata("inode");
  }

  // Sparse-inode filesystems have 64 logical slots per inode record but may
  // back only some of them. A hole slot has no inode cluster at all; report it
  // as not allocated instead of reading unrelated device bytes.
  if (geometry_.features.spinodes) {
    const uint32_t agno = geometry_.InoAgno(ino);
    const uint32_t agino = geometry_.InoAgino(ino);
    auto rec = LookupInodeRecord(*device_, geometry_, agno, agino);
    if (!rec.ok()) return rec.error();
    const uint32_t offset = agino - rec.value().startino;
    if (!((InobtAllocMask(rec.value().holemask) >> offset) & 1ULL)) {
      return XFS_ERR(XfsError::kNotFound,
                     "inode number is a sparse inode chunk hole")
          .Ino(ino)
          .Metadata("inode")
          .Actual(agino);
    }
  }

  // Read a whole 16 KiB inode chunk once; a directory full of files usually
  // has many inodes in the same chunk, so this cuts device reads ~32x.
  const uint64_t inode_off = geometry_.InoToByteOffset(ino);
  const uint64_t chunk_start = inode_off & ~(kInodeChunkBytes - 1);
  std::shared_ptr<const std::vector<uint8_t>> chunk;
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = inode_chunk_cache_.find(chunk_start);
    if (it != inode_chunk_cache_.end()) chunk = it->second;
  }
  if (!chunk) {
    const uint64_t dev_size = device_->Size();
    uint64_t len = kInodeChunkBytes;
    if (chunk_start + len > dev_size) {
      len = dev_size > chunk_start ? dev_size - chunk_start : 0;
    }
    if (len >= geometry_.inode_size) {
      auto buf = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(len));
      if (!device_->Read(chunk_start, buf->data(), buf->size())) {
        return XFS_ERR(XfsError::kIoError, "failed to read inode")
            .Ino(ino)
            .Offset(chunk_start)
            .Metadata("inode");
      }
      {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        constexpr size_t kMaxChunks = 4096;  // 64 MiB of inode chunks
        if (inode_chunk_cache_.size() >= kMaxChunks) inode_chunk_cache_.clear();
        inode_chunk_cache_.emplace(chunk_start, buf);
      }
      chunk = std::move(buf);
    }
  }

  const uint64_t local = inode_off - chunk_start;
  if (chunk && local + geometry_.inode_size <= chunk->size()) {
    XfsInode out;
    Status s = ParseInode(chunk->data() + local,
                          static_cast<size_t>(geometry_.inode_size),
                          geometry_, ino, &out);
    if (!s.ok()) return s.error();
    return finish(std::move(out));
  }

  auto inode = ReadInode(*device_, geometry_, ino);
  if (!inode.ok()) return inode.error();
  return finish(std::move(inode.value()));
}

void XfsFilesystem::CacheDirectory(uint64_t ino,
                                   std::vector<XfsDirEntry> entries) const {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  constexpr size_t kMaxCachedDirs = 128;
  constexpr size_t kMaxCachedDirEntries = 262144;
  if (entries.size() > kMaxCachedDirEntries) return;
  auto existing = dir_cache_.find(ino);
  if (existing != dir_cache_.end()) {
    dir_cache_entries_ -= existing->second.entries.size();
    dir_cache_.erase(existing);
  }
  // Bounded LRU (clock) eviction instead of the old cache-cliff clear: a
  // workload that touches more than 128 directories keeps its hot entries.
  while (!dir_cache_.empty() &&
         (dir_cache_.size() >= kMaxCachedDirs ||
          dir_cache_entries_ + entries.size() > kMaxCachedDirEntries)) {
    auto victim = dir_cache_.begin();
    for (auto it = dir_cache_.begin(); it != dir_cache_.end(); ++it) {
      if (it->second.last_used < victim->second.last_used) victim = it;
    }
    dir_cache_entries_ -= victim->second.entries.size();
    dir_cache_.erase(victim);
  }
  DirCache cache;
  cache.index.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    cache.index.emplace(entries[i].name, i);
  }
  cache.entries = std::move(entries);
  cache.last_used = ++dir_cache_clock_;
  dir_cache_entries_ += cache.entries.size();
  dir_cache_.emplace(ino, std::move(cache));
}

void XfsFilesystem::CacheDirectoryAdd(uint64_t ino, std::string_view name,
                                      uint64_t child_ino,
                                      uint8_t ftype) const {
  constexpr size_t kMaxCachedDirEntries = 262144;
  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto it = dir_cache_.find(ino);
  if (it == dir_cache_.end()) return;  // cold: the next lookup loads it
  auto indexed = it->second.index.find(std::string(name));
  if (indexed != it->second.index.end()) {
    XfsDirEntry& entry = it->second.entries[indexed->second];
    entry.ino = child_ino;
    entry.ftype = ftype;
    it->second.last_used = ++dir_cache_clock_;
    return;
  }
  if (it->second.entries.size() >= kMaxCachedDirEntries ||
      dir_cache_entries_ + 1 > kMaxCachedDirEntries) {
    // Cannot represent the added entry without exceeding the budget. Drop the
    // cache entry rather than leaving a false negative in the lookup index.
    dir_cache_entries_ -= it->second.entries.size();
    dir_cache_.erase(it);
    return;
  }
  it->second.entries.push_back(
      XfsDirEntry{std::string(name), child_ino, ftype});
  it->second.index.emplace(std::string(name), it->second.entries.size() - 1);
  it->second.last_used = ++dir_cache_clock_;
  ++dir_cache_entries_;
}

void XfsFilesystem::CacheDirectoryRemove(uint64_t ino,
                                         std::string_view name) const {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto it = dir_cache_.find(ino);
  if (it == dir_cache_.end()) return;
  auto indexed = it->second.index.find(std::string(name));
  if (indexed == it->second.index.end()) return;
  const size_t position = indexed->second;
  it->second.entries.erase(it->second.entries.begin() + position);
  // Positions after the removed entry shift down. A directory with 50k
  // entries makes this O(n) CPU work, but it avoids the much more expensive
  // O(n) device re-read and keeps the cache authoritative.
  it->second.index.clear();
  it->second.index.reserve(it->second.entries.size());
  for (size_t i = 0; i < it->second.entries.size(); ++i) {
    it->second.index.emplace(it->second.entries[i].name, i);
  }
  it->second.last_used = ++dir_cache_clock_;
  --dir_cache_entries_;
}

Result<XfsInode> XfsFilesystem::LookupPath(std::string_view path) const {
  uint64_t ino = geometry_.rootino;
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();

  size_t pos = 0;
  while (pos < path.size()) {
    while (pos < path.size() && path[pos] == '/') ++pos;
    if (pos >= path.size()) break;
    size_t end = path.find('/', pos);
    if (end == std::string_view::npos) end = path.size();
    std::string_view component = path.substr(pos, end - pos);
    pos = end;

    if (component == ".") continue;
    if (!inode.value().IsDir()) {
      return XFS_ERR(XfsError::kNotDirectory, "path component is not a "
                                             "directory")
          .Ino(inode.value().ino);
    }
    auto entry = LookupName(inode.value().ino, component);
    if (!entry.ok()) return entry.error();
    auto next = OpenInode(entry.value().ino);
    if (!next.ok()) return next.error();
    inode = std::move(next);
  }
  return inode;
}

Result<std::vector<XfsDirEntry>> XfsFilesystem::ListDirectory(
    uint64_t ino) const {
  PerfStats& perf = PerfStats::Get();
  const bool perf_on = perf.enabled();
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = dir_cache_.find(ino);
    if (it != dir_cache_.end()) {
      it->second.last_used = ++dir_cache_clock_;
      if (perf_on) {
        perf.directory_cache_hits.fetch_add(1, std::memory_order_relaxed);
      }
      return it->second.entries;
    }
  }
  if (perf_on) {
    perf.directory_cache_misses.fetch_add(1, std::memory_order_relaxed);
  }

  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  if (!inode.value().IsDir()) {
    return XFS_ERR(XfsError::kNotDirectory, "inode is not a directory")
        .Ino(ino);
  }
  std::vector<XfsDirEntry> entries;
  Status s = xfs::ListDirectory(
      *device_, geometry_, inode.value(),
      [&](const XfsDirEntry& e) -> Status {
        entries.push_back(e);
        return OkStatus();
      });
  if (!s.ok()) return s.error();
  CacheDirectory(ino, entries);
  return entries;
}

Result<XfsDirEntry> XfsFilesystem::LookupName(uint64_t dir_ino,
                                              std::string_view name) const {
  PerfStats& perf = PerfStats::Get();
  const bool perf_on = perf.enabled();
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = dir_cache_.find(dir_ino);
    if (it != dir_cache_.end()) {
      it->second.last_used = ++dir_cache_clock_;
      if (perf_on) {
        perf.directory_cache_hits.fetch_add(1, std::memory_order_relaxed);
      }
      auto indexed = it->second.index.find(std::string(name));
      if (indexed == it->second.index.end()) {
        return XFS_ERR(XfsError::kNotFound, "name not found in directory")
            .Ino(dir_ino);
      }
      return it->second.entries[indexed->second];
    }
  }
  if (perf_on) {
    perf.directory_cache_misses.fetch_add(1, std::memory_order_relaxed);
  }

  // Populate the full directory cache (bounded by CacheDirectory) instead of
  // doing a one-off targeted lookup. The common caller pattern is a namespace
  // loop -- create/unlink/stat repeatedly in the same directory -- and a
  // targeted lookup leaves the cache cold, making every iteration re-read the
  // directory and turning a large-directory create loop quadratic. A single
  // random lookup on a huge directory pays one bounded parse here and all
  // following lookups are hash hits.
  auto listed = ListDirectory(dir_ino);
  if (!listed.ok()) return listed.error();
  bool cache_complete = false;
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = dir_cache_.find(dir_ino);
    if (it != dir_cache_.end()) {
      cache_complete = true;
      auto indexed = it->second.index.find(std::string(name));
      if (indexed != it->second.index.end()) {
        if (perf_on) {
          perf.directory_cache_hits.fetch_add(1, std::memory_order_relaxed);
        }
        return it->second.entries[indexed->second];
      }
    }
  }
  if (cache_complete) {
    return XFS_ERR(XfsError::kNotFound, "name not found in directory")
        .Ino(dir_ino);
  }
  // The directory is larger than the bounded cache budget. Fall back to the
  // targeted on-disk lookup instead of reporting a false negative.
  auto inode = OpenInode(dir_ino);
  if (!inode.ok()) return inode.error();
  return xfs::LookupName(*device_, geometry_, inode.value(), name);
}

Result<size_t> XfsFilesystem::ReadFile(uint64_t ino, uint64_t offset,
                                       void* buffer, size_t length) const {
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  return ReadFile(inode.value(), nullptr, offset, buffer, length);
}

Result<size_t> XfsFilesystem::ReadFile(
    const XfsInode& inode, const std::vector<XfsExtent>* cached_extents,
    uint64_t offset, void* buffer, size_t length) const {
  PerfStats::ReadScope read_scope;
  PerfStats& perf = PerfStats::Get();
  const bool perf_on = perf.enabled();
  if (perf_on) perf.read_calls.fetch_add(1, std::memory_order_relaxed);
  if (inode.IsDir()) {
    return XFS_ERR(XfsError::kNotRegularFile, "cannot ReadFile a directory")
        .Ino(inode.ino);
  }

  if (inode.format == di::kFormatLocal) {
    return ReadFileData(*device_, geometry_, inode, offset, buffer, length);
  }

  std::shared_ptr<const std::vector<XfsExtent>> owned_extents;
  const std::vector<XfsExtent>* extents = cached_extents;
  if (!extents) {
    auto loaded = LoadExtentsCached(inode);
    if (!loaded.ok()) return loaded.error();
    owned_extents = std::move(loaded.value());
    extents = owned_extents.get();
  }
  // Large requests read straight into the caller's buffer: routing them
  // through the chunk cache would add a full memory copy per chunk and evict
  // cached thumbnail data.
  if (length >= kDataChunkBytes) {
    auto direct = ReadFileData(*device_, geometry_, inode, offset, buffer,
                               length, extents);
    if (direct.ok() && perf_on) {
      perf.read_bytes.fetch_add(direct.value(), std::memory_order_relaxed);
    }
    return direct;
  }

  // Serve from the 256 KiB chunk cache: a thumbnail decoder issues many small
  // reads against the same few files; the first read fetches a whole chunk
  // (read-ahead) and repeat reads are pure memory copies.
  uint8_t* out = static_cast<uint8_t*>(buffer);
  size_t total = 0;
  const uint64_t size = inode.size;
  while (total < length && offset < size) {
    const size_t want = static_cast<size_t>(
        std::min<uint64_t>(length - total, size - offset));
    const uint64_t chunk_index = offset / kDataChunkBytes;
    const uint64_t chunk_start = chunk_index * kDataChunkBytes;
    const uint64_t chunk_len =
        std::min<uint64_t>(kDataChunkBytes, size - chunk_start);

    // Snapshot the data generation before touching the device: a concurrent
    // write bumps it, which makes both this lookup and the later insert
    // refuse the chunk (see InsertDataChunk).
    const uint64_t generation = DataGeneration(inode.ino);
    auto data = LookupDataChunk(inode.ino, chunk_index);
    if (!data) {
      auto loaded =
          std::make_shared<AlignedBuffer>(static_cast<size_t>(chunk_len));
      auto read = ReadFileData(*device_, geometry_, inode, chunk_start,
                               loaded->data(), loaded->size(), extents);
      if (!read.ok()) return read.error();
      loaded->resize(read.value());
      data = InsertDataChunk(inode.ino, chunk_index, std::move(loaded),
                             generation);
    }

    const size_t in_chunk = static_cast<size_t>(offset - chunk_start);
    if (in_chunk >= data->size()) break;  // safety: never spin
    const size_t n = std::min<size_t>(want, data->size() - in_chunk);
    std::memcpy(out + total, data->data() + in_chunk, n);
    total += n;
    offset += n;
  }
  if (perf_on) {
    perf.read_bytes.fetch_add(total, std::memory_order_relaxed);
  }
  return total;
}

Result<std::shared_ptr<const std::vector<XfsExtent>>>
XfsFilesystem::LoadExtentsCached(const XfsInode& inode) const {
  PerfStats& perf = PerfStats::Get();
  const bool perf_on = perf.enabled();
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = extent_cache_.find(inode.ino);
    if (it != extent_cache_.end()) {
      if (perf_on) {
        perf.extent_cache_hits.fetch_add(1, std::memory_order_relaxed);
      }
      return it->second;
    }
  }
  if (perf_on) {
    perf.extent_cache_misses.fetch_add(1, std::memory_order_relaxed);
  }
  auto loaded = std::make_shared<std::vector<XfsExtent>>();
  Status s = LoadDataForkExtents(*device_, geometry_, inode, loaded.get());
  if (!s.ok()) return s.error();
  std::shared_ptr<const std::vector<XfsExtent>> shared(loaded);
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    constexpr size_t kMaxCachedExtentLists = 8192;
    if (extent_cache_.size() >= kMaxCachedExtentLists) extent_cache_.clear();
    // A concurrent mutation may have replaced the extent list between the
    // lookup above and the insert here; keep whichever entry is already there
    // (the invalidation that replaced it knows about the newer state).
    auto existing = extent_cache_.find(inode.ino);
    if (existing != extent_cache_.end()) return existing->second;
    extent_cache_.emplace(inode.ino, shared);
  }
  return shared;
}

void XfsFilesystem::MarkDeferredTimes(uint64_t ino,
                                      const XfsTimestamp& now) const {
  // Drop the cached XfsFileInfo first so GetFileInfo rebuilds it through the
  // OpenInode overlay. (Cache lock before the deferred lock, matching OpenInode.)
  {
    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
    info_cache_.erase(ino);
  }
  std::lock_guard<std::mutex> lock(deferred_mutex_);
  deferred_times_[ino] = now;
}

Status XfsFilesystem::CommitDeferredTimesUnderInodeLock(uint64_t ino) {
  XfsTimestamp now;
  {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    auto it = deferred_times_.find(ino);
    if (it == deferred_times_.end()) return OkStatus();
    now = it->second;
  }
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  auto txn = BeginTransaction();
  if (!txn.ok()) return txn.error();
  Status status = UpdateTimesInTransaction(txn.value(), ino,
                                           inode.value().atime, now, now);
  if (status.ok()) status = CommitTransaction(txn.value());
  if (!status.ok()) return status;
  {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_times_.erase(ino);
  }
  return OkStatus();
}

Status XfsFilesystem::CommitDeferredMetadata(uint64_t ino) {
  if (!writable_) return OkStatus();
  // Close is on the small-file hot path. Most closes have no deferred
  // timestamp; check before taking the barrier and inode stripe. A concurrent
  // write on another handle will publish its own timestamp at its boundary.
  {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    if (deferred_times_.find(ino) == deferred_times_.end()) {
      return OkStatus();
    }
  }
  std::shared_lock<std::shared_mutex> barrier(write_barrier_);
  TimedLockGuard inode_lock(InodeLock(ino));
  std::lock_guard<std::mutex> lock(mutation_mutex_);
  return CommitDeferredTimesUnderInodeLock(ino);
}

Status XfsFilesystem::CommitAllDeferredTimesLocked() {
  for (;;) {
    uint64_t ino = 0;
    {
      std::lock_guard<std::mutex> lock(deferred_mutex_);
      if (deferred_times_.empty()) break;
      ino = deferred_times_.begin()->first;
    }
    std::lock_guard<std::mutex> inode_lock(InodeLock(ino));
    std::lock_guard<std::mutex> lock(mutation_mutex_);
    Status status = CommitDeferredTimesUnderInodeLock(ino);
    if (!status.ok()) return status;
  }
  return OkStatus();
}

Status XfsFilesystem::CommitAllDeferredMetadata() {
  if (!writable_) return OkStatus();
  std::unique_lock<std::shared_mutex> barrier(write_barrier_);
  return CommitAllDeferredTimesLocked();
}

uint64_t XfsFilesystem::ExtentGeneration(uint64_t ino) const {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  auto it = extent_generation_.find(ino);
  return it == extent_generation_.end() ? 0 : it->second;
}

uint64_t XfsFilesystem::DataGeneration(uint64_t ino) const {
  std::lock_guard<std::mutex> lock(data_mutex_);
  auto it = data_generation_.find(ino);
  return it == data_generation_.end() ? 0 : it->second;
}

std::shared_ptr<const AlignedBuffer> XfsFilesystem::LookupDataChunk(
    uint64_t ino, uint64_t chunk) const {
  const DataCacheKey key{ino, chunk};
  std::lock_guard<std::mutex> lock(data_mutex_);
  auto it = data_cache_.find(key);
  if (it == data_cache_.end()) return nullptr;
  auto gen = data_generation_.find(ino);
  const uint64_t current = gen == data_generation_.end() ? 0 : gen->second;
  if (it->second.generation != current) {
    // Written after this chunk was read: drop it instead of serving it.
    data_cache_bytes_ -= it->second.data->size();
    data_lru_.erase(it->second.lru);
    data_cache_.erase(it);
    return nullptr;
  }
  data_lru_.splice(data_lru_.begin(), data_lru_, it->second.lru);
  it->second.lru = data_lru_.begin();
  return it->second.data;
}

std::shared_ptr<const AlignedBuffer> XfsFilesystem::InsertDataChunk(
    uint64_t ino, uint64_t chunk, std::shared_ptr<const AlignedBuffer> data,
    uint64_t generation) const {
  const DataCacheKey key{ino, chunk};
  std::lock_guard<std::mutex> lock(data_mutex_);
  auto gen = data_generation_.find(ino);
  const uint64_t current = gen == data_generation_.end() ? 0 : gen->second;
  if (generation != current) {
    // The data changed while this chunk was being read; publishing it would
    // put stale bytes back into the cache. The caller still gets the buffer it
    // read (it is what the device held at some point during this call).
    return data;
  }
  auto existing = data_cache_.find(key);
  if (existing != data_cache_.end()) {
    if (existing->second.generation == current) {
      data_lru_.splice(data_lru_.begin(), data_lru_, existing->second.lru);
      existing->second.lru = data_lru_.begin();
      return existing->second.data;
    }
    data_cache_bytes_ -= existing->second.data->size();
    data_lru_.erase(existing->second.lru);
    data_cache_.erase(existing);
  }
  data_lru_.push_front(key);
  data_cache_bytes_ += data->size();
  data_cache_.emplace(key, DataCacheEntry{data, current, data_lru_.begin()});
  while (data_cache_bytes_ > kDataCacheMaxBytes && !data_lru_.empty()) {
    const DataCacheKey victim = data_lru_.back();
    data_lru_.pop_back();
    auto victim_it = data_cache_.find(victim);
    if (victim_it != data_cache_.end()) {
      data_cache_bytes_ -= victim_it->second.data->size();
      data_cache_.erase(victim_it);
    }
  }
  return data;
}

void XfsFilesystem::DropDataChunks(uint64_t ino, uint64_t byte_offset,
                                   uint64_t byte_length, bool drop_all) const {
  std::lock_guard<std::mutex> lock(data_mutex_);
  // Bumping the generation is what actually makes older chunks unusable; the
  // erase below is hygiene (it frees memory and forces a fresh device read).
  // Callers must have written the device *before* calling this.
  ++data_generation_[ino];
  const uint64_t current = data_generation_[ino];
  const uint64_t first_chunk = byte_offset / kDataChunkBytes;
  const uint64_t last_chunk =
      byte_length == 0
          ? UINT64_MAX
          : (byte_offset + byte_length - 1) / kDataChunkBytes;
  for (auto it = data_cache_.begin(); it != data_cache_.end();) {
    const DataCacheKey& key = it->first;
    if (key.ino != ino) {
      ++it;
      continue;
    }
    const bool in_range = drop_all || byte_length == 0 ||
                          (key.chunk >= first_chunk && key.chunk <= last_chunk);
    if (!in_range && it->second.generation == current) {
      ++it;
      continue;
    }
    data_cache_bytes_ -= it->second.data->size();
    data_lru_.erase(it->second.lru);
    it = data_cache_.erase(it);
  }
}

Result<std::string> XfsFilesystem::ReadSymlink(uint64_t ino) const {
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  if (!inode.value().IsSymlink()) {
    return XFS_ERR(XfsError::kNotSymlink, "inode is not a symlink").Ino(ino);
  }
  return ReadSymlinkTarget(*device_, geometry_, inode.value());
}


Result<XfsFileInfo> XfsFilesystem::GetFileInfo(uint64_t ino) const {
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = info_cache_.find(ino);
    if (it != info_cache_.end()) return it->second;
  }

  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  const XfsInode& i = inode.value();
  XfsFileInfo info;
  info.ino = i.ino;
  info.mode = i.mode;
  info.uid = i.uid;
  info.gid = i.gid;
  info.projid = i.projid;
  info.nlink = i.nlink;
  info.size = i.size;
  info.nblocks = i.nblocks;
  info.nextents = i.nextents;
  info.anextents = i.anextents;
  info.atime = i.atime;
  info.mtime = i.mtime;
  info.ctime = i.ctime;
  info.crtime = i.crtime;
  info.di_flags = i.flags;
  info.immutable = (i.flags & di::kDiFlagImmutable) != 0;
  info.append_only = (i.flags & di::kDiFlagAppend) != 0;

  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    constexpr size_t kMaxCachedInfos = 32768;
    if (info_cache_.size() >= kMaxCachedInfos) info_cache_.clear();
    info_cache_.emplace(ino, info);
  }
  return info;
}

// ---------------------------------------------------------------------------
// Consistency check
// ---------------------------------------------------------------------------

namespace {

struct FreeSpaceCounts {
  uint64_t blocks = 0;
  uint32_t longest = 0;
  uint64_t records = 0;
};

struct InodeCounts {
  uint64_t count = 0;
  uint64_t freecount = 0;
  uint64_t records = 0;
};

// Counts the bmbt blocks reachable from an in-fork root.  Shared by the data
// and attribute forks; only the fork bytes/root counters differ.
Status CountForkBmbtBlocks(BlockDevice& dev, const XfsGeometry& geo,
                           uint64_t ino, const std::vector<uint8_t>& fork,
                           uint16_t root_level, uint16_t root_numrecs,
                           const char* what, uint64_t* out,
                           std::vector<uint64_t>* block_list = nullptr) {
  *out = 0;
  if (root_level == 0) return OkStatus();
  if (fork.size() < bt::kBmdrSize) {
    return XFS_ERR(XfsError::kCorruptInode, std::string(what) + " root too small")
        .Ino(ino)
        .Metadata("inode");
  }
  const size_t node_maxrecs =
      (fork.size() - bt::kBmdrSize) /
      (bt::kBmbtKeySize + bt::kBmbtPtrSize);
  BtreeSpec spec;
  spec.name = what;
  spec.format = BtreeFormat::kLong;
  spec.block_size = geo.block_size;
  spec.key_size = bt::kBmbtKeySize;
  spec.ptr_size = bt::kBmbtPtrSize;
  spec.rec_size = bt::kBmbtRecSize;
  spec.magic = ondisk::kBmapCrcMagic;
  spec.max_depth = bt::kMaxTraversalLevels;
  spec.null_ptr = ~0ull;
  spec.expected_owner = ino;
  spec.owner_size = 8;
  spec.uuid = geo.meta_uuid;
  spec.ptr_to_daddr = [&](uint64_t fsb, uint64_t* daddr) {
    if (!geo.FsbValid(fsb)) return false;
    *daddr = geo.FsbToByte(fsb) / 512;
    return true;
  };
  uint64_t bmbt_blocks = 0;
  BtreeWalkCallbacks cb;
  cb.read_block = [&](uint64_t fsb, std::vector<uint8_t>* buf) {
    if (!geo.FsbValid(fsb)) return false;
    buf->resize(geo.block_size);
    return dev.Read(geo.FsbToByte(fsb), buf->data(), buf->size());
  };
  cb.ptr_in_range = [&](uint64_t ptr) {
    return ptr != spec.null_ptr && geo.FsbValid(ptr);
  };
  cb.visit = [&](uint64_t fsb, const BtreeBlockMeta&,
                 const uint8_t*) -> Status {
    ++bmbt_blocks;
    if (block_list != nullptr) block_list->push_back(fsb);
    return OkStatus();
  };
  std::vector<uint64_t> children;
  for (uint16_t i = 0; i < root_numrecs; ++i) {
    const uint64_t ptr =
        GetBE64(fork.data(), bt::kBmdrSize + node_maxrecs * 8 + i * 8);
    if (ptr == spec.null_ptr) {
      return XFS_ERR(XfsError::kCorruptInode,
                     std::string(what) + " root has a null child")
          .Ino(ino)
          .Metadata("inode");
    }
    children.push_back(ptr);
  }
  Status s = WalkBtreeForest(spec, children,
                             static_cast<uint16_t>(root_level - 1), cb);
  if (!s.ok()) return s;
  *out = bmbt_blocks;
  return OkStatus();
}

// Cross-checks every attribute-fork data extent against rmapbt.  DA blocks and
// remote values use OWN_INODE + ATTR_FORK + logical offset; a missing record or
// a record owned by another logical block is corruption.
Status CheckAttrRmap(BlockDevice& dev, const XfsGeometry& geo,
                     const XfsInode& inode, const XattrFork& fork,
                     uint64_t sb_lsn) {
  if (!geo.features.rmapbt) return OkStatus();
  std::map<uint32_t, RmapTree> trees;
  auto lookup_rmap = [&](uint64_t fsb, uint32_t flags, uint64_t logical,
                         const char* what) -> Status {
    const uint32_t agno = static_cast<uint32_t>(fsb >> geo.agblklog);
    const uint32_t agbno = static_cast<uint32_t>(
        fsb & ((1ULL << geo.agblklog) - 1));
    auto it = trees.find(agno);
    if (it == trees.end()) {
      auto loaded = LoadRmapTree(dev, geo, agno, sb_lsn);
      if (!loaded.ok()) return loaded.error();
      it = trees.emplace(agno, std::move(loaded.value())).first;
    }
    const RmapRecord* covering = nullptr;
    for (const RmapRecord& rec : it->second.records) {
      if (rec.key.startblock > agbno) break;
      if (agbno >= rec.key.startblock && agbno < rec.EndBlock()) {
        covering = &rec;
      }
    }
    if (covering == nullptr) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     std::string(what) + " has no reverse mapping")
          .Ino(inode.ino)
          .Ag(agno)
          .Agbno(agbno)
          .Metadata("rmap");
    }
    if (covering->key.owner != inode.ino ||
        (covering->key.flags & kRmapAttrFork) == 0 ||
        covering->unwritten()) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     std::string(what) +
                         " reverse mapping has the wrong owner/flags")
          .Ino(inode.ino)
          .Ag(agno)
          .Agbno(agbno)
          .Metadata("rmap")
          .Expected(inode.ino)
          .Actual(covering->key.owner);
    }
    uint64_t expected_offset = 0;
    if (flags == kRmapBmbtBlock) {
      expected_offset = 0;
    } else {
      expected_offset =
          covering->key.offset + (agbno - covering->key.startblock);
    }
    if (expected_offset != logical) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     std::string(what) +
                         " reverse mapping has the wrong offset")
          .Ino(inode.ino)
          .Ag(agno)
          .Agbno(agbno)
          .Metadata("rmap")
          .Expected(logical)
          .Actual(expected_offset);
    }
    return OkStatus();
  };

  for (const XfsExtent& e : fork.extents) {
    for (uint64_t i = 0; i < e.blockcount; ++i) {
      const uint64_t logical = e.startoff + i;
      Status s = lookup_rmap(e.startblock + i, kRmapAttrFork, logical,
                             "attr fork block");
      if (!s.ok()) return s;
    }
  }
  if (inode.aformat == di::kFormatBtree) {
    std::vector<uint64_t> bmbt;
    uint64_t count = 0;
    Status s = CountForkBmbtBlocks(
        dev, geo, inode.ino, inode.attr_fork,
        GetBE16(inode.attr_fork.data(), 0), GetBE16(inode.attr_fork.data(), 2),
        "attr bmbt", &count, &bmbt);
    if (!s.ok()) return s;
    for (uint64_t fsb : bmbt) {
      s = lookup_rmap(fsb, kRmapAttrFork | kRmapBmbtBlock, 0,
                      "attr bmbt block");
      if (!s.ok()) return s;
    }
  }
  return OkStatus();
}

// Recomputes di_nblocks from both forks (data/attr extents plus their bmbt
// blocks) and verifies the attribute fork at the same time.  This is the value
// xfs_repair checks.
Status ExpectedInodeBlocks(BlockDevice& dev, const XfsGeometry& geo,
                           const XfsInode& inode, uint64_t* out) {
  uint64_t blocks = 0;
  if (inode.format != di::kFormatLocal && inode.format != di::kFormatDev &&
      inode.format != di::kFormatUuid) {
    std::vector<XfsExtent> extents;
    Status s = LoadDataForkExtents(dev, geo, inode, &extents);
    if (!s.ok()) return s;
    for (const XfsExtent& e : extents) blocks += e.blockcount;
    if (inode.format == di::kFormatBtree) {
      if (inode.data_fork.size() < bt::kBmdrSize) {
        return XFS_ERR(XfsError::kCorruptInode, "bmbt fork too small")
            .Ino(inode.ino)
            .Metadata("inode");
      }
      uint64_t bmbt = 0;
      s = CountForkBmbtBlocks(dev, geo, inode.ino, inode.data_fork,
                              GetBE16(inode.data_fork.data(), 0),
                              GetBE16(inode.data_fork.data(), 2), "bmbt",
                              &bmbt);
      if (!s.ok()) return s;
      blocks += bmbt;
    }
  }
  if (inode.HasAttrFork()) {
    auto attr = ReadXattrFork(dev, geo, inode);
    if (!attr.ok()) return attr.error();
    for (const XfsExtent& e : attr.value().extents) {
      blocks += e.blockcount;
    }
    if (inode.aformat == di::kFormatBtree) {
      uint64_t bmbt = 0;
      Status s = CountForkBmbtBlocks(dev, geo, inode.ino, inode.attr_fork,
                                     GetBE16(inode.attr_fork.data(), 0),
                                     GetBE16(inode.attr_fork.data(), 2),
                                     "attr bmbt", &bmbt);
      if (!s.ok()) return s;
      blocks += bmbt;
    }
  }
  *out = blocks;
  return OkStatus();
}

}  // namespace

Result<std::vector<uint64_t>> XfsFilesystem::ListAllocatedInodes() const {
  std::vector<uint64_t> out;
  const XfsGeometry& geo = geometry_;
  BlockDevice& dev = *device_;
  uint64_t sb_lsn = superblock_.lsn;
  auto live_sb = ReadSuperblock(dev, 0, /*verify_crc=*/true);
  if (live_sb.ok()) sb_lsn = live_sb.value().lsn;
  for (uint32_t agno = 0; agno < geo.agcount; ++agno) {
    auto headers = ReadAgHeaders(dev, geo, agno);
    if (!headers.ok()) return headers.error();
    const XfsAgf& agf = headers.value().agf;
    const XfsAgi& agi = headers.value().agi;

    BtreeSpec inobt;
    inobt.name = "inobt";
    inobt.format = BtreeFormat::kShort;
    inobt.block_size = geo.block_size;
    inobt.key_size = bt::kInobtKeySize;
    inobt.ptr_size = bt::kInobtPtrSize;
    inobt.rec_size = bt::kInobtRecSize;
    inobt.magic = ondisk::kIbtCrcMagic;
    inobt.null_ptr = kNullAgBlock;
    inobt.expected_owner = agno;
    inobt.owner_size = 4;
    inobt.uuid = geo.meta_uuid;
    inobt.sb_lsn = sb_lsn;
    inobt.ptr_to_daddr = [&](uint64_t agbno, uint64_t* daddr) {
      if (agbno >= agf.length) return false;
      *daddr = geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))) /
               512;
      return true;
    };
    BtreeWalkCallbacks cb;
    cb.read_block = [&](uint64_t agbno, std::vector<uint8_t>* buf) {
      if (agbno >= agf.length) return false;
      buf->resize(geo.block_size);
      return dev.Read(
          geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))),
          buf->data(), geo.block_size);
    };
    cb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
    cb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                   const uint8_t* buf) -> Status {
      if (meta.level != 0) return OkStatus();
      for (uint16_t i = 0; i < meta.numrecs; ++i) {
        const uint8_t* rec = BtreeLeafRec(buf, meta, inobt, i);
        InobtRecord r;
        r.startino = GetBE32(rec);
        r.free = GetBE64(rec + 8);
        if (geo.features.spinodes) {
          r.holemask = GetBE16(rec + 4);
          r.count = GetU8(rec + 6);
          r.freecount = GetU8(rec + 7);
        } else {
          r.holemask = 0;
          r.count = ondisk::kInodesPerChunk;
          r.freecount = GetBE32(rec + 4);
        }
        const uint64_t allocmask =
            geo.features.spinodes ? InobtAllocMask(r.holemask) : ~0ULL;
        for (uint32_t bit = 0; bit < ondisk::kInodesPerChunk; ++bit) {
          if (!((allocmask >> bit) & 1ULL)) continue;
          if ((r.free >> bit) & 1ULL) continue;
          out.push_back(geo.AginoToIno(agno, r.startino + bit));
        }
      }
      return OkStatus();
    };
    Status s = WalkBtree(inobt, agi.root, cb);
    if (!s.ok()) return s.error();
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

CheckReport XfsFilesystem::CheckConsistency(const CheckOptions& options) const {
  CheckReport report;
  const XfsGeometry& geo = geometry_;
  BlockDevice& dev = *device_;

  // Counters and the metadata LSN move with every mutation, so read the
  // primary superblock again instead of trusting the mount-time copy.
  XfsSuperblock live_sb = superblock_;
  auto fresh_sb = ReadSuperblock(dev, 0, /*verify_crc=*/true);
  if (fresh_sb.ok()) live_sb = fresh_sb.value();

  report.Add("superblock", CheckStatus::kOk,
             "magic/uuid/geometry OK, crc " +
                 std::string(live_sb.crc_verified ? "verified" : "skipped"));

  // Secondary superblocks.
  if (options.secondary_superblocks && geo.agcount > 1) {
    uint32_t mismatches = 0;
    uint32_t checked = 0;
    for (uint32_t agno = 1; agno < geo.agcount; ++agno) {
      uint64_t off = geo.AgByteOffset(agno);
      auto sb2 = ReadSuperblock(dev, off, /*verify_crc=*/true);
      if (!sb2.ok()) {
        report.Add("superblock AG" + std::to_string(agno),
                   CheckStatus::kCorrupt, sb2.error().Format());
        ++mismatches;
        continue;
      }
      ++checked;
      const XfsSuperblock& s = sb2.value();
      // Like xfs_repair's compare_sb: only the geometry fields must match.
      // Counters, version numbers, root inode, feature bits and other fields
      // may legitimately differ in secondary copies.
      bool same = s.blocksize == live_sb.blocksize &&
                  s.dblocks == live_sb.dblocks &&
                  s.rblocks == live_sb.rblocks &&
                  s.rextents == live_sb.rextents &&
                  s.logstart == live_sb.logstart &&
                  s.rextsize == live_sb.rextsize &&
                  s.agblocks == live_sb.agblocks &&
                  s.agcount == live_sb.agcount &&
                  s.rbmblocks == live_sb.rbmblocks &&
                  s.logblocks == live_sb.logblocks &&
                  s.sectsize == live_sb.sectsize &&
                  s.inodesize == live_sb.inodesize;
      if (!same) {
        report.Add("superblock AG" + std::to_string(agno),
                   CheckStatus::kWarning,
                   "secondary superblock differs from primary");
        ++mismatches;
      }
    }
    if (mismatches == 0) {
      report.Add("secondary superblocks", CheckStatus::kOk,
                 std::to_string(checked) + " copies verified");
    }
  }

  // Per-AG checks.
  uint64_t total_free = 0;
  uint64_t total_icount = 0;
  uint64_t total_ifree = 0;
  bool rmap_seen = false;
  bool rmap_clean = true;
  bool refcount_seen = false;
  bool refcount_clean = true;
  uint64_t refcount_cow_records = 0;
  if (options.allocation_groups) {
    for (uint32_t agno = 0; agno < geo.agcount; ++agno) {
      auto headers = ReadAgHeaders(dev, geo, agno);
      if (!headers.ok()) {
        report.Add("AG" + std::to_string(agno), CheckStatus::kCorrupt,
                   headers.error().Format());
        continue;
      }
      const XfsAgf& agf = headers.value().agf;
      const XfsAgi& agi = headers.value().agi;
      // sb_fdblocks includes AGFL-reserved blocks and the non-root free space
      // btree blocks (the kernel's fdblocks = freeblks + flcount + btreeblks
      // identity, also used by xfs_initialize_perag_data and xfs_repair).
      total_free += agf.freeblks + agf.flcount + agf.btreeblks;
      total_icount += agi.count;
      total_ifree += agi.freecount;

      if (options.btrees) {
        // bnobt: enumerate free space, validate ordering and totals.
        BtreeSpec bnobt;
        bnobt.name = "bnobt";
        bnobt.format = BtreeFormat::kShort;
        bnobt.block_size = geo.block_size;
        bnobt.key_size = bt::kAllocKeySize;
        bnobt.ptr_size = bt::kAllocPtrSize;
        bnobt.rec_size = bt::kAllocRecSize;
        bnobt.magic = ondisk::kBtbCrcMagic;
        bnobt.null_ptr = kNullAgBlock;
        bnobt.expected_owner = agno;
        bnobt.owner_size = 4;
        bnobt.uuid = geo.meta_uuid;
        bnobt.sb_lsn = live_sb.lsn;
        bnobt.ptr_to_daddr = [&](uint64_t agbno, uint64_t* daddr) {
          if (agbno >= agf.length) return false;
          *daddr = geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))) / 512;
          return true;
        };

        auto read_ag_block = [&](uint64_t agbno, std::vector<uint8_t>* buf) {
          if (agbno >= agf.length) return false;
          buf->resize(geo.block_size);
          return dev.Read(geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))),
                          buf->data(), geo.block_size);
        };

        FreeSpaceCounts bno;
        std::vector<std::pair<uint32_t, uint32_t>> free_ranges;
        uint32_t last_end = 0;
        bool first = true;
        uint32_t bno_blocks = 0;
        BtreeWalkCallbacks cb;
        cb.read_block = read_ag_block;
        cb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
        cb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                       const uint8_t* buf) -> Status {
          ++bno_blocks;
          if (meta.level != 0) return OkStatus();
          for (uint16_t i = 0; i < meta.numrecs; ++i) {
            const uint8_t* rec = BtreeLeafRec(buf, meta, bnobt, i);
            uint32_t start = GetBE32(rec);
            uint32_t len = GetBE32(rec + 4);
            if (len == 0 || start >= agf.length ||
                len > agf.length - start) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "invalid free space record")
                  .Ag(agno)
                  .Metadata("bnobt")
                  .Actual(start);
            }
            if (!first && start <= last_end) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "free space records out of order/overlapping")
                  .Ag(agno)
                  .Metadata("bnobt")
                  .Expected(last_end)
                  .Actual(start);
            }
            first = false;
            last_end = start + len - 1;
            free_ranges.push_back({start, len});
            bno.blocks += len;
            bno.longest = std::max(bno.longest, len);
            ++bno.records;
          }
          return OkStatus();
        };
        Status s = WalkBtree(bnobt, agf.bno_root, cb);
        const bool bno_ok = s.ok();
        if (!s.ok()) {
          report.Add("AG" + std::to_string(agno) + " bnobt",
                     CheckStatus::kCorrupt, s.error().Format());
        } else {
          if (bno.blocks != agf.freeblks) {
            report.Add("AG" + std::to_string(agno) + " free space",
                       CheckStatus::kCorrupt,
                       "bnobt total " + std::to_string(bno.blocks) +
                           " != AGF freeblks " + std::to_string(agf.freeblks));
          }
          if (bno.longest != agf.longest) {
            report.Add("AG" + std::to_string(agno) + " free space",
                       CheckStatus::kCorrupt,
                       "bnobt longest " + std::to_string(bno.longest) +
                           " != AGF longest " + std::to_string(agf.longest));
          }
          if (bno.blocks == agf.freeblks && bno.longest == agf.longest) {
            report.Add("AG" + std::to_string(agno) + " bnobt",
                       CheckStatus::kOk,
                       std::to_string(bno.records) + " free extents, " +
                           std::to_string(bno.blocks) + " blocks");
          }
        }

        // cntbt: same records, ordered by block count.
        BtreeSpec cnt = bnobt;
        cnt.name = "cntbt";
        cnt.magic = ondisk::kBtcCrcMagic;
        FreeSpaceCounts cnt_counts;
        uint32_t last_count = 0;
        bool cnt_first = true;
        uint32_t cnt_blocks = 0;
        BtreeWalkCallbacks ccb;
        ccb.read_block = read_ag_block;
        ccb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
        ccb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                        const uint8_t* buf) -> Status {
          ++cnt_blocks;
          if (meta.level != 0) return OkStatus();
          for (uint16_t i = 0; i < meta.numrecs; ++i) {
            const uint8_t* rec = BtreeLeafRec(buf, meta, cnt, i);
            uint32_t start = GetBE32(rec);
            uint32_t len = GetBE32(rec + 4);
            if (len == 0 || start >= agf.length || len > agf.length - start) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "invalid cntbt record")
                  .Ag(agno)
                  .Metadata("cntbt")
                  .Actual(start);
            }
            if (!cnt_first && len < last_count) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "cntbt records out of order")
                  .Ag(agno)
                  .Metadata("cntbt")
                  .Expected(last_count)
                  .Actual(len);
            }
            cnt_first = false;
            last_count = len;
            cnt_counts.blocks += len;
            ++cnt_counts.records;
          }
          return OkStatus();
        };
        s = WalkBtree(cnt, agf.cnt_root, ccb);
        const bool cnt_ok = s.ok();
        if (!s.ok()) {
          report.Add("AG" + std::to_string(agno) + " cntbt",
                     CheckStatus::kCorrupt, s.error().Format());
        } else if (cnt_counts.blocks != bno.blocks ||
                   cnt_counts.records != bno.records) {
          report.Add("AG" + std::to_string(agno) + " cntbt",
                     CheckStatus::kCorrupt,
                     "cntbt total " + std::to_string(cnt_counts.blocks) +
                         " != bnobt total " + std::to_string(bno.blocks));
        }

        // agf_btreeblks is the number of *non-root* blocks of the free space
        // btrees plus, when the rmapbt is enabled, the non-root rmap blocks
        // (xchk_agf_xref_btreeblks and xfs_repair count exactly these; the
        // refcount tree is not part of it, see the comment in xfs_ag.c). The
        // root blocks are represented by the trees themselves, and the AGFL
        // blocks are tracked by agf_flcount instead.
        uint32_t rmap_blocks = 0;
        bool rmap_ok = true;
        if (live_sb.features.rmapbt) {
          rmap_seen = true;
          // The generic WalkBtree cannot decode the overlapping tree's
          // interleaved low/high key pairs. The typed reader also validates
          // both child keys, record ownership and the AGF block counter.
          auto tree = LoadRmapTree(dev, geo, agno, live_sb.lsn);
          if (!tree.ok()) {
            report.Add("AG" + std::to_string(agno) + " rmapbt",
                       CheckStatus::kCorrupt, tree.error().Format());
            rmap_ok = false;
            rmap_clean = false;
          } else {
            rmap_blocks = tree.value().blocks;
            report.Add("AG" + std::to_string(agno) + " rmapbt",
                       CheckStatus::kOk,
                       "level " + std::to_string(tree.value().level) + ", " +
                           std::to_string(rmap_blocks) + " blocks, " +
                           std::to_string(tree.value().records.size()) +
                           " records (structurally verified)");
          }
        }
        if (live_sb.features.reflink) {
          refcount_seen = true;
          auto tree = LoadRefcountTree(dev, geo, agno, live_sb.lsn);
          if (!tree.ok()) {
            report.Add("AG" + std::to_string(agno) + " refcountbt",
                       CheckStatus::kCorrupt, tree.error().Format());
            refcount_clean = false;
          } else {
            uint64_t cow_records = 0;
            uint64_t shared_records = 0;
            for (const RefcountRecord& rec : tree.value().records) {
              if (rec.cow()) {
                ++cow_records;
              } else {
                ++shared_records;
              }
            }
            refcount_cow_records += cow_records;
            report.Add("AG" + std::to_string(agno) + " refcountbt",
                       CheckStatus::kOk,
                       "level " + std::to_string(tree.value().level) + ", " +
                           std::to_string(tree.value().blocks) + " blocks, " +
                           std::to_string(shared_records) +
                           " shared + " + std::to_string(cow_records) +
                           " CoW records (structurally verified)");
            if (cow_records != 0) {
              report.Add("AG" + std::to_string(agno) + " CoW staging",
                         CheckStatus::kWarning,
                         std::to_string(cow_records) +
                             " orphan CoW staging extent(s); a writable mount "
                             "will recover them");
            }
          }
        }
        if (bno_ok && cnt_ok && rmap_ok) {
          const uint32_t counted_btreeblks =
              (bno_blocks - 1) + (cnt_blocks - 1) +
              (rmap_blocks > 0 ? rmap_blocks - 1 : 0);
          if (agf.btreeblks != counted_btreeblks) {
            report.Add("AG" + std::to_string(agno) + " btree blocks",
                       CheckStatus::kCorrupt,
                       "agf_btreeblks " + std::to_string(agf.btreeblks) +
                           " != counted " +
                           std::to_string(counted_btreeblks));
          }
        }

        // inobt.
        BtreeSpec inobt;
        inobt.name = "inobt";
        inobt.format = BtreeFormat::kShort;
        inobt.block_size = geo.block_size;
        inobt.key_size = bt::kInobtKeySize;
        inobt.ptr_size = bt::kInobtPtrSize;
        inobt.rec_size = bt::kInobtRecSize;
        inobt.magic = ondisk::kIbtCrcMagic;
        inobt.null_ptr = kNullAgBlock;
        inobt.expected_owner = agno;
        inobt.owner_size = 4;
        inobt.uuid = geo.meta_uuid;
        inobt.sb_lsn = live_sb.lsn;
        inobt.ptr_to_daddr = bnobt.ptr_to_daddr;

        InodeCounts ino;
        std::map<uint32_t, InobtRecord> inobt_records;
        uint32_t inobt_blocks = 0;
        uint32_t last_startino = 0;
        bool ino_first = true;
        BtreeWalkCallbacks icb;
        icb.read_block = read_ag_block;
        icb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
        icb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                        const uint8_t* buf) -> Status {
          ++inobt_blocks;
          if (meta.level != 0) return OkStatus();
          for (uint16_t i = 0; i < meta.numrecs; ++i) {
            const uint8_t* rec = BtreeLeafRec(buf, meta, inobt, i);
            InobtRecord r;
            r.startino = GetBE32(rec);
            r.free = GetBE64(rec + 8);
            if (geo.features.spinodes) {
              r.holemask = GetBE16(rec + 4);
              r.count = GetU8(rec + 6);
              r.freecount = GetU8(rec + 7);
            } else {
              r.holemask = 0;
              r.count = ondisk::kInodesPerChunk;
              r.freecount = GetBE32(rec + 4);
            }
            if (static_cast<uint64_t>(r.startino) +
                        ondisk::kInodesPerChunk >
                    static_cast<uint64_t>(agf.length) * geo.inopblock) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "inobt record startino out of range")
                  .Ag(agno)
                  .Metadata("inobt")
                  .Actual(r.startino);
            }
            const uint64_t allocmask = geo.features.spinodes
                                           ? InobtAllocMask(r.holemask)
                                           : ~0ULL;
            if (geo.features.spinodes) {
              uint32_t chunk_count = 0;
              for (uint32_t biti = 0; biti < bt::kInobtHolemaskBits;
                   ++biti) {
                if (!(r.holemask & (1u << biti))) {
                  chunk_count += ondisk::kInodesPerHolemaskBit;
                }
              }
              if (r.count != chunk_count) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "sparse inode record count mismatch")
                    .Ag(agno)
                    .Metadata("inobt")
                    .Expected(chunk_count)
                    .Actual(r.count);
              }
            }
            uint32_t actual_free = 0;
            for (uint32_t ino_i = 0; ino_i < ondisk::kInodesPerChunk; ++ino_i) {
              if (!((allocmask >> ino_i) & 1ULL)) continue;
              if (r.free & (1ULL << ino_i)) ++actual_free;
            }
            if (actual_free != r.freecount) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "inobt freecount does not match the free mask")
                  .Ag(agno)
                  .Metadata("inobt")
                  .Expected(actual_free)
                  .Actual(r.freecount);
            }
            if (!ino_first && r.startino <= last_startino) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "inobt records out of order")
                  .Ag(agno)
                  .Metadata("inobt");
            }
            ino_first = false;
            last_startino = r.startino;
            ino.count += r.count;
            ino.freecount += r.freecount;
            ++ino.records;
            inobt_records.emplace(r.startino, r);
          }
          return OkStatus();
        };
        s = WalkBtree(inobt, agi.root, icb);
        if (!s.ok()) {
          report.Add("AG" + std::to_string(agno) + " inobt",
                     CheckStatus::kCorrupt, s.error().Format());
        } else if (ino.count != agi.count || ino.freecount != agi.freecount) {
          report.Add("AG" + std::to_string(agno) + " inobt",
                     CheckStatus::kCorrupt,
                     "inobt inodes " + std::to_string(ino.count) +
                         "/free " + std::to_string(ino.freecount) +
                         " != AGI " + std::to_string(agi.count) + "/" +
                         std::to_string(agi.freecount));
        } else {
          report.Add("AG" + std::to_string(agno) + " inobt",
                     CheckStatus::kOk,
                     std::to_string(ino.records) + " chunks, " +
                         std::to_string(ino.count) + " inodes");
        }

        // Sparse chunks: every backed block belongs to the inode chunk and
        // must not also be in the free-space tree; every backed inode image
        // must pass the inode verifier (hole slots have no such image).
        if (s.ok() && geo.features.spinodes) {
          auto overlaps_free = [&](uint32_t start, uint32_t len) {
            for (const auto& fr : free_ranges) {
              if (start < fr.first + fr.second && fr.first < start + len) {
                return true;
              }
            }
            return false;
          };
          uint32_t checked_inodes = 0;
          bool sparse_ok = true;
          for (const auto& entry : inobt_records) {
            const InobtRecord& r = entry.second;
            const uint32_t sagbno = r.startino >> geo.inopblog;
            uint32_t bit = 0;
            while (bit < bt::kInobtHolemaskBits) {
              if ((r.holemask >> bit) & 1u) {
                ++bit;
                continue;
              }
              const uint32_t run_start = bit;
              while (bit < bt::kInobtHolemaskBits &&
                     ((r.holemask >> bit) & 1u) == 0) {
                ++bit;
              }
              const uint32_t inode_offset =
                  run_start * ondisk::kInodesPerHolemaskBit;
              const uint32_t inode_count =
                  (bit - run_start) * ondisk::kInodesPerHolemaskBit;
              if (inode_offset % geo.inopblock != 0 ||
                  inode_count % geo.inopblock != 0) {
                report.Add("AG" + std::to_string(agno) + " sparse backing",
                           CheckStatus::kCorrupt,
                           "unaligned backing run in chunk " +
                               std::to_string(r.startino));
                sparse_ok = false;
                break;
              }
              const uint32_t agbno = sagbno + inode_offset / geo.inopblock;
              const uint32_t blocks = inode_count / geo.inopblock;
              if (overlaps_free(agbno, blocks)) {
                report.Add("AG" + std::to_string(agno) + " sparse backing",
                           CheckStatus::kCorrupt,
                           "backed inode block " + std::to_string(agbno) +
                               " is in the free-space tree");
                sparse_ok = false;
              }
              for (uint32_t i = 0; i < inode_count; ++i) {
                const uint64_t backed_ino =
                    geo.AginoToIno(agno, r.startino + inode_offset + i);
                std::vector<uint8_t> image(geo.inode_size);
                if (!dev.Read(geo.InoToByteOffset(backed_ino), image.data(),
                              image.size())) {
                  report.Add("AG" + std::to_string(agno) + " sparse inode",
                             CheckStatus::kCorrupt,
                             "cannot read inode " +
                                 std::to_string(backed_ino));
                  sparse_ok = false;
                  break;
                }
                XfsInode parsed;
                Status ps = ParseInode(image.data(), image.size(), geo,
                                       backed_ino, &parsed,
                                       /*allow_free_inode=*/true);
                if (!ps.ok()) {
                  report.Add("AG" + std::to_string(agno) + " sparse inode",
                             CheckStatus::kCorrupt,
                             "inode " + std::to_string(backed_ino) + ": " +
                                 ps.error().Format());
                  sparse_ok = false;
                  break;
                }
                ++checked_inodes;
              }
              if (!sparse_ok) break;
            }
            if (!sparse_ok) break;
          }
          if (sparse_ok) {
            report.Add("AG" + std::to_string(agno) + " sparse backing",
                       CheckStatus::kOk,
                       std::to_string(checked_inodes) +
                           " backed inode images verified, holes unowned");
          }
        }

        // finobt: a filtered view of the inobt with an independent record
        // lifecycle. Every finobt record must match its inobt record, refer
        // to a real chunk with free inodes, and the recorded free mask must
        // account for the hole mask.
        uint32_t finobt_blocks = 0;
        if (geo.features.finobt) {
          BtreeSpec fibt = inobt;
          fibt.name = "finobt";
          fibt.magic = ondisk::kFibtCrcMagic;
          std::map<uint32_t, InobtRecord> free_records;
          BtreeWalkCallbacks fcb;
          fcb.read_block = read_ag_block;
          fcb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
          fcb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                          const uint8_t* buf) -> Status {
            ++finobt_blocks;
            if (meta.level != 0) return OkStatus();
            for (uint16_t i = 0; i < meta.numrecs; ++i) {
              const uint8_t* rec = BtreeLeafRec(buf, meta, fibt, i);
              InobtRecord f;
              f.startino = GetBE32(rec);
              f.free = GetBE64(rec + 8);
              if (geo.features.spinodes) {
                f.holemask = GetBE16(rec + 4);
                f.count = GetU8(rec + 6);
                f.freecount = GetU8(rec + 7);
              } else {
                f.holemask = 0;
                f.count = ondisk::kInodesPerChunk;
                f.freecount = GetBE32(rec + 4);
              }
              auto it = inobt_records.find(f.startino);
              if (it == inobt_records.end()) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "finobt record has no inobt record")
                    .Ag(agno)
                    .Metadata("finobt")
                    .Actual(f.startino);
              }
              const InobtRecord& r = it->second;
              if (f.holemask != r.holemask || f.count != r.count ||
                  f.free != r.free || f.freecount != r.freecount) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "finobt record does not match the inobt")
                    .Ag(agno)
                    .Metadata("finobt")
                    .Actual(f.startino);
              }
              if (f.freecount == 0) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "finobt tracks a fully allocated chunk")
                    .Ag(agno)
                    .Metadata("finobt")
                    .Actual(f.startino);
              }
              free_records.emplace(f.startino, f);
            }
            return OkStatus();
          };
          s = WalkBtree(fibt, agi.free_root, fcb);
          if (!s.ok()) {
            report.Add("AG" + std::to_string(agno) + " finobt",
                       CheckStatus::kCorrupt, s.error().Format());
          } else {
            bool missing = false;
            for (const auto& entry : inobt_records) {
              if (entry.second.freecount == 0) continue;
              if (free_records.find(entry.first) == free_records.end()) {
                report.Add("AG" + std::to_string(agno) + " finobt",
                           CheckStatus::kCorrupt,
                           "chunk " + std::to_string(entry.first) +
                               " has free inodes but no finobt record");
                missing = true;
                break;
              }
            }
            if (!missing) {
              report.Add("AG" + std::to_string(agno) + " finobt",
                         CheckStatus::kOk,
                         std::to_string(free_records.size()) +
                             " free chunks match the inobt");
            }
          }
        }

        // inobtcount: the AGI counters are the reachable block counts,
        // including each tree's root.
        if (geo.features.inobtcnt) {
          const bool ib_ok = agi.iblocks == inobt_blocks;
          const bool fb_ok =
              !geo.features.finobt || agi.fblocks == finobt_blocks;
          if (!ib_ok || !fb_ok) {
            report.Add("AG" + std::to_string(agno) + " inode btree blocks",
                       CheckStatus::kCorrupt,
                       "AGI iblocks/fblocks " + std::to_string(agi.iblocks) +
                           "/" + std::to_string(agi.fblocks) +
                           " != reachable " + std::to_string(inobt_blocks) +
                           "/" + std::to_string(finobt_blocks));
          } else {
            report.Add("AG" + std::to_string(agno) + " inode btree blocks",
                       CheckStatus::kOk,
                       std::to_string(inobt_blocks) + "/" +
                           std::to_string(finobt_blocks) + " blocks");
          }
        }
      }
    }
  }

  if (rmap_seen) {
    report.Add("rmapbt", rmap_clean ? CheckStatus::kOk : CheckStatus::kCorrupt,
               rmap_clean
                   ? "all AG reverse-mapping trees structurally clean"
                   : "one or more AG reverse-mapping trees failed");
  }
  if (refcount_seen) {
    report.Add("refcountbt",
               refcount_clean ? CheckStatus::kOk : CheckStatus::kCorrupt,
               refcount_clean
                   ? "all AG reference-count trees structurally clean"
                   : "one or more AG reference-count trees failed");
    if (refcount_clean && refcount_cow_records == 0) {
      report.Add("CoW staging", CheckStatus::kOk,
                 "no orphan CoW staging extents");
    }
  }

  if (options.allocation_groups) {
    if (total_free != live_sb.fdblocks) {
      report.Add("superblock counters", CheckStatus::kWarning,
                 "sum of AG free blocks " + std::to_string(total_free) +
                     " != sb_fdblocks " + std::to_string(live_sb.fdblocks) +
                     (live_sb.features2 & ondisk::kVersion2LazySbCount
                          ? " (lazy counter)"
                          : ""));
    } else {
      report.Add("superblock counters", CheckStatus::kOk,
                 "free block counters match");
    }
    if (total_icount != live_sb.icount ||
        total_ifree != live_sb.ifree) {
      report.Add("superblock inode counters", CheckStatus::kWarning,
                 "AGI inode counts " + std::to_string(total_icount) + "/" +
                     std::to_string(total_ifree) + " != sb " +
                     std::to_string(live_sb.icount) + "/" +
                     std::to_string(live_sb.ifree));
    } else {
      report.Add("superblock inode counters", CheckStatus::kOk,
                 "inode counters match");
    }
  }

  // Root inode + root directory.
  if (options.root_directory) {
    auto root = OpenInode(geo.rootino);
    if (!root.ok()) {
      report.Add("root inode", CheckStatus::kCorrupt, root.error().Format());
    } else {
      report.Add("root inode", CheckStatus::kOk,
                 "inode " + std::to_string(geo.rootino) + " verified");
      auto root_attrs = ReadXattrFork(dev, geo, root.value(), live_sb.lsn);
      if (!root_attrs.ok()) {
        report.Add("root inode xattrs", CheckStatus::kCorrupt,
                   root_attrs.error().Format());
      } else {
        report.Add("root inode xattrs", CheckStatus::kOk,
                   std::to_string(root_attrs.value().entries.size()) +
                       " entries, format " +
                       [&] {
                         switch (root_attrs.value().format) {
                           case XattrForkFormat::kNone: return "none";
                           case XattrForkFormat::kShortform: return "shortform";
                           case XattrForkFormat::kLeaf: return "leaf";
                           case XattrForkFormat::kNode: return "node";
                           case XattrForkFormat::kExtentsEmpty:
                             return "empty-extents";
                         }
                         return "unknown";
                       }());
        Status root_attr_rmap = CheckAttrRmap(
            dev, geo, root.value(), root_attrs.value(), live_sb.lsn);
        if (!root_attr_rmap.ok()) {
          report.Add("root inode attr rmap", CheckStatus::kCorrupt,
                     root_attr_rmap.error().Format());
        }
      }
      auto entries = ListDirectory(geo.rootino);
      if (!entries.ok()) {
        report.Add("root directory", CheckStatus::kCorrupt,
                   entries.error().Format());
      } else {
        report.Add("root directory", CheckStatus::kOk,
                   std::to_string(entries.value().size()) + " entries");
        if (options.root_children) {
          uint32_t bad = 0;
          uint32_t checked_attrs = 0;
          for (const auto& e : entries.value()) {
            if (e.name == "." || e.name == "..") continue;
            auto inode = OpenInode(e.ino);
            if (!inode.ok()) {
              report.Add("root entry '" + e.name + "'", CheckStatus::kCorrupt,
                         inode.error().Format());
              ++bad;
              if (bad > 8) break;
              continue;
            }
            auto child_attrs =
                ReadXattrFork(dev, geo, inode.value(), live_sb.lsn);
            if (!child_attrs.ok()) {
              report.Add("root entry '" + e.name + "' xattrs",
                         CheckStatus::kCorrupt,
                         child_attrs.error().Format());
              ++bad;
            } else if (child_attrs.value().HasIncomplete()) {
              report.Add("root entry '" + e.name + "' xattrs",
                         CheckStatus::kWarning,
                         "attribute fork contains an INCOMPLETE entry");
            }
            if (child_attrs.ok()) {
              ++checked_attrs;
              Status rmap_check = CheckAttrRmap(
                  dev, geo, inode.value(), child_attrs.value(), live_sb.lsn);
              if (!rmap_check.ok()) {
                report.Add("root entry '" + e.name + "' attr rmap",
                           CheckStatus::kCorrupt,
                           rmap_check.error().Format());
                ++bad;
              }
            }
            uint64_t expected_blocks = 0;
            Status blocks = ExpectedInodeBlocks(dev, geo, inode.value(),
                                                &expected_blocks);
            if (!blocks.ok()) {
              report.Add("root entry '" + e.name + "' nblocks",
                         CheckStatus::kCorrupt, blocks.error().Format());
              ++bad;
            } else if (inode.value().nblocks != expected_blocks) {
              report.Add("root entry '" + e.name + "' nblocks",
                         CheckStatus::kCorrupt,
                         "di_nblocks " + std::to_string(inode.value().nblocks) +
                             " != data extents + bmbt blocks " +
                             std::to_string(expected_blocks));
              ++bad;
            }
            if (bad > 8) break;
          }
          if (bad <= 8 && checked_attrs > 0) {
            report.Add("root child xattrs", CheckStatus::kOk,
                       std::to_string(checked_attrs) +
                           " attribute forks and rmap owners verified");
          }
        }
      }
    }
  }

  // Parent-pointer namespace audit: every real dirent has exactly one reverse
  // record and every reverse record has exactly one real dirent.  This is a
  // second, independent view of the directory tree (the root_children check
  // above only validates direct children).
  if (options.parent_pointers && live_sb.features.parent) {
    auto parents = VerifyParentPointers();
    if (!parents.ok()) {
      report.Add("parent pointers", CheckStatus::kCorrupt,
                 parents.error().Format());
    } else {
      const ParentNamespaceReport& p = parents.value();
      std::string detail = p.Summary();
      if (!p.clean()) {
        auto append = [&](const char* label,
                          const std::vector<std::string>& v) {
          if (!v.empty()) detail += std::string("; ") + label + ": " + v[0];
        };
        append("missing", p.missing);
        append("unexpected", p.unexpected);
        append("wrong generation", p.wrong_parent_generation);
        append("wrong name", p.wrong_name);
        append("duplicate", p.duplicate);
        append("dangling parent", p.dangling_parent);
        append("non-directory parent", p.non_directory_parent);
        append("dotdot", p.wrong_dotdot);
        append("nlink", p.link_count_mismatch);
        append("malformed", p.malformed);
      }
      report.Add("parent pointers",
                 p.clean() ? CheckStatus::kOk : CheckStatus::kCorrupt, detail);
    }
  }

  // Log state: classified with the recovery engine, read-only. A dirty log is
  // a legal crash state, not corruption: it is reported with what a replay
  // would do (recoverable transactions, unsupported items) so the caller can
  // decide between `xfswin recover` and giving up.
  if (options.log) {
    auto journal = AnalyzeJournal();
    if (!journal.ok()) {
      report.Add("log", CheckStatus::kCorrupt, journal.error().Format());
    } else {
      const RecoverySummary& summary = journal.value();
      std::string message =
          std::string(ToString(summary.state)) + " - " + summary.detail;
      CheckStatus cs = summary.state == LogState::kUnknown
                           ? CheckStatus::kWarning
                           : CheckStatus::kOk;
      if (summary.needed) {
        message += "; recovery required: recoverable transactions=" +
                   std::to_string(summary.recoverable_transactions) +
                   ", items=" + std::to_string(summary.items);
        if (!summary.unsupported_items.empty()) {
          message += ", unsupported items=" +
                     std::to_string(summary.unsupported_items.size()) + " (" +
                     summary.unsupported_items.front() + ")";
        } else {
          message += " (recoverable by `xfswin recover` or a read-write mount)";
        }
      }
      report.Add("log", cs, message);
    }
  }

  return report;
}

}  // namespace xfs
