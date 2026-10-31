// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/perf_stats.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace xfs {

uint64_t PerfStats::NowNanos() {
  using clock = std::chrono::steady_clock;
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          clock::now().time_since_epoch())
          .count());
}

PerfStats& PerfStats::Get() {
  static PerfStats instance;
  return instance;
}

PerfStats::PerfStats() : started_ns_(NowNanos()) {
  const char* env = std::getenv("XFSWIN_PERF_STATS");
  if (env && *env && std::strcmp(env, "0") != 0 &&
      std::strcmp(env, "false") != 0) {
    enabled_.store(true, std::memory_order_relaxed);
  }
}

void PerfStats::SetEnabled(bool enabled) {
  const bool was_enabled = enabled_.exchange(enabled, std::memory_order_relaxed);
  if (enabled && !was_enabled) started_ns_ = NowNanos();
}

void PerfStats::Reset() {
  auto zero = [](std::atomic<uint64_t>& value) {
    value.store(0, std::memory_order_relaxed);
  };
  zero(read_calls);
  zero(read_bytes);
  zero(write_calls);
  zero(write_bytes);
  zero(flush_calls);
  zero(device_read_calls);
  zero(device_read_bytes);
  zero(device_write_calls);
  zero(device_write_bytes);
  zero(device_flush_calls);
  zero(journal_transactions);
  zero(journal_commits);
  zero(journal_bytes);
  zero(journal_flushes);
  zero(metadata_write_calls);
  zero(metadata_write_bytes);
  zero(inode_opens);
  zero(inode_cache_hits);
  zero(inode_cache_misses);
  zero(extent_cache_hits);
  zero(extent_cache_misses);
  zero(directory_cache_hits);
  zero(directory_cache_misses);
  zero(path_cache_hits);
  zero(path_cache_misses);
  zero(rmap_tree_loads);
  zero(refcount_tree_loads);
  zero(lock_wait_count);
  zero(lock_wait_ns);
  zero(inode_lock_waits);
  zero(max_concurrent_reads);
  zero(max_concurrent_writes);
  zero(device_active_reads);
  zero(device_active_writes);
  zero(max_concurrent_device_reads);
  zero(max_concurrent_device_writes);
  zero(fs_open_calls);
  zero(fs_read_calls);
  zero(fs_write_calls);
  zero(fs_flush_calls);
  zero(fs_get_file_info_calls);
  zero(fs_read_directory_calls);
  zero(fs_cleanup_calls);
  zero(fs_close_calls);
  zero(fs_create_calls);
  zero(fs_rename_calls);
  started_ns_ = NowNanos();
}

void PerfStats::UpdatePeak(std::atomic<uint64_t>& peak, uint64_t value) {
  uint64_t current = peak.load(std::memory_order_relaxed);
  while (value > current &&
         !peak.compare_exchange_weak(current, value,
                                     std::memory_order_relaxed,
                                     std::memory_order_relaxed)) {
  }
}

TimedLockGuard::TimedLockGuard(conc::Mutex& mutex)
    : lock_(mutex, std::defer_lock) {
  // Do not pay two clock reads for every file-data write when instrumentation
  // is off. The lock itself is unchanged.
  if (!PerfStats::Get().enabled()) {
    lock_.lock();
    return;
  }
  const uint64_t start = PerfStats::NowNanos();
  lock_.lock();
  const uint64_t elapsed = PerfStats::NowNanos() - start;
  PerfStats& stats = PerfStats::Get();
  stats.lock_wait_count.fetch_add(1, std::memory_order_relaxed);
  stats.lock_wait_ns.fetch_add(elapsed, std::memory_order_relaxed);
  stats.inode_lock_waits.fetch_add(1, std::memory_order_relaxed);
}

PerfStats::ReadScope::ReadScope() {
  PerfStats& stats = Get();
  if (!stats.enabled()) return;
  active_ = true;
  const uint64_t now = stats.active_reads.fetch_add(1) + 1;
  stats.UpdatePeak(stats.max_concurrent_reads, now);
}

PerfStats::ReadScope::~ReadScope() {
  if (active_) Get().active_reads.fetch_sub(1);
}

PerfStats::WriteScope::WriteScope() {
  PerfStats& stats = Get();
  if (!stats.enabled()) return;
  active_ = true;
  const uint64_t now = stats.active_writes.fetch_add(1) + 1;
  stats.UpdatePeak(stats.max_concurrent_writes, now);
}

PerfStats::WriteScope::~WriteScope() {
  if (active_) Get().active_writes.fetch_sub(1);
}

PerfStats::DeviceReadScope::DeviceReadScope() {
  PerfStats& stats = Get();
  if (!stats.enabled()) return;
  active_ = true;
  const uint64_t now = stats.device_active_reads.fetch_add(1) + 1;
  stats.UpdatePeak(stats.max_concurrent_device_reads, now);
}

PerfStats::DeviceReadScope::~DeviceReadScope() {
  if (active_) Get().device_active_reads.fetch_sub(1);
}

PerfStats::DeviceWriteScope::DeviceWriteScope() {
  PerfStats& stats = Get();
  if (!stats.enabled()) return;
  active_ = true;
  const uint64_t now = stats.device_active_writes.fetch_add(1) + 1;
  stats.UpdatePeak(stats.max_concurrent_device_writes, now);
}

PerfStats::DeviceWriteScope::~DeviceWriteScope() {
  if (active_) Get().device_active_writes.fetch_sub(1);
}

namespace {

constexpr const char* kSchema = "xfswin-perf-1";

std::string Ratio(uint64_t numerator, uint64_t denominator) {
  if (denominator == 0) return "n/a";
  std::ostringstream out;
  out << std::fixed << std::setprecision(3)
      << static_cast<double>(numerator) / static_cast<double>(denominator);
  return out.str();
}

}  // namespace

std::string PerfStats::Text() const {
  const uint64_t elapsed_ns =
      started_ns_ ? NowNanos() - started_ns_ : 0;
  const double elapsed_s = static_cast<double>(elapsed_ns) / 1e9;
  std::ostringstream out;
  out << "XFS performance counters (" << (enabled() ? "enabled" : "disabled")
      << ", " << std::fixed << std::setprecision(3) << elapsed_s << " s)\n";
  out << "  filesystem read   : " << read_calls.load() << " calls, "
      << read_bytes.load() << " bytes\n";
  out << "  filesystem write  : " << write_calls.load() << " calls, "
      << write_bytes.load() << " bytes\n";
  out << "  device read       : " << device_read_calls.load() << " calls, "
      << device_read_bytes.load() << " bytes\n";
  out << "  device write      : " << device_write_calls.load() << " calls, "
      << device_write_bytes.load() << " bytes\n";
  out << "  device flush      : " << device_flush_calls.load() << "\n";
  out << "  flush calls       : " << flush_calls.load() << "\n";
  out << "  journal           : " << journal_transactions.load()
      << " transactions, " << journal_commits.load() << " commits, "
      << journal_bytes.load() << " bytes, " << journal_flushes.load()
      << " flushes\n";
  out << "  metadata write    : " << metadata_write_calls.load()
      << " calls, " << metadata_write_bytes.load() << " bytes\n";
  out << "  inode cache       : " << inode_opens.load() << " opens, "
      << inode_cache_hits.load() << " hits, "
      << inode_cache_misses.load() << " misses\n";
  out << "  extent cache      : " << extent_cache_hits.load() << " hits, "
      << extent_cache_misses.load() << " misses\n";
  out << "  directory cache   : " << directory_cache_hits.load() << " hits, "
      << directory_cache_misses.load() << " misses\n";
  out << "  path cache        : " << path_cache_hits.load() << " hits, "
      << path_cache_misses.load() << " misses\n";
  out << "  rmap/refcount load: " << rmap_tree_loads.load() << " / "
      << refcount_tree_loads.load() << "\n";
  out << "  locks             : " << lock_wait_count.load() << " waits, "
      << lock_wait_ns.load() << " ns, inode waits "
      << inode_lock_waits.load() << "\n";
  out << "  concurrency       : max reads " << max_concurrent_reads.load()
      << ", max writes " << max_concurrent_writes.load()
      << ", max device reads " << max_concurrent_device_reads.load()
      << ", max device writes " << max_concurrent_device_writes.load()
      << "\n";
  out << "  WinFsp callbacks  : Open " << fs_open_calls.load() << ", Read "
      << fs_read_calls.load() << ", Write " << fs_write_calls.load()
      << ", Flush " << fs_flush_calls.load() << ", GetFileInfo "
      << fs_get_file_info_calls.load() << ", ReadDirectory "
      << fs_read_directory_calls.load() << ", Cleanup "
      << fs_cleanup_calls.load() << ", Close " << fs_close_calls.load()
      << "\n";
  out << "  I/O amplification : read x"
      << Ratio(device_read_bytes.load(), read_bytes.load()) << ", write x"
      << Ratio(device_write_bytes.load(), write_bytes.load()) << "\n";
  return out.str();
}

std::string PerfStats::Json() const {
  const uint64_t elapsed_ns =
      started_ns_ ? NowNanos() - started_ns_ : 0;
  auto q = [](uint64_t value) { return std::to_string(value); };
  std::ostringstream out;
  out << "{";
  out << "\"schema\":\"" << kSchema << "\",";
  out << "\"enabled\":" << (enabled() ? "true" : "false") << ",";
  out << "\"elapsed_ns\":" << elapsed_ns << ",";
  out << "\"filesystem\":{"
      << "\"read_calls\":" << q(read_calls.load()) << ","
      << "\"read_bytes\":" << q(read_bytes.load()) << ","
      << "\"write_calls\":" << q(write_calls.load()) << ","
      << "\"write_bytes\":" << q(write_bytes.load()) << ","
      << "\"flush_calls\":" << q(flush_calls.load()) << "},";
  out << "\"device\":{"
      << "\"read_calls\":" << q(device_read_calls.load()) << ","
      << "\"read_bytes\":" << q(device_read_bytes.load()) << ","
      << "\"write_calls\":" << q(device_write_calls.load()) << ","
      << "\"write_bytes\":" << q(device_write_bytes.load()) << ","
      << "\"flush_calls\":" << q(device_flush_calls.load()) << "},";
  out << "\"journal\":{"
      << "\"transactions\":" << q(journal_transactions.load()) << ","
      << "\"commits\":" << q(journal_commits.load()) << ","
      << "\"bytes\":" << q(journal_bytes.load()) << ","
      << "\"flushes\":" << q(journal_flushes.load()) << ","
      << "\"metadata_write_calls\":" << q(metadata_write_calls.load()) << ","
      << "\"metadata_write_bytes\":" << q(metadata_write_bytes.load()) << "},";
  out << "\"caches\":{"
      << "\"inode_opens\":" << q(inode_opens.load()) << ","
      << "\"inode_hits\":" << q(inode_cache_hits.load()) << ","
      << "\"inode_misses\":" << q(inode_cache_misses.load()) << ","
      << "\"extent_hits\":" << q(extent_cache_hits.load()) << ","
      << "\"extent_misses\":" << q(extent_cache_misses.load()) << ","
      << "\"directory_hits\":" << q(directory_cache_hits.load()) << ","
      << "\"directory_misses\":" << q(directory_cache_misses.load()) << ","
      << "\"path_hits\":" << q(path_cache_hits.load()) << ","
      << "\"path_misses\":" << q(path_cache_misses.load()) << "},";
  out << "\"locks\":{"
      << "\"wait_count\":" << q(lock_wait_count.load()) << ","
      << "\"wait_ns\":" << q(lock_wait_ns.load()) << ","
      << "\"inode_waits\":" << q(inode_lock_waits.load()) << "},";
  out << "\"concurrency\":{"
      << "\"max_reads\":" << q(max_concurrent_reads.load()) << ","
      << "\"max_writes\":" << q(max_concurrent_writes.load()) << ","
      << "\"max_device_reads\":" << q(max_concurrent_device_reads.load())
      << ","
      << "\"max_device_writes\":" << q(max_concurrent_device_writes.load())
      << "},";
  out << "\"winfsp\":{"
      << "\"open\":" << q(fs_open_calls.load()) << ","
      << "\"read\":" << q(fs_read_calls.load()) << ","
      << "\"write\":" << q(fs_write_calls.load()) << ","
      << "\"flush\":" << q(fs_flush_calls.load()) << ","
      << "\"get_file_info\":" << q(fs_get_file_info_calls.load()) << ","
      << "\"read_directory\":" << q(fs_read_directory_calls.load()) << ","
      << "\"cleanup\":" << q(fs_cleanup_calls.load()) << ","
      << "\"close\":" << q(fs_close_calls.load()) << ","
      << "\"create\":" << q(fs_create_calls.load()) << ","
      << "\"rename\":" << q(fs_rename_calls.load()) << "},";
  out << "\"amplification\":{"
      << "\"read_bytes_per_filesystem_byte\":";
  if (read_bytes.load() == 0) {
    out << "null";
  } else {
    out << std::fixed << std::setprecision(6)
        << static_cast<double>(device_read_bytes.load()) /
               static_cast<double>(read_bytes.load());
  }
  out << ",\"write_bytes_per_filesystem_byte\":";
  if (write_bytes.load() == 0) {
    out << "null";
  } else {
    out << std::fixed << std::setprecision(6)
        << static_cast<double>(device_write_bytes.load()) /
               static_cast<double>(write_bytes.load());
  }
  out << "}}";
  return out.str();
}

}  // namespace xfs
