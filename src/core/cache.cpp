// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - cache coherence and deferred timestamps.
//
// Owner: bounded inode/directory/extent/data caches, generation-checked data
// publication, and the deferred mtime/ctime lifecycle. It does not own the
// mutation transaction or the commit boundary; mutations call the central
// CacheInvalidation entry point (src/core/invalidation.cpp).
#include "xfs/filesystem.h"

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "xfs/perf_stats.h"
#include "xfs/write.h"

namespace xfs {

void XfsFilesystem::CacheDirectory(uint64_t ino,
                                   std::vector<XfsDirEntry> entries,
                                   uint64_t generation) const {
  std::lock_guard<xfs::conc::Mutex> lock(cache_mutex_);
  if (metadata_generation_[ino] != generation) {
    // A mutation invalidated this directory while the listing was being read.
    // Publishing it would make LookupName return a name that no longer exists
    // (or miss one that does) for as long as the cache entry lives.
    return;
  }
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
  std::lock_guard<xfs::conc::Mutex> lock(cache_mutex_);
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
  std::lock_guard<xfs::conc::Mutex> lock(cache_mutex_);
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

void XfsFilesystem::MarkDeferredTimes(uint64_t ino,
                                      const XfsTimestamp& now) const {
  // Drop the cached XfsFileInfo first so GetFileInfo rebuilds it through the
  // OpenInode overlay. (Cache lock before the deferred lock, matching OpenInode.)
  {
    std::lock_guard<xfs::conc::Mutex> cache_lock(cache_mutex_);
    info_cache_.erase(ino);
  }
  std::lock_guard<xfs::conc::Mutex> lock(deferred_mutex_);
  deferred_times_[ino] = now;
}

Status XfsFilesystem::CommitDeferredTimesUnderInodeLock(uint64_t ino) {
  XfsTimestamp now;
  {
    std::lock_guard<xfs::conc::Mutex> lock(deferred_mutex_);
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
    std::lock_guard<xfs::conc::Mutex> lock(deferred_mutex_);
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
    std::lock_guard<xfs::conc::Mutex> lock(deferred_mutex_);
    if (deferred_times_.find(ino) == deferred_times_.end()) {
      return OkStatus();
    }
  }
  std::shared_lock<xfs::conc::SharedMutex> barrier(write_barrier_);
  TimedLockGuard inode_lock(InodeLock(ino));
  std::lock_guard<xfs::conc::Mutex> lock(mutation_mutex_);
  return CommitDeferredTimesUnderInodeLock(ino);
}

Status XfsFilesystem::CommitAllDeferredTimesLocked() {
  for (;;) {
    uint64_t ino = 0;
    {
      std::lock_guard<xfs::conc::Mutex> lock(deferred_mutex_);
      if (deferred_times_.empty()) break;
      ino = deferred_times_.begin()->first;
    }
    std::lock_guard<xfs::conc::Mutex> inode_lock(InodeLock(ino));
    std::lock_guard<xfs::conc::Mutex> lock(mutation_mutex_);
    Status status = CommitDeferredTimesUnderInodeLock(ino);
    if (!status.ok()) return status;
  }
  return OkStatus();
}

Status XfsFilesystem::CommitAllDeferredMetadata() {
  if (!writable_) return OkStatus();
  std::unique_lock<xfs::conc::SharedMutex> barrier(write_barrier_);
  return CommitAllDeferredTimesLocked();
}

uint64_t XfsFilesystem::ExtentGeneration(uint64_t ino) const {
  std::lock_guard<xfs::conc::Mutex> lock(cache_mutex_);
  auto it = extent_generation_.find(ino);
  return it == extent_generation_.end() ? 0 : it->second;
}

uint64_t XfsFilesystem::DataGeneration(uint64_t ino) const {
  std::lock_guard<xfs::conc::Mutex> lock(data_mutex_);
  auto it = data_generation_.find(ino);
  return it == data_generation_.end() ? 0 : it->second;
}

uint64_t XfsFilesystem::MetadataGeneration(uint64_t ino) const {
  std::lock_guard<xfs::conc::Mutex> lock(cache_mutex_);
  auto it = metadata_generation_.find(ino);
  return it == metadata_generation_.end() ? 0 : it->second;
}

std::shared_ptr<const AlignedBuffer> XfsFilesystem::LookupDataChunk(
    uint64_t ino, uint64_t chunk) const {
  const DataCacheKey key{ino, chunk};
  std::lock_guard<xfs::conc::Mutex> lock(data_mutex_);
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
  std::lock_guard<xfs::conc::Mutex> lock(data_mutex_);
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
  std::lock_guard<xfs::conc::Mutex> lock(data_mutex_);
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

}  // namespace xfs
