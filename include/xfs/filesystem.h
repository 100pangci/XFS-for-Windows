// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - filesystem-level API (read-only).
//
// XfsFilesystem owns a BlockDevice and exposes the core operations the
// WinFsp adapter and the CLI are built on. It never writes to an existing
// filesystem.
#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/cache.h"
#include "xfs/directory.h"
#include "xfs/error.h"
#include "xfs/extent.h"
#include "xfs/inode.h"
#include "xfs/log_checker.h"
#include "xfs/superblock.h"
#include "xfs/transaction.h"

namespace xfs {

struct MountOptions {
  bool verify_crc = true;
  bool check_log = true;
  // Allows mounting when the log is dirty/unknown or features are not fully
  // understood. Never allowed by default; read-only access only, and the
  // caller is responsible for the safety implications.
  bool force_unsafe_ro = false;
  // Requests a writable mount. The device must be writable, the log clean and
  // the feature profile must pass CheckWritableProfile(); otherwise mounting
  // fails instead of silently falling back to read-only. Callers that want
  // read-only access after a refused read-write mount ask for read-only.
  bool read_write = false;
};

struct XfsFileInfo {
  uint64_t ino = 0;
  uint16_t mode = 0;
  uint32_t uid = 0;
  uint32_t gid = 0;
  uint32_t projid = 0;
  uint32_t nlink = 0;
  uint64_t size = 0;
  uint64_t nblocks = 0;  // filesystem blocks used (di_nblocks)
  uint32_t nextents = 0;
  uint32_t anextents = 0;
  XfsTimestamp atime, mtime, ctime, crtime;
  uint16_t di_flags = 0;
  bool immutable = false;
  bool append_only = false;
};

enum class CheckStatus { kOk, kWarning, kCorrupt, kUnsupported };

const char* ToString(CheckStatus status);

// A heap buffer whose start is 4 KiB-aligned. Raw volume/disk handles require
// sector-aligned buffers, so storing file-data chunks in aligned memory lets
// device reads go straight into the cache without a bounce-buffer copy.
class AlignedBuffer {
 public:
  explicit AlignedBuffer(size_t size)
      : size_(size), storage_(new uint8_t[size + kAlignment]) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(storage_.get());
    data_ = reinterpret_cast<uint8_t*>(
        (base + kAlignment - 1) & ~static_cast<uintptr_t>(kAlignment - 1));
  }
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;

  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  void resize(size_t size) { size_ = size; }  // logical size; capacity is fixed

  static constexpr size_t kAlignment = 4096;

 private:
  size_t size_;
  std::unique_ptr<uint8_t[]> storage_;
  uint8_t* data_ = nullptr;
};

struct CheckItem {
  std::string name;
  CheckStatus status = CheckStatus::kOk;
  std::string message;
};

struct CheckReport {
  std::vector<CheckItem> items;

  void Add(std::string name, CheckStatus status, std::string message = {});
  CheckStatus Overall() const;
  std::string Summary() const;
};

struct CheckOptions {
  bool secondary_superblocks = true;
  bool allocation_groups = true;
  bool btrees = true;
  bool root_directory = true;
  bool log = true;
  // Verify every inode referenced from the root directory (shallow tree walk).
  bool root_children = true;
};

class XfsFilesystem {
 public:
  static Result<std::unique_ptr<XfsFilesystem>> Mount(
      std::unique_ptr<BlockDevice> device, const MountOptions& options);

  // Non-copyable/non-movable (keeps references stable).
  XfsFilesystem(const XfsFilesystem&) = delete;
  XfsFilesystem& operator=(const XfsFilesystem&) = delete;

  const XfsGeometry& geometry() const { return geometry_; }
  const XfsSuperblock& superblock() const { return superblock_; }
  const MountOptions& options() const { return options_; }
  BlockDevice& device() { return *device_; }
  const BlockDevice& device() const { return *device_; }
  const std::vector<std::string>& warnings() const { return warnings_; }
  uint64_t RootInode() const { return geometry_.rootino; }

  // True when this mount may modify the filesystem (MountOptions::read_write
  // plus a writable device, clean log and a writable feature profile).
  bool writable() const { return writable_; }

  // ---- core API ----
  Result<XfsInode> OpenInode(uint64_t ino) const;
  Result<XfsInode> LookupPath(std::string_view path) const;
  Result<std::vector<XfsDirEntry>> ListDirectory(uint64_t ino) const;
  Result<XfsDirEntry> LookupName(uint64_t ino, std::string_view name) const;
  Result<size_t> ReadFile(uint64_t ino, uint64_t offset, void* buffer,
                          size_t length) const;
  // Fast path: the caller already holds the inode (and may hold its cached
  // extent list); skips the inode cache and extent-cache lookups.
  Result<size_t> ReadFile(const XfsInode& inode,
                          const std::vector<XfsExtent>* cached_extents,
                          uint64_t offset, void* buffer, size_t length) const;
  Result<std::string> ReadSymlink(uint64_t ino) const;
  Result<XfsFileInfo> GetFileInfo(uint64_t ino) const;

  // ---- mutation API (read-write mounts only) ----
  // Overwrites bytes inside extents that are already allocated and written.
  // Never allocates, never converts an unwritten extent and never changes the
  // inode size (see MapWrittenRange for the exact range rule). Returns the
  // number of bytes written; on failure nothing is guaranteed to be intact
  // beyond the reported error, but no metadata was touched either.
  //
  // A successful write also updates mtime/ctime through a metadata
  // transaction (RW-1), exactly like a Linux write: the data itself is not
  // journalled, the inode change is.
  Result<size_t> OverwriteFileData(uint64_t ino, uint64_t offset,
                                   const void* buffer, size_t length);

  // ---- transactions and metadata mutations (read-write mounts only) ----
  // Begins a transaction at the current log position. The returned LSN is the
  // LSN of the record that will carry it; log items and the metadata buffers
  // they describe must store that LSN. Callers must serialize mutations (the
  // built-in paths do, through RunTransaction).
  Result<XfsTransaction> BeginTransaction();
  // Commits a transaction: log record + flush, then metadata + flush, then
  // cache invalidation. A failed build (before commit) writes nothing.
  Status CommitTransaction(XfsTransaction& txn);
  // Begins a transaction, lets `build` fill it, and commits it under the
  // filesystem mutation lock. This is the entry point mutations use.
  Status RunTransaction(
      const std::function<Status(XfsTransaction&)>& build);

  // Sets the inode's atime/mtime/ctime in one transaction (RW-1's first
  // metadata mutation). Returns the updated file info.
  Result<XfsFileInfo> SetFileTimes(uint64_t ino, const XfsTimestamp& atime,
                                   const XfsTimestamp& mtime,
                                   const XfsTimestamp& ctime);

  // Writes the clean-unmount log record. Must be called before an unmount so
  // the next mount (Linux or this build) finds a clean log; it is a no-op on a
  // read-only mount or when the log is already clean.
  Status WriteCleanUnmountRecord();

  // Flushes pending writes to stable storage. Returns failure if the device
  // flush fails: callers (WinFsp Flush, clean unmount) must not report
  // success after a failed flush.
  Status Flush();

  // Central invalidation entry point: every mutation reports what it changed
  // here instead of touching caches directly (see xfs/cache.h for the policy
  // table).
  void InvalidateCaches(const CacheInvalidation& invalidation) const;

  // ---- read-only consistency validation (xfswin check) ----
  CheckReport CheckConsistency(const CheckOptions& options) const;

 private:
  XfsFilesystem() = default;

  // Read-only caches. Directory contents and inodes cannot change while we
  // mount (the filesystem is never written by this process), so caching them
  // is safe and makes large-directory enumeration fast.
  struct DirCache {
    std::vector<XfsDirEntry> entries;
    std::unordered_map<std::string, size_t> index;
  };
  void CacheDirectory(uint64_t ino, std::vector<XfsDirEntry> entries) const;

  // Parsed data-fork extent list, from the extent cache or freshly loaded.
  // Shared between reads and writes so both see the same extents.
  Result<std::shared_ptr<const std::vector<XfsExtent>>> LoadExtentsCached(
      const XfsInode& inode) const;

  // Updates atime/mtime/ctime inside an open transaction (the caller holds the
  // mutation lock and owns the transaction). Shared by the public
  // SetFileTimes and by the data write path.
  Status UpdateTimesInTransaction(XfsTransaction& txn, uint64_t ino,
                                  const XfsTimestamp& atime,
                                  const XfsTimestamp& mtime,
                                  const XfsTimestamp& ctime);

  // ---- file data chunk cache ----
  // Reads are served in 256 KiB chunks so that one device read covers many
  // small reads (thumbnail/metadata decoders) and repeated reads never touch
  // the device again. A global byte budget bounds memory use; eviction is LRU.
  //
  // Writers and readers coordinate through a per-inode generation counter:
  // a mutation writes the device first, then bumps the generation, then drops
  // the affected chunks (in that order - see InvalidateCaches). A reader
  // snapshots the generation before its device read and refuses to publish a
  // chunk whose generation is no longer current, so a stale chunk can never
  // survive in the cache.
  struct DataCacheKey {
    uint64_t ino = 0;
    uint64_t chunk = 0;
    bool operator==(const DataCacheKey& other) const {
      return ino == other.ino && chunk == other.chunk;
    }
  };
  struct DataCacheKeyHash {
    size_t operator()(const DataCacheKey& key) const {
      uint64_t h = key.ino * 0x9e3779b97f4a7c15ull;
      h ^= key.chunk + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      return static_cast<size_t>(h);
    }
  };
  struct DataCacheEntry {
    std::shared_ptr<const AlignedBuffer> data;
    uint64_t generation = 0;  // data generation at insert time
    std::list<DataCacheKey>::iterator lru;
  };
  std::shared_ptr<const AlignedBuffer> LookupDataChunk(uint64_t ino,
                                                       uint64_t chunk) const;
  std::shared_ptr<const AlignedBuffer> InsertDataChunk(
      uint64_t ino, uint64_t chunk, std::shared_ptr<const AlignedBuffer> data,
      uint64_t generation) const;
  // Current data generation of an inode (0 for never-written inodes).
  uint64_t DataGeneration(uint64_t ino) const;
  void DropDataChunks(uint64_t ino, uint64_t byte_offset, uint64_t byte_length,
                      bool drop_all) const;

  std::unique_ptr<BlockDevice> device_;
  XfsSuperblock superblock_;
  XfsGeometry geometry_;
  MountOptions options_;
  std::vector<std::string> warnings_;
  bool writable_ = false;

  // Serializes mutations (coarse by design: correctness first, then per-object
  // locking once the writer is stable). Also orders "device write" against
  // "cache publication" for concurrent readers.
  mutable std::mutex mutation_mutex_;

  // ---- journal state (read-write writer) ----
  // Next log write position. Scanned from the log on first use; kept in memory
  // while the filesystem is mounted and invalidated after a clean-unmount
  // record so the next mutation rescans.
  bool log_position_valid_ = false;
  LogPosition log_position_;
  // True when this mount wrote at least one record that is not an unmount
  // record (i.e. the log needs a clean-unmount record before we let go).
  bool log_dirty_ = false;
  uint32_t next_tid_ = 1;

  mutable std::mutex cache_mutex_;
  mutable std::unordered_map<uint64_t, XfsInode> inode_cache_;
  // 16 KiB inode-chunk read-ahead (shared so lookups stay cheap).
  mutable std::unordered_map<uint64_t,
                             std::shared_ptr<const std::vector<uint8_t>>>
      inode_chunk_cache_;
  mutable std::unordered_map<uint64_t, XfsFileInfo> info_cache_;
  mutable std::unordered_map<uint64_t,
                             std::shared_ptr<const std::vector<XfsExtent>>>
      extent_cache_;
  mutable std::unordered_map<uint64_t, DirCache> dir_cache_;
  mutable size_t dir_cache_entries_ = 0;

  static constexpr uint64_t kInodeChunkBytes = 16384;  // inode read-ahead chunk
  static constexpr uint64_t kDataChunkBytes = 512 * 1024;
  static constexpr size_t kDataCacheMaxBytes = 512ull * 1024 * 1024;
  mutable std::mutex data_mutex_;
  mutable std::unordered_map<DataCacheKey, DataCacheEntry, DataCacheKeyHash>
      data_cache_;
  mutable std::list<DataCacheKey> data_lru_;  // front = most recently used
  mutable size_t data_cache_bytes_ = 0;
  // Per-inode write generation (see LookupDataChunk/InvalidateCaches).
  mutable std::unordered_map<uint64_t, uint64_t> data_generation_;
};

}  // namespace xfs
