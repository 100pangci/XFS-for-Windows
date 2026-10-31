// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - central cache invalidation.
//
// This is the only place that touches the caches on behalf of a mutation; see
// include/xfs/cache.h for the policy table that maps mutations to the objects
// they invalidate.
#include "xfs/filesystem.h"

#include "xfs/log.h"

namespace xfs {

void XfsFilesystem::InvalidateCaches(const CacheInvalidation& invalidation) const {
  XFS_CONC_SYNC("cache.invalidate.before",
                "ino=" + std::to_string(invalidation.ino));
  struct ExitHook {
    ~ExitHook() { XFS_CONC_SYNC("cache.invalidate.after", ""); }
  } exit_hook;
  if (invalidation.Has(CacheInvalidation::kAll)) {
    {
      std::lock_guard<xfs::conc::Mutex> lock(cache_mutex_);
      inode_cache_.clear();
      inode_chunk_cache_.clear();
      info_cache_.clear();
      extent_cache_.clear();
      for (auto& entry : extent_generation_) ++entry.second;
      for (auto& entry : metadata_generation_) ++entry.second;
      ++inode_chunk_generation_;
      dir_cache_.clear();
      dir_cache_entries_ = 0;
    }
    std::lock_guard<xfs::conc::Mutex> lock(data_mutex_);
    // Bump every known generation instead of clearing the map: a reader that
    // is mid-read must not be able to publish a chunk afterwards.
    for (auto& entry : data_generation_) ++entry.second;
    data_cache_.clear();
    data_lru_.clear();
    data_cache_bytes_ = 0;
    XFS_TRACE("caches: dropped everything");
    return;
  }

  const uint64_t ino = invalidation.ino;
  {
    std::lock_guard<xfs::conc::Mutex> lock(cache_mutex_);
    const bool metadata_changed =
        invalidation.Has(CacheInvalidation::kInode) ||
        invalidation.Has(CacheInvalidation::kInodeChunk) ||
        invalidation.Has(CacheInvalidation::kFileInfo) ||
        invalidation.Has(CacheInvalidation::kExtents) ||
        invalidation.Has(CacheInvalidation::kDirectory);
    if (metadata_changed) {
      ++metadata_generation_[ino];
    }
    if (invalidation.Has(CacheInvalidation::kInodeChunk)) {
      ++inode_chunk_generation_;
    }
    if (invalidation.Has(CacheInvalidation::kInode)) {
      inode_cache_.erase(ino);
    }
    if (invalidation.Has(CacheInvalidation::kInodeChunk)) {
      if (geometry_.InoValid(ino)) {
        const uint64_t inode_offset = geometry_.InoToByteOffset(ino);
        inode_chunk_cache_.erase(inode_offset & ~(kInodeChunkBytes - 1));
      }
    }
    if (invalidation.Has(CacheInvalidation::kFileInfo)) {
      info_cache_.erase(ino);
    }
    if (invalidation.Has(CacheInvalidation::kExtents)) {
      extent_cache_.erase(ino);
      ++extent_generation_[ino];
    }
    if (invalidation.Has(CacheInvalidation::kDirectory)) {
      auto it = dir_cache_.find(ino);
      if (it != dir_cache_.end()) {
        dir_cache_entries_ -= it->second.entries.size();
        dir_cache_.erase(it);
      }
    }
  }
  if (invalidation.Has(CacheInvalidation::kData)) {
    DropDataChunks(ino, invalidation.data_offset, invalidation.data_length,
                   /*drop_all=*/invalidation.data_length == 0);
  }
}

}  // namespace xfs
