// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// PERF-1 core benchmark runner.
//
// Usage:
//   xfswin_perf --image <freshly formatted image> [--json perf.json]
//               [--label baseline] [--small-files 1000]
//
// The runner measures the core (no WinFsp) so it can run on every Windows CI
// machine without a driver. It asserts deterministic properties (device call
// counts, overlap counters), not absolute MB/s. Timings are still recorded for
// A/B comparison with the same runner on the same machine.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "xfs/filesystem.h"
#include "xfs/image_device.h"
#include "xfs/log.h"
#include "xfs/perf_stats.h"

namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

struct Workload {
  std::string name;
  uint64_t bytes = 0;
  uint64_t ops = 0;
  uint64_t ns = 0;
  uint64_t read_calls = 0;
  uint64_t read_bytes = 0;
  uint64_t write_calls = 0;
  uint64_t write_bytes = 0;
  uint64_t device_read_calls = 0;
  uint64_t device_read_bytes = 0;
  uint64_t device_write_calls = 0;
  uint64_t device_write_bytes = 0;
  uint64_t flush_calls = 0;
  uint64_t journal_commits = 0;
  uint64_t journal_bytes = 0;
  uint64_t max_concurrent_reads = 0;
  uint64_t max_concurrent_writes = 0;
  uint64_t inode_cache_hits = 0;
  uint64_t inode_cache_misses = 0;
  uint64_t extent_cache_hits = 0;
  uint64_t extent_cache_misses = 0;
  uint64_t directory_cache_hits = 0;
  uint64_t directory_cache_misses = 0;
  uint64_t lock_wait_ns = 0;
  bool gate_ok = true;
  std::string gate_failure;
};

uint64_t Counter(const std::atomic<uint64_t>& value) {
  return value.load(std::memory_order_relaxed);
}

void ResetCounters(xfs::PerfStats& stats) {
  stats.Reset();
}

Workload Snapshot(const std::string& name, uint64_t ns, uint64_t bytes,
                  uint64_t ops, xfs::PerfStats& stats) {
  Workload w;
  w.name = name;
  w.ns = ns;
  w.bytes = bytes;
  w.ops = ops;
  w.read_calls = Counter(stats.read_calls);
  w.read_bytes = Counter(stats.read_bytes);
  w.write_calls = Counter(stats.write_calls);
  w.write_bytes = Counter(stats.write_bytes);
  w.device_read_calls = Counter(stats.device_read_calls);
  w.device_read_bytes = Counter(stats.device_read_bytes);
  w.device_write_calls = Counter(stats.device_write_calls);
  w.device_write_bytes = Counter(stats.device_write_bytes);
  w.flush_calls = Counter(stats.flush_calls);
  w.journal_commits = Counter(stats.journal_commits);
  w.journal_bytes = Counter(stats.journal_bytes);
  w.max_concurrent_reads = Counter(stats.max_concurrent_reads);
  w.max_concurrent_writes = Counter(stats.max_concurrent_writes);
  w.inode_cache_hits = Counter(stats.inode_cache_hits);
  w.inode_cache_misses = Counter(stats.inode_cache_misses);
  w.extent_cache_hits = Counter(stats.extent_cache_hits);
  w.extent_cache_misses = Counter(stats.extent_cache_misses);
  w.directory_cache_hits = Counter(stats.directory_cache_hits);
  w.directory_cache_misses = Counter(stats.directory_cache_misses);
  w.lock_wait_ns = Counter(stats.lock_wait_ns);
  return w;
}

std::string JsonEscape(const std::string& value) {
  std::string out;
  for (char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

void EmitWorkload(std::ostream& out, const Workload& w, bool last) {
  const double seconds = static_cast<double>(w.ns) / 1e9;
  const double mbps = seconds > 0 ? (static_cast<double>(w.bytes) / (1 << 20)) /
                                        seconds
                                  : 0.0;
  const double iops = seconds > 0 ? static_cast<double>(w.ops) / seconds : 0.0;
  out << "    {\"name\":\"" << JsonEscape(w.name) << "\","
      << "\"ns\":" << w.ns << ",\"bytes\":" << w.bytes
      << ",\"ops\":" << w.ops << ",\"mb_per_s\":" << mbps
      << ",\"iops\":" << iops
      << ",\"gate_ok\":" << (w.gate_ok ? "true" : "false")
      << ",\"gate_failure\":\"" << JsonEscape(w.gate_failure) << "\","
      << "\"filesystem\":{\"read_calls\":" << w.read_calls
      << ",\"read_bytes\":" << w.read_bytes
      << ",\"write_calls\":" << w.write_calls
      << ",\"write_bytes\":" << w.write_bytes
      << ",\"flush_calls\":" << w.flush_calls << "},"
      << "\"device\":{\"read_calls\":" << w.device_read_calls
      << ",\"read_bytes\":" << w.device_read_bytes
      << ",\"write_calls\":" << w.device_write_calls
      << ",\"write_bytes\":" << w.device_write_bytes << "},"
      << "\"journal\":{\"commits\":" << w.journal_commits
      << ",\"bytes\":" << w.journal_bytes << "},"
      << "\"caches\":{\"inode_hits\":" << w.inode_cache_hits
      << ",\"inode_misses\":" << w.inode_cache_misses
      << ",\"extent_hits\":" << w.extent_cache_hits
      << ",\"extent_misses\":" << w.extent_cache_misses
      << ",\"directory_hits\":" << w.directory_cache_hits
      << ",\"directory_misses\":" << w.directory_cache_misses << "},"
      << "\"concurrency\":{\"max_reads\":" << w.max_concurrent_reads
      << ",\"max_writes\":" << w.max_concurrent_writes << "},"
      << "\"lock_wait_ns\":" << w.lock_wait_ns << "}"
      << (last ? "\n" : ",\n");
}

std::string Bytes(uint64_t value) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(value) /
                                                     (1 << 20));
  return buf;
}

bool Gate(Workload& w, bool condition, const std::string& message) {
  if (condition) return true;
  if (w.gate_ok) {
    w.gate_ok = false;
    w.gate_failure = message;
  }
  return false;
}

xfs::Result<uint64_t> EnsureBenchFile(xfs::XfsFilesystem& filesystem,
                                      const std::string& name) {
  auto entry = filesystem.LookupName(filesystem.RootInode(), name);
  if (entry.ok()) return entry.value().ino;
  return filesystem.CreateRegularFile(filesystem.RootInode(), name, 0644, 0, 0);
}

void PrintWorkload(const Workload& w) {
  const double seconds = static_cast<double>(w.ns) / 1e9;
  const double mbps = seconds > 0 ? (static_cast<double>(w.bytes) / (1 << 20)) /
                                        seconds
                                  : 0.0;
  std::printf("%-28s %8.2f MiB/s  %10.3f s  bytes=%s dev_r=%llu dev_w=%llu "
              "commits=%llu max_w=%llu %s\n",
              w.name.c_str(), mbps, seconds, Bytes(w.bytes).c_str(),
              static_cast<unsigned long long>(w.device_read_calls),
              static_cast<unsigned long long>(w.device_write_calls),
              static_cast<unsigned long long>(w.journal_commits),
              static_cast<unsigned long long>(w.max_concurrent_writes),
              w.gate_ok ? "gate=ok" : "gate=FAIL");
  if (!w.gate_ok) {
    std::printf("    gate failure: %s\n", w.gate_failure.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string image;
  std::string json_path;
  std::string label = "perf1";
  int small_files = 1000;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--image" && i + 1 < argc) {
      image = argv[++i];
    } else if (arg == "--json" && i + 1 < argc) {
      json_path = argv[++i];
    } else if (arg == "--label" && i + 1 < argc) {
      label = argv[++i];
    } else if (arg == "--small-files" && i + 1 < argc) {
      small_files = std::max(1, std::atoi(argv[++i]));
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      return 2;
    }
  }
  if (image.empty()) {
    std::fprintf(stderr,
                 "usage: xfswin_perf --image <formatted.img> [--json out] "
                 "[--label name] [--small-files N]\n");
    return 2;
  }

  xfs::log::SetLevel(xfs::log::Level::kWarn);
  xfs::PerfStats& stats = xfs::PerfStats::Get();
  stats.SetEnabled(true);
  stats.Reset();

  std::string error;
  const auto* image_path = reinterpret_cast<const char8_t*>(image.data());
  auto device = xfs::ImageBlockDevice::Open(
      fs::path(std::u8string(image_path, image.size())), /*writable=*/true, 512,
      &error);
  if (!device) {
    std::fprintf(stderr, "cannot open %s: %s\n", image.c_str(), error.c_str());
    return 1;
  }
  xfs::MountOptions options;
  options.read_write = true;
  options.perf_stats = true;
  auto mounted = xfs::XfsFilesystem::Mount(std::move(device), options);
  if (!mounted.ok()) {
    std::fprintf(stderr, "cannot mount: %s\n",
                 mounted.error().Format().c_str());
    return 1;
  }
  xfs::XfsFilesystem& filesystem = *mounted.value();
  const auto& geo = filesystem.geometry();
  const uint64_t block_size = geo.block_size;

  std::vector<Workload> results;

  // ------ sequential new-file write (1 MiB callbacks) ------
  auto bench_ino = EnsureBenchFile(filesystem, "perf-seq.bin");
  if (!bench_ino.ok()) {
    std::fprintf(stderr, "cannot create benchmark file\n");
    return 1;
  }
  constexpr uint64_t kChunk = 1u << 20;
  constexpr uint64_t kLargeBytes = 8u << 20;
  std::vector<uint8_t> data(kLargeBytes);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>((i * 131u) ^ (i >> 7));
  }
  ResetCounters(stats);
  auto start = Clock::now();
  uint64_t offset = 0;
  bool write_ok = true;
  while (offset < kLargeBytes) {
    const uint64_t length = std::min<uint64_t>(kChunk, kLargeBytes - offset);
    auto wrote = filesystem.WriteFileData(bench_ino.value(), offset,
                                          data.data() + offset, length);
    if (!wrote.ok()) {
      std::fprintf(stderr, "write failed: %s\n",
                   wrote.error().Format().c_str());
      write_ok = false;
      break;
    }
    offset += length;
  }
  Workload seq_new = Snapshot(
      "sequential_new_write_8mib", static_cast<uint64_t>(
                                      std::chrono::duration_cast<
                                          std::chrono::nanoseconds>(
                                          Clock::now() - start)
                                          .count()),
      kLargeBytes, kLargeBytes / kChunk, stats);
  Gate(seq_new, write_ok, "write failed");
  Gate(seq_new, seq_new.device_write_calls <=
                    (kLargeBytes / kChunk) * 12 + 32,
       "new-file write used too many device writes");
  results.push_back(seq_new);

  // ------ sequential read (1 MiB requests) ------
  std::vector<uint8_t> readback(kLargeBytes);
  filesystem.Flush();  // make the workload boundary explicit
  ResetCounters(stats);
  start = Clock::now();
  uint64_t read_offset = 0;
  bool read_ok = true;
  while (read_offset < kLargeBytes) {
    const uint64_t length =
        std::min<uint64_t>(kChunk, kLargeBytes - read_offset);
    auto read = filesystem.ReadFile(bench_ino.value(), read_offset,
                                    readback.data() + read_offset, length);
    if (!read.ok() || read.value() != length) {
      read_ok = false;
      break;
    }
    read_offset += length;
  }
  Workload seq_read = Snapshot(
      "sequential_read_8mib", static_cast<uint64_t>(
                                   std::chrono::duration_cast<
                                       std::chrono::nanoseconds>(
                                       Clock::now() - start)
                                       .count()),
      kLargeBytes, kLargeBytes / kChunk, stats);
  Gate(seq_read, read_ok, "read failed or returned a short buffer");
  Gate(seq_read, readback == data, "readback content mismatch");
  auto extents = filesystem.GetDataExtents(bench_ino.value());
  const size_t extent_count = extents.ok() ? extents.value().size() : 1;
  Gate(seq_read, seq_read.device_read_calls <=
                    (kLargeBytes / kChunk) + extent_count + 4,
       "8 MiB read used more device reads than physical extents");
  Gate(seq_read, seq_read.device_read_bytes <= kLargeBytes + (1u << 20),
       "read amplification above 1.125x");
  results.push_back(seq_read);

  // ------ sequential overwrite of the same file ------
  ResetCounters(stats);
  start = Clock::now();
  offset = 0;
  write_ok = true;
  while (offset < kLargeBytes) {
    const uint64_t length = std::min<uint64_t>(kChunk, kLargeBytes - offset);
    auto wrote = filesystem.WriteFileData(bench_ino.value(), offset,
                                          data.data() + offset, length);
    if (!wrote.ok()) {
      write_ok = false;
      break;
    }
    offset += length;
  }
  Workload seq_overwrite = Snapshot(
      "sequential_overwrite_8mib", static_cast<uint64_t>(
                                       std::chrono::duration_cast<
                                           std::chrono::nanoseconds>(
                                           Clock::now() - start)
                                           .count()),
      kLargeBytes, kLargeBytes / kChunk, stats);
  Gate(seq_overwrite, write_ok, "overwrite failed");
  Gate(seq_overwrite,
       seq_overwrite.device_write_calls <= (kLargeBytes / kChunk) * 2 + 16,
       "1 MiB overwrite issued too many device writes");
  results.push_back(seq_overwrite);

  // ------ four independent files / four writers ------
  constexpr int kFiles = 4;
  constexpr uint64_t kConcurrentBytes = 4u << 20;
  std::vector<uint64_t> inos;
  bool setup_ok = true;
  for (int i = 0; i < kFiles; ++i) {
    auto ino = EnsureBenchFile(filesystem, "perf-conc-" + std::to_string(i));
    if (!ino.ok()) {
      setup_ok = false;
      break;
    }
    inos.push_back(ino.value());
    auto pre = filesystem.AllocateUnwritten(ino.value(), 0, kConcurrentBytes,
                                            /*keep_size=*/true);
    if (!pre.ok()) {
      setup_ok = false;
      break;
    }
  }
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::vector<uint8_t> concurrent_data(kConcurrentBytes);
  for (size_t i = 0; i < concurrent_data.size(); ++i) {
    concurrent_data[i] = static_cast<uint8_t>(i ^ (i >> 8));
  }
  ResetCounters(stats);
  start = Clock::now();
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  if (setup_ok) {
    for (int i = 0; i < kFiles; ++i) {
      threads.emplace_back([&, i]() {
        ready.fetch_add(1);
        while (!go.load()) std::this_thread::yield();
        auto wrote = filesystem.WriteFileData(
            inos[static_cast<size_t>(i)], 0, concurrent_data.data(),
            concurrent_data.size());
        if (!wrote.ok()) failures.fetch_add(1);
      });
    }
    while (ready.load() < kFiles) std::this_thread::yield();
    go.store(true);
    for (auto& thread : threads) thread.join();
  }
  Workload concurrent = Snapshot(
      "four_file_concurrent_write_16mib", static_cast<uint64_t>(
                                             std::chrono::duration_cast<
                                                 std::chrono::nanoseconds>(
                                                 Clock::now() - start)
                                                 .count()),
      kConcurrentBytes * kFiles, kFiles, stats);
  bool concurrent_content_ok = setup_ok;
  std::vector<uint8_t> concurrent_readback(kConcurrentBytes);
  for (int i = 0; i < kFiles && concurrent_content_ok; ++i) {
    auto read = filesystem.ReadFile(inos[static_cast<size_t>(i)], 0,
                                    concurrent_readback.data(),
                                    concurrent_readback.size());
    concurrent_content_ok = read.ok() &&
                            read.value() == concurrent_readback.size() &&
                            concurrent_readback == concurrent_data;
  }
  Gate(concurrent, setup_ok && failures.load() == 0, "concurrent write failed");
  Gate(concurrent, concurrent_content_ok, "concurrent write content mismatch");
  Gate(concurrent, concurrent.max_concurrent_writes >= 2,
       "independent files did not overlap in the core write path");
  Gate(concurrent, concurrent.device_write_calls <= kFiles * 40,
       "concurrent write used too many device writes");
  results.push_back(concurrent);

  // ------ small-file workload ------
  auto small_dir = filesystem.LookupName(filesystem.RootInode(), "perf-small");
  uint64_t small_dir_ino = 0;
  if (small_dir.ok()) {
    small_dir_ino = small_dir.value().ino;
  } else {
    auto made = filesystem.Mkdir(filesystem.RootInode(), "perf-small", 0755, 0,
                                 0);
    if (!made.ok()) {
      std::fprintf(stderr, "cannot create small-file directory\n");
      return 1;
    }
    auto entry = filesystem.LookupName(filesystem.RootInode(), "perf-small");
    if (!entry.ok()) return 1;
    small_dir_ino = entry.value().ino;
  }
  const std::vector<uint8_t> small_data(4096, 0x5a);
  ResetCounters(stats);
  start = Clock::now();
  bool small_ok = true;
  for (int i = 0; i < small_files && small_ok; ++i) {
    const std::string name = "f" + std::to_string(i) + ".bin";
    auto created = filesystem.CreateRegularFile(small_dir_ino, name, 0644, 0, 0);
    if (!created.ok()) {
      small_ok = false;
      break;
    }
    auto wrote = filesystem.WriteFileData(created.value(), 0, small_data.data(),
                                          small_data.size());
    if (!wrote.ok()) {
      small_ok = false;
      break;
    }
    std::vector<uint8_t> verify(small_data.size());
    auto read = filesystem.ReadFile(created.value(), 0, verify.data(),
                                    verify.size());
    if (!read.ok() || read.value() != verify.size() || verify != small_data) {
      small_ok = false;
      break;
    }
  }
  Workload small = Snapshot(
      "small_file_create_write_read_" + std::to_string(small_files),
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - start)
                                .count()),
      static_cast<uint64_t>(small_files) * small_data.size(),
      static_cast<uint64_t>(small_files) * 3, stats);
  Gate(small, small_ok, "small-file workload failed");
  results.push_back(small);

  // Warm and then re-enumerate the directory (cache gate).
  auto warm = filesystem.ListDirectory(small_dir_ino);
  ResetCounters(stats);
  start = Clock::now();
  auto listed = filesystem.ListDirectory(small_dir_ino);
  Workload dir = Snapshot(
      "directory_warm_" + std::to_string(small_files),
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - start)
                                .count()),
      0, listed.ok() ? listed.value().size() : 0, stats);
  Gate(dir, warm.ok() && listed.ok(), "directory enumeration failed");
  Gate(dir, dir.directory_cache_hits >= 1 && dir.directory_cache_misses == 0,
       "warm directory enumeration missed the cache");
  results.push_back(dir);

  // Explicit durability barrier.
  ResetCounters(stats);
  start = Clock::now();
  auto flushed = filesystem.Flush();
  Workload flush = Snapshot(
      "flush_barrier",
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - start)
                                .count()),
      0, 1, stats);
  Gate(flush, flushed.ok() && flush.flush_calls == 1,
       "explicit flush did not reach the device");
  results.push_back(flush);

  // ------ report ------
  std::printf("PERF-1 core benchmark label=%s block=%llu agcount=%u\n",
              label.c_str(), static_cast<unsigned long long>(block_size),
              geo.agcount);
  bool all_ok = true;
  for (const Workload& w : results) {
    PrintWorkload(w);
    if (!w.gate_ok) all_ok = false;
  }
  std::printf("%s\n", all_ok ? "all deterministic gates passed"
                             : "DETERMINISTIC GATE FAILURE");

  std::ostringstream json;
  json << "{\n"
       << "  \"schema\":\"xfswin-perf-run-1\",\n"
       << "  \"label\":\"" << JsonEscape(label) << "\",\n"
       << "  \"image\":\"" << JsonEscape(image) << "\",\n"
       << "  \"block_size\":" << block_size << ",\n"
       << "  \"agcount\":" << geo.agcount << ",\n"
       << "  \"hardware_concurrency\":"
       << std::thread::hardware_concurrency() << ",\n"
       << "  \"workloads\":[\n";
  for (size_t i = 0; i < results.size(); ++i) {
    EmitWorkload(json, results[i], i + 1 == results.size());
  }
  json << "  ],\n"
       << "  \"all_gates_passed\":" << (all_ok ? "true" : "false") << "\n"
       << "}\n";
  if (!json_path.empty()) {
    std::ofstream out(json_path, std::ios::binary | std::ios::trunc);
    out << json.str();
  }
  std::fputs(json.str().c_str(), stdout);

  // Clean close: a clean unmount record makes the image reusable by the next
  // run and by Linux oracles.
  filesystem.WriteCleanUnmountRecord();
  return all_ok ? 0 : 1;
}
