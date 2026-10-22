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
#include <optional>
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
#include "xfs/recovery.h"
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
  // Requests a writable mount. The device must be writable, the log clean or
  // recoverable (see below) and the feature profile must pass
  // CheckWritableProfile(); otherwise mounting fails instead of silently
  // falling back to read-only. Callers that want read-only access after a
  // refused read-write mount ask for read-only.
  bool read_write = false;
  // (RW-5) When set, a dirty log is replayed as part of the mount (read-write
  // mounts only; the default). `false` is the "norecovery" behaviour: a dirty
  // log refuses the mount unless force_unsafe_ro is set.
  bool recover_dirty_log = true;
  // Finish a mount-time recovery with a clean-unmount record (the standalone
  // `xfswin recover` shape). Read-write mounts keep the log writable instead.
  bool clean_log_after_recovery = false;
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

  // Full write path (RW-2): overwrites, fills holes, converts unwritten
  // extents and extends EOF. Data is written and flushed before the metadata
  // that makes it visible, and freshly allocated or newly visible bytes are
  // never left as uninitialised disk contents (newly covered bytes outside the
  // caller's range are zeroed). Crash contract: after replay the file is
  // either in the old state or in the new one; an unfinished extension leaves
  // an unwritten (zero-reading) extent, never garbage.
  //
  // `append` writes the bytes at EOF: the offset is resolved *under the
  // mutation lock*, so concurrent appends cannot both compute the same EOF.
  Result<size_t> WriteFileData(uint64_t ino, uint64_t offset,
                               const void* buffer, size_t length,
                               bool append = false);

  // SetFileSize (ftruncate semantics):
  //   - growing only moves the logical EOF; the new range is a hole and no
  //     blocks are allocated. Bytes that would become visible over an existing
  //     *written* extent are zeroed on disk first, like Linux does;
  //   - shrinking removes the blocks from ceil(new_size / block_size) on,
  //     frees them through the allocator, keeps a partially covered last
  //     block and zeroes the part of it beyond the new size.
  Result<XfsFileInfo> SetFileSize(uint64_t ino, uint64_t new_size);

  // ---- namespace mutation (RW-3, read-write mounts only) ----
  // Creates a regular file (mode must be a regular-file mode; 0 means 0644)
  // in `parent_ino`. The whole create is one transaction: inode allocation,
  // inode initialisation, parent dirent insertion, parent timestamps and the
  // AGI/inobt/superblock counters commit together. Returns the new inode.
  Result<uint64_t> CreateRegularFile(uint64_t parent_ino, std::string_view name,
                              uint16_t mode, uint32_t uid, uint32_t gid);

  // Creates an empty directory (shortform, nlink 2) and bumps the parent's
  // link count, in one transaction.
  Status Mkdir(uint64_t parent_ino, std::string_view name, uint16_t mode,
               uint32_t uid, uint32_t gid);

  // Removes a regular file. The namespace entry, the link count transition
  // and the AGI unlinked-list insertion are one transaction. When
  // `defer_inode_free` is false the inode is then truncated and freed
  // (rolling transactions, like xfs_inactive/xfs_ifree); when it is true the
  // zero-link inode stays on the unlinked list until FreeUnlinkedInode.
  Status Unlink(uint64_t parent_ino, std::string_view name,
                bool defer_inode_free = false);

  // Removes an empty directory (only "." and ".." may remain), same
  // transaction/deferral contract as Unlink. A non-empty directory fails with
  // kDirectoryNotEmpty before anything is modified.
  Status RemoveEmptyDirectory(uint64_t parent_ino, std::string_view name,
                         bool defer_inode_free = false);

  // Finishes the deletion of a zero-link inode that is on the AGI unlinked
  // list: truncates its data fork and frees the inode (xfs_inactive +
  // xfs_ifree). This is the "last handle closed" step of an open-but-unlinked
  // file.
  Status FreeUnlinkedInode(uint64_t ino);

  // ---- rename / hardlink / symlink (RW-4, read-write mounts only) ----
  struct RenameOutcome {
    uint64_t moved_ino = 0;      // the inode that changed name and/or parent
    uint64_t replaced_ino = 0;   // target inode, 0 when the target was free
    uint32_t replaced_new_nlink = 0;
    // True when the replaced inode reached nlink 0 and went on the AGI
    // unlinked list; it must be freed with FreeUnlinkedInode once its last
    // handle closes (unless defer_replaced_free was false and this call
    // already did it).
    bool replaced_zero_link = false;
  };

  // Atomic rename (xfs_rename + xfs_dir_rename_children): one transaction
  // carries the source and target directory edits, both parents' timestamps
  // and link counts, the moved inode's ctime, the ".." rewrite of a
  // cross-parent directory move, and the replaced target's link count plus
  // AGI unlinked insertion. A crash therefore leaves either the old or the new
  // namespace, never a half-applied one.
  //
  // Semantics match Linux/POSIX: renaming between two names of the same inode
  // is a no-op; a directory can only replace an empty directory, a file only a
  // file; a directory cannot move into its own subtree (kWouldCycle); a
  // non-empty target directory fails with kDirectoryNotEmpty before anything
  // is modified; `replace_existing == false` refuses an existing target with
  // kAlreadyExists (RENAME_NOREPLACE).
  //
  // `defer_replaced_free` mirrors Unlink: with true the zero-link target inode
  // stays on the unlinked list for a later FreeUnlinkedInode (open handles);
  // with false it is freed before returning.
  Result<RenameOutcome> Rename(uint64_t src_parent, std::string_view src_name,
                               uint64_t dst_parent, std::string_view dst_name,
                               bool replace_existing,
                               bool defer_replaced_free = false);

  // Creates a hard link (xfs_link): one transaction inserts the dirent,
  // updates the parent's timestamps and bumps the inode's link count + ctime.
  // Directories are refused (kPermissionDenied); a link count at XFS_MAXLINK
  // fails with kTooManyLinks.
  Status Link(uint64_t ino, uint64_t dst_parent, std::string_view dst_name);

  // Creates a real XFS symlink inode (xfs_symlink). Targets that fit the
  // inode literal area are stored inline (LOCAL); longer targets (up to
  // XFS_SYMLINK_MAXLEN - 1 bytes) get remote data blocks with the v5
  // xfs_dsymlink_hdr format. The target is an opaque byte string: it is never
  // resolved, escaped or required to exist.
  Result<uint64_t> CreateSymlink(uint64_t parent_ino, std::string_view name,
                                 std::string_view target, uint16_t mode,
                                 uint32_t uid, uint32_t gid);

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

  // Writes the primary superblock sector with its LSN set to `lsn` and a fresh
  // CRC. The kernel does the same when it covers the log: every metadata LSN
  // must stay at or below the superblock's LSN, otherwise a later check of the
  // clean image reports metadata "ahead of superblock".
  Status StampSuperblockLsn(uint64_t lsn);

  // Rebuilds the in-core superblock counters from the AGI/AGF headers
  // (xfs_initialize_perag_data). Lazy counters are not authoritative after a
  // crash, and the completion transactions need correct values.
  Status RefreshSuperblockCounters();

  // ---- journal recovery (RW-5, writable devices) ----
  // Read-only analysis of the internal log for `xfswin check` and for mount
  // diagnostics: head/tail, record validation, committed transactions and the
  // item inventory (unsupported item types make the log unrecoverable by this
  // build). Never writes.
  struct RecoverySummary {
    bool needed = false;
    LogState state = LogState::kUnknown;
    uint64_t recoverable_transactions = 0;
    uint64_t items = 0;
    std::vector<std::string> unsupported_items;
    std::string detail;
    bool recoverable() const { return unsupported_items.empty(); }
  };
  Result<RecoverySummary> AnalyzeJournal() const;

  // The outcome of a recovery run.
  struct RecoveryReport {
    ReplayStats replay;
    uint64_t intents_completed = 0;
    uint64_t unlinked_inodes_processed = 0;
    uint64_t agi_buckets_cleared = 0;
    bool recovered = false;  // false when the log was already clean/empty
  };
  // Runs the whole recovery: scan and validate, replay the committed
  // transactions (pass1/pass2), re-read the superblock, finish unresolved
  // intents through normal transactions, process the AGI unlinked lists and
  // flush. Requires a writable device. Refuses - before the first write -
  // when the log contains anything this build cannot replay.
  //
  // `clean_log` writes the clean-unmount record at the end (the standalone
  // `xfswin recover` shape); a mounted filesystem leaves the log writable and
  // writes the unmount record when it goes away.
  enum class RecoveryFinish { kMount, kCleanLog };
  Result<RecoveryReport> RecoverJournal(RecoveryFinish finish);

  // The report of the recovery Mount ran, when the log was dirty.
  const std::optional<RecoveryReport>& recovery_report() const {
    return recovery_report_;
  }

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

  // ---- inode and data fork write helpers (RW-2) ----
  // Reads the complete on-disk inode image.
  Result<std::vector<uint8_t>> ReadRawInode(uint64_t ino) const;
  // Reads the inode, lets `mutate` change its raw bytes, then logs it (core +
  // optional data fork region) and queues the write-back.
  Status UpdateInodeInTransaction(
      XfsTransaction& txn, uint64_t ino, uint32_t ilog_fields,
      const std::vector<uint8_t>& fork_region,
      const std::function<Status(uint8_t* raw)>& mutate);
  // Logs an already prepared inode image (the LSN and CRC are stamped here).
  Status LogPreparedInode(XfsTransaction& txn, uint64_t ino,
                          std::vector<uint8_t> inode, uint32_t ilog_fields,
                          const std::vector<uint8_t>& fork_region);

  // ---- namespace helpers (RW-3) ----
  // Allocates a new free inode chunk in a separate transaction when no AG has
  // free inodes, so the create/mkdir transaction stays within one log record.
  Status EnsureInodeCapacity(uint32_t preferred_agno);
  // Shared unlink/rmdir implementation; the caller holds the transaction.
  Status RemoveNamespaceEntry(uint64_t parent_ino, std::string_view name,
                              bool directory_expected, bool defer_inode_free,
                              uint64_t* removed_ino);
  // Releases every data block of an inode with nlink 0 in rolling
  // transactions (xfs_itruncate_extents before xfs_ifree).
  Status TruncateForFree(uint64_t ino);

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
  // (RW-5) While unresolved intents from a recovered log are being completed,
  // every commit must keep h_tail_lsn pinned to the start of the replayed
  // window: a crash mid-completion then re-replays the whole window (including
  // the intent items and their done items), which is what makes the completion
  // crash-safe. Cleared once all intents are finished.
  std::optional<uint64_t> recovery_tail_pin_;
  // Set by Mount when it replayed a dirty log.
  std::optional<RecoveryReport> recovery_report_;

  // Runs the recovery phases on the already-scanned log (RecoverJournal).
  Result<RecoveryReport> RecoverScannedLog(const LogScan& scan,
                                           RecoveryFinish finish);
  // Completes one recovered intent item (deferred operation) in a normal
  // transaction, logging the matching done item.
  Status CompleteIntent(const PendingIntent& intent);
  // Processes every AG's AGI unlinked buckets (xlog_recover_process_iunlinks):
  // truncates and frees zero-link inodes, clearing buckets that cannot be
  // walked.
  Result<uint64_t> ProcessAgiUnlinkedLists(uint64_t* buckets_cleared);
  Status ClearAgiBucket(uint32_t agno, uint32_t bucket,
                        const char* reason);

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
