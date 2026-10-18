// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/filesystem.h"

#include <algorithm>
#include <cstring>

#include "xfs/ag.h"
#include "xfs/btree.h"
#include "xfs/crc32c.h"
#include "xfs/crash.h"
#include "xfs/endian.h"
#include "xfs/extent.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
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
          Status profile = CheckWritableProfile(fs->superblock_, fs->geometry_);
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
    Status profile = CheckWritableProfile(fs->superblock_, fs->geometry_);
    if (!profile.ok()) {
      auto error = profile.error();
      error.message +=
          " (read-only access is unaffected; mount without the read-write "
          "option)";
      return error;
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
  }

  for (const auto& w : fs->warnings_) XFS_WARN(w);
  return fs;
}

Result<XfsInode> XfsFilesystem::OpenInode(uint64_t ino) const {
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
    return inode;
  };

  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = inode_cache_.find(ino);
    if (it != inode_cache_.end()) return it->second;
  }

  if (!geometry_.InoValid(ino)) {
    return XFS_ERR(XfsError::kOutOfRange, "inode number out of range")
        .Ino(ino)
        .Metadata("inode");
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
  if (dir_cache_.size() >= kMaxCachedDirs ||
      dir_cache_entries_ + entries.size() > kMaxCachedDirEntries) {
    dir_cache_.clear();
    dir_cache_entries_ = 0;
  }
  DirCache cache;
  cache.index.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    cache.index.emplace(entries[i].name, i);
  }
  cache.entries = std::move(entries);
  dir_cache_entries_ += cache.entries.size();
  dir_cache_.emplace(ino, std::move(cache));
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
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = dir_cache_.find(ino);
    if (it != dir_cache_.end()) return it->second.entries;
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
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = dir_cache_.find(dir_ino);
    if (it != dir_cache_.end()) {
      auto indexed = it->second.index.find(std::string(name));
      if (indexed == it->second.index.end()) {
        return XFS_ERR(XfsError::kNotFound, "name not found in directory")
            .Ino(dir_ino);
      }
      return it->second.entries[indexed->second];
    }
  }

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
    return ReadFileData(*device_, geometry_, inode, offset, buffer, length,
                        extents);
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
  return total;
}

Result<std::shared_ptr<const std::vector<XfsExtent>>>
XfsFilesystem::LoadExtentsCached(const XfsInode& inode) const {
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = extent_cache_.find(inode.ino);
    if (it != extent_cache_.end()) return it->second;
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

// Recomputes di_nblocks from the extent map (data extents plus bmbt blocks),
// the same value xfs_repair checks. Formats without a data fork - shortform
// (local) directories and symlinks, device nodes, UUID inodes - have no
// blocks.
Status ExpectedInodeBlocks(BlockDevice& dev, const XfsGeometry& geo,
                           const XfsInode& inode, uint64_t* out) {
  if (inode.format == di::kFormatLocal || inode.format == di::kFormatDev ||
      inode.format == di::kFormatUuid) {
    *out = 0;
    return OkStatus();
  }
  std::vector<XfsExtent> extents;
  Status s = LoadDataForkExtents(dev, geo, inode, &extents);
  if (!s.ok()) return s;
  uint64_t blocks = 0;
  for (const XfsExtent& e : extents) blocks += e.blockcount;
  if (inode.format != di::kFormatBtree) {
    *out = blocks;
    return OkStatus();
  }
  if (inode.data_fork.size() < bt::kBmdrSize) {
    return XFS_ERR(XfsError::kCorruptInode, "bmbt fork too small")
        .Ino(inode.ino)
        .Metadata("inode");
  }
  const uint16_t root_level = GetBE16(inode.data_fork.data(), 0);
  const uint16_t root_numrecs = GetBE16(inode.data_fork.data(), 2);
  const size_t node_maxrecs =
      (inode.data_fork.size() - bt::kBmdrSize) / (bt::kBmbtKeySize + bt::kBmbtPtrSize);
  BtreeSpec spec;
  spec.name = "bmbt";
  spec.format = BtreeFormat::kLong;
  spec.block_size = geo.block_size;
  spec.key_size = bt::kBmbtKeySize;
  spec.ptr_size = bt::kBmbtPtrSize;
  spec.rec_size = bt::kBmbtRecSize;
  spec.magic = ondisk::kBmapCrcMagic;
  spec.max_depth = bt::kMaxTraversalLevels;
  spec.null_ptr = ~0ull;
  spec.expected_owner = inode.ino;
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
  cb.visit = [&](uint64_t, const BtreeBlockMeta&, const uint8_t*) -> Status {
    ++bmbt_blocks;
    return OkStatus();
  };
  for (uint16_t i = 0; i < root_numrecs; ++i) {
    const uint64_t ptr = GetBE64(inode.data_fork.data(),
                                 bt::kBmdrSize + node_maxrecs * 8 + i * 8);
    if (ptr == spec.null_ptr) {
      return XFS_ERR(XfsError::kCorruptInode, "bmbt root has a null child")
          .Ino(inode.ino)
          .Metadata("inode");
    }
    s = WalkBtree(spec, ptr, cb);
    if (!s.ok()) return s;
  }
  (void)root_level;
  *out = blocks + bmbt_blocks;
  return OkStatus();
}

}  // namespace

CheckReport XfsFilesystem::CheckConsistency(const CheckOptions& options) const {
  CheckReport report;
  const XfsGeometry& geo = geometry_;
  BlockDevice& dev = *device_;

  report.Add("superblock", CheckStatus::kOk,
             "magic/uuid/geometry OK, crc " +
                 std::string(superblock_.crc_verified ? "verified" : "skipped"));

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
      bool same = s.blocksize == superblock_.blocksize &&
                  s.dblocks == superblock_.dblocks &&
                  s.rblocks == superblock_.rblocks &&
                  s.rextents == superblock_.rextents &&
                  s.logstart == superblock_.logstart &&
                  s.rextsize == superblock_.rextsize &&
                  s.agblocks == superblock_.agblocks &&
                  s.agcount == superblock_.agcount &&
                  s.rbmblocks == superblock_.rbmblocks &&
                  s.logblocks == superblock_.logblocks &&
                  s.sectsize == superblock_.sectsize &&
                  s.inodesize == superblock_.inodesize;
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
        bnobt.sb_lsn = superblock_.lsn;
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
        if (superblock_.features.rmapbt) {
          BtreeSpec rmap;
          rmap.name = "rmapbt";
          // Short block header (56 bytes, CRC at 52) but two 20-byte keys per
          // node entry and 4-byte pointers (XFS_RMAP_KEY_ADDR etc).
          rmap.format = BtreeFormat::kShort;
          rmap.block_size = geo.block_size;
          rmap.key_size = bt::kRmapKeySize;
          rmap.ptr_size = bt::kRmapPtrSize;
          rmap.rec_size = bt::kRmapRecSize;
          rmap.magic = ondisk::kRmapCrcMagic;
          rmap.max_depth = bt::kMaxTraversalLevels;
          rmap.null_ptr = kNullAgBlock;
          rmap.expected_owner = agno;
          rmap.owner_size = 4;
          rmap.uuid = geo.meta_uuid;
          rmap.sb_lsn = superblock_.lsn;
          rmap.ptr_to_daddr = bnobt.ptr_to_daddr;
          BtreeWalkCallbacks rcb;
          rcb.read_block = read_ag_block;
          rcb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
          rcb.visit = [&](uint64_t, const BtreeBlockMeta&,
                          const uint8_t*) -> Status {
            ++rmap_blocks;
            return OkStatus();
          };
          const Status rs = WalkBtree(rmap, agf.rmap_root, rcb);
          if (!rs.ok()) {
            report.Add("AG" + std::to_string(agno) + " rmapbt",
                       CheckStatus::kCorrupt, rs.error().Format());
            rmap_ok = false;
          } else if (rmap_blocks != agf.rmap_blocks) {
            report.Add("AG" + std::to_string(agno) + " rmapbt blocks",
                       CheckStatus::kCorrupt,
                       "AGF rmap_blocks " + std::to_string(agf.rmap_blocks) +
                           " != counted " + std::to_string(rmap_blocks));
            rmap_ok = false;
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
        inobt.sb_lsn = superblock_.lsn;
        inobt.ptr_to_daddr = bnobt.ptr_to_daddr;

        InodeCounts ino;
        uint32_t last_startino = 0;
        bool ino_first = true;
        BtreeWalkCallbacks icb;
        icb.read_block = read_ag_block;
        icb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
        icb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                        const uint8_t* buf) -> Status {
          if (meta.level != 0) return OkStatus();
          for (uint16_t i = 0; i < meta.numrecs; ++i) {
            const uint8_t* rec = BtreeLeafRec(buf, meta, inobt, i);
            uint32_t startino = GetBE32(rec);
            uint64_t free_mask = GetBE64(rec + 8);
            if (geo.features.spinodes) {
              uint16_t holemask = GetBE16(rec + 4);
              uint8_t count = GetU8(rec + 6);
              uint8_t freecount = GetU8(rec + 7);
              uint32_t chunk_count = 0;
              for (int biti = 0; biti < 16; ++biti) {
                if (!(holemask & (1u << biti))) chunk_count += 4;
              }
              if (count != chunk_count) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "sparse inode record count mismatch")
                    .Ag(agno)
                    .Metadata("inobt")
                    .Expected(chunk_count)
                    .Actual(count);
              }
              uint32_t actual_free = 0;
              for (int ino_i = 0; ino_i < 64; ++ino_i) {
                bool hole = false;
                if (geo.features.spinodes) {
                  uint16_t holemask2 = GetBE16(rec + 4);
                  hole = (holemask2 & (1u << (ino_i / 4))) != 0;
                }
                if (hole) continue;
                if (free_mask & (1ULL << ino_i)) ++actual_free;
              }
              if (actual_free != freecount) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "sparse inode freecount mismatch")
                    .Ag(agno)
                    .Metadata("inobt")
                    .Expected(actual_free)
                    .Actual(freecount);
              }
              ino.count += count;
              ino.freecount += freecount;
            } else {
              uint32_t freecount = GetBE32(rec + 4);
              uint32_t actual_free = 0;
              for (int ino_i = 0; ino_i < 64; ++ino_i) {
                if (free_mask & (1ULL << ino_i)) ++actual_free;
              }
              if (actual_free != freecount) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "inobt freecount does not match mask")
                    .Ag(agno)
                    .Metadata("inobt")
                    .Expected(actual_free)
                    .Actual(freecount);
              }
              ino.count += 64;
              ino.freecount += freecount;
            }
            if (!ino_first && startino <= last_startino) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "inobt records out of order")
                  .Ag(agno)
                  .Metadata("inobt");
            }
            ino_first = false;
            last_startino = startino;
            ++ino.records;
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
      }
    }
  }

  if (options.allocation_groups) {
    if (total_free != superblock_.fdblocks) {
      report.Add("superblock counters", CheckStatus::kWarning,
                 "sum of AG free blocks " + std::to_string(total_free) +
                     " != sb_fdblocks " + std::to_string(superblock_.fdblocks) +
                     (superblock_.features2 & ondisk::kVersion2LazySbCount
                          ? " (lazy counter)"
                          : ""));
    } else {
      report.Add("superblock counters", CheckStatus::kOk,
                 "free block counters match");
    }
    if (total_icount != superblock_.icount ||
        total_ifree != superblock_.ifree) {
      report.Add("superblock inode counters", CheckStatus::kWarning,
                 "AGI inode counts " + std::to_string(total_icount) + "/" +
                     std::to_string(total_ifree) + " != sb " +
                     std::to_string(superblock_.icount) + "/" +
                     std::to_string(superblock_.ifree));
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
      auto entries = ListDirectory(geo.rootino);
      if (!entries.ok()) {
        report.Add("root directory", CheckStatus::kCorrupt,
                   entries.error().Format());
      } else {
        report.Add("root directory", CheckStatus::kOk,
                   std::to_string(entries.value().size()) + " entries");
        if (options.root_children) {
          uint32_t bad = 0;
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
        }
      }
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
