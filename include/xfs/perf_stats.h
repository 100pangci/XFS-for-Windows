// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - optional performance instrumentation (PERF-1).
//
// Disabled by default. Enable with XFSWIN_PERF_STATS=1 (or
// `xfswin --perf-stats ...`, or MountOptions::perf_stats) before mounting.
// Every counter is a relaxed atomic and is only touched when enabled, so the
// normal production path pays one predictable branch per event.
//
// The counters deliberately distinguish the layers that can be confused when
// a mounted filesystem is slow:
//   - XFS API reads/writes            (filesystem level)
//   - block-device reads/writes       (backend level)
//   - journal transactions/commits    (durability work)
//   - cache hits/misses               (metadata reparse work)
//   - WinFsp callback counts          (IPC/adapter work)
// A benchmark can therefore say whether a change moved the bottleneck instead
// of only measuring MB/s.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace xfs {

class PerfStats {
 public:
  static PerfStats& Get();

  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  void SetEnabled(bool enabled);
  void Reset();

  // ---- filesystem API ----
  std::atomic<uint64_t> read_calls{0};
  std::atomic<uint64_t> read_bytes{0};
  std::atomic<uint64_t> write_calls{0};
  std::atomic<uint64_t> write_bytes{0};
  std::atomic<uint64_t> flush_calls{0};

  // ---- block device ----
  std::atomic<uint64_t> device_read_calls{0};
  std::atomic<uint64_t> device_read_bytes{0};
  std::atomic<uint64_t> device_write_calls{0};
  std::atomic<uint64_t> device_write_bytes{0};
  std::atomic<uint64_t> device_flush_calls{0};

  // ---- journal / transaction ----
  std::atomic<uint64_t> journal_transactions{0};
  std::atomic<uint64_t> journal_commits{0};
  std::atomic<uint64_t> journal_bytes{0};
  std::atomic<uint64_t> journal_flushes{0};
  std::atomic<uint64_t> metadata_write_calls{0};
  std::atomic<uint64_t> metadata_write_bytes{0};

  // ---- caches ----
  std::atomic<uint64_t> inode_opens{0};
  std::atomic<uint64_t> inode_cache_hits{0};
  std::atomic<uint64_t> inode_cache_misses{0};
  std::atomic<uint64_t> extent_cache_hits{0};
  std::atomic<uint64_t> extent_cache_misses{0};
  std::atomic<uint64_t> directory_cache_hits{0};
  std::atomic<uint64_t> directory_cache_misses{0};
  std::atomic<uint64_t> path_cache_hits{0};
  std::atomic<uint64_t> path_cache_misses{0};
  std::atomic<uint64_t> rmap_tree_loads{0};
  std::atomic<uint64_t> refcount_tree_loads{0};

  // ---- locking ----
  std::atomic<uint64_t> lock_wait_count{0};
  std::atomic<uint64_t> lock_wait_ns{0};
  std::atomic<uint64_t> inode_lock_waits{0};

  // ---- concurrency (high-water marks) ----
  std::atomic<uint64_t> active_reads{0};
  std::atomic<uint64_t> active_writes{0};
  std::atomic<uint64_t> max_concurrent_reads{0};
  std::atomic<uint64_t> max_concurrent_writes{0};

  // ---- WinFsp callbacks ----
  std::atomic<uint64_t> fs_open_calls{0};
  std::atomic<uint64_t> fs_read_calls{0};
  std::atomic<uint64_t> fs_write_calls{0};
  std::atomic<uint64_t> fs_flush_calls{0};
  std::atomic<uint64_t> fs_get_file_info_calls{0};
  std::atomic<uint64_t> fs_read_directory_calls{0};
  std::atomic<uint64_t> fs_cleanup_calls{0};
  std::atomic<uint64_t> fs_close_calls{0};
  std::atomic<uint64_t> fs_create_calls{0};
  std::atomic<uint64_t> fs_rename_calls{0};

  // Text (human) and JSON reports. Both include the time since the stats were
  // enabled and a derived I/O amplification ratio.
  std::string Text() const;
  std::string Json() const;

  static uint64_t NowNanos();

  // RAII helpers used by the read/write hot paths.
  class ReadScope {
   public:
    ReadScope();
    ~ReadScope();
    ReadScope(const ReadScope&) = delete;
    ReadScope& operator=(const ReadScope&) = delete;

   private:
    bool active_ = false;
  };
  class WriteScope {
   public:
    WriteScope();
    ~WriteScope();
    WriteScope(const WriteScope&) = delete;
    WriteScope& operator=(const WriteScope&) = delete;

   private:
    bool active_ = false;
  };

 private:
  PerfStats();
  void UpdatePeak(std::atomic<uint64_t>& peak, uint64_t value);

  std::atomic<bool> enabled_{false};
  uint64_t started_ns_ = 0;
};

// A mutex guard that records how long the caller waited for the lock. Used for
// inode stripes; relies on the same optional counters.
class TimedLockGuard {
 public:
  explicit TimedLockGuard(std::mutex& mutex);
  ~TimedLockGuard() = default;
  TimedLockGuard(const TimedLockGuard&) = delete;
  TimedLockGuard& operator=(const TimedLockGuard&) = delete;

 private:
  std::unique_lock<std::mutex> lock_;
};

// Convenience: only evaluate the event when instrumentation is on.
inline bool PerfEnabled() { return PerfStats::Get().enabled(); }

}  // namespace xfs
