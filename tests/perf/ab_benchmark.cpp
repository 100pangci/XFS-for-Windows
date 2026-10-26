// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// PERF-1 A/B benchmark (baseline-compatible).
//
// This tool deliberately uses only APIs that exist before PERF-1 and measures
// wall time plus device I/O through its own counting BlockDevice wrapper. That
// makes it possible to build the same source against an older commit (see
// tests/perf/standalone/CMakeLists.txt and run_ab.ps1) and compare results on
// the same machine without linking the PERF-1 instrumentation.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/filesystem.h"
#include "xfs/image_device.h"
#include "xfs/log.h"

namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

class CountingDevice final : public xfs::BlockDevice {
 public:
  explicit CountingDevice(std::unique_ptr<xfs::BlockDevice> inner)
      : inner_(std::move(inner)) {}

  bool Read(uint64_t offset, void* buffer, size_t size) override {
    const uint64_t now = active_reads_.fetch_add(1) + 1;
    UpdatePeak(max_reads_, now);
    read_calls_.fetch_add(1);
    const bool ok = inner_->Read(offset, buffer, size);
    if (ok) read_bytes_.fetch_add(size);
    active_reads_.fetch_sub(1);
    return ok;
  }
  bool Write(uint64_t offset, const void* buffer, size_t size) override {
    const uint64_t now = active_writes_.fetch_add(1) + 1;
    UpdatePeak(max_writes_, now);
    write_calls_.fetch_add(1);
    if (size >= 65536) large_write_calls_.fetch_add(1);
    const bool ok = inner_->Write(offset, buffer, size);
    if (ok) write_bytes_.fetch_add(size);
    active_writes_.fetch_sub(1);
    return ok;
  }
  bool Flush() override {
    flush_calls_.fetch_add(1);
    return inner_->Flush();
  }
  uint64_t Size() const override { return inner_->Size(); }
  uint32_t SectorSize() const override { return inner_->SectorSize(); }
  const xfs::DeviceInfo& Info() const override { return inner_->Info(); }
  void Close() override { inner_->Close(); }

  void Reset() {
    read_calls_ = 0;
    read_bytes_ = 0;
    write_calls_ = 0;
    write_bytes_ = 0;
    large_write_calls_ = 0;
    flush_calls_ = 0;
    max_reads_ = 0;
    max_writes_ = 0;
  }
  uint64_t read_calls() const { return read_calls_.load(); }
  uint64_t read_bytes() const { return read_bytes_.load(); }
  uint64_t write_calls() const { return write_calls_.load(); }
  uint64_t write_bytes() const { return write_bytes_.load(); }
  uint64_t large_write_calls() const { return large_write_calls_.load(); }
  uint64_t flush_calls() const { return flush_calls_.load(); }
  uint64_t max_concurrent_reads() const { return max_reads_.load(); }
  uint64_t max_concurrent_writes() const { return max_writes_.load(); }

 private:
  static void UpdatePeak(std::atomic<uint64_t>& peak, uint64_t value) {
    uint64_t current = peak.load();
    while (value > current && !peak.compare_exchange_weak(current, value)) {
    }
  }

  std::unique_ptr<xfs::BlockDevice> inner_;
  std::atomic<uint64_t> read_calls_{0};
  std::atomic<uint64_t> read_bytes_{0};
  std::atomic<uint64_t> write_calls_{0};
  std::atomic<uint64_t> write_bytes_{0};
  std::atomic<uint64_t> large_write_calls_{0};
  std::atomic<uint64_t> flush_calls_{0};
  std::atomic<uint64_t> active_reads_{0};
  std::atomic<uint64_t> active_writes_{0};
  std::atomic<uint64_t> max_reads_{0};
  std::atomic<uint64_t> max_writes_{0};
};

struct Result {
  std::string name;
  uint64_t ns = 0;
  uint64_t bytes = 0;
  uint64_t ops = 0;
  uint64_t dev_read_calls = 0;
  uint64_t dev_read_bytes = 0;
  uint64_t dev_write_calls = 0;
  uint64_t dev_write_bytes = 0;
  uint64_t large_write_calls = 0;
  uint64_t max_reads = 0;
  uint64_t max_writes = 0;
};

std::string JsonEscape(const std::string& value) {
  std::string out;
  for (char c : value) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += ' ';
    } else {
      out.push_back(c);
    }
  }
  return out;
}

Result Capture(const std::string& name, uint64_t ns, uint64_t bytes,
               uint64_t ops, const CountingDevice& device) {
  Result result;
  result.name = name;
  result.ns = ns;
  result.bytes = bytes;
  result.ops = ops;
  result.dev_read_calls = device.read_calls();
  result.dev_read_bytes = device.read_bytes();
  result.dev_write_calls = device.write_calls();
  result.dev_write_bytes = device.write_bytes();
  result.large_write_calls = device.large_write_calls();
  result.max_reads = device.max_concurrent_reads();
  result.max_writes = device.max_concurrent_writes();
  return result;
}

// The production WinFsp adapter defers the timestamp transaction for ordinary
// overwrites until Cleanup/Flush. The PERF-1 build enables that path here so
// the A/B result reflects what a mounted filesystem actually does; the
// baseline build compiles the legacy call (no extra argument).
bool WriteFile(xfs::XfsFilesystem& filesystem, uint64_t ino, uint64_t offset,
               const void* data, size_t length) {
#ifdef XFSWIN_HAVE_DEFERRED_TIMESTAMPS
  return filesystem
      .WriteFileData(ino, offset, data, length, /*append=*/false,
                     /*defer_timestamps=*/true)
      .ok();
#else
  return filesystem.WriteFileData(ino, offset, data, length).ok();
#endif
}

xfs::Result<uint64_t> EnsureFile(xfs::XfsFilesystem& filesystem,
                                 const std::string& name) {
  auto entry = filesystem.LookupName(filesystem.RootInode(), name);
  if (entry.ok()) return entry.value().ino;
  return filesystem.CreateRegularFile(filesystem.RootInode(), name, 0644, 0, 0);
}

void Emit(std::ostream& out, const Result& r, bool last) {
  const double seconds = static_cast<double>(r.ns) / 1e9;
  const double mbps =
      seconds > 0 ? (static_cast<double>(r.bytes) / (1 << 20)) / seconds : 0;
  out << "    {\"name\":\"" << JsonEscape(r.name) << "\",\"ns\":" << r.ns
      << ",\"bytes\":" << r.bytes << ",\"ops\":" << r.ops
      << ",\"mb_per_s\":" << mbps
      << ",\"device\":{\"read_calls\":" << r.dev_read_calls
      << ",\"read_bytes\":" << r.dev_read_bytes
      << ",\"write_calls\":" << r.dev_write_calls
      << ",\"write_bytes\":" << r.dev_write_bytes
      << ",\"large_write_calls\":" << r.large_write_calls << "},"
      << "\"max_concurrent_reads\":" << r.max_reads
      << ",\"max_concurrent_writes\":" << r.max_writes << "}"
      << (last ? "\n" : ",\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string image;
  std::string json_path;
  std::string label = "bench";
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
                 "usage: ab_benchmark --image <formatted.img> [--json out] "
                 "[--label name] [--small-files N]\n");
    return 2;
  }

  xfs::log::SetLevel(xfs::log::Level::kWarn);
  std::string error;
  const auto* image_path = reinterpret_cast<const char8_t*>(image.data());
  auto opened = xfs::ImageBlockDevice::Open(
      fs::path(std::u8string(image_path, image.size())), /*writable=*/true, 512,
      &error);
  if (!opened) {
    std::fprintf(stderr, "cannot open %s: %s\n", image.c_str(), error.c_str());
    return 1;
  }
  auto counted = std::make_unique<CountingDevice>(std::move(opened));
  CountingDevice* device = counted.get();
  xfs::MountOptions options;
  options.read_write = true;
  auto mounted = xfs::XfsFilesystem::Mount(std::move(counted), options);
  if (!mounted.ok()) {
    std::fprintf(stderr, "cannot mount: %s\n",
                 mounted.error().Format().c_str());
    return 1;
  }
  xfs::XfsFilesystem& filesystem = *mounted.value();

  constexpr uint64_t kChunk = 1u << 20;
  constexpr uint64_t kLarge = 8u << 20;
  std::vector<uint8_t> data(kLarge);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>((i * 131u) ^ (i >> 7));
  }
  std::vector<Result> results;

  auto bench = EnsureFile(filesystem, "ab-seq.bin");
  if (!bench.ok()) return 1;

  device->Reset();
  auto start = Clock::now();
  for (uint64_t offset = 0; offset < kLarge; offset += kChunk) {
    if (!WriteFile(filesystem, bench.value(), offset, data.data() + offset,
                   kChunk)) {
      return 1;
    }
  }
  results.push_back(Capture(
      "sequential_new_write_8mib",
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - start)
                                .count()),
      kLarge, kLarge / kChunk, *device));

  std::vector<uint8_t> readback(kLarge);
  filesystem.Flush();
  device->Reset();
  start = Clock::now();
  for (uint64_t offset = 0; offset < kLarge; offset += kChunk) {
    auto read = filesystem.ReadFile(bench.value(), offset,
                                    readback.data() + offset, kChunk);
    if (!read.ok() || read.value() != kChunk) return 1;
  }
  results.push_back(Capture(
      "sequential_read_8mib",
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - start)
                                .count()),
      kLarge, kLarge / kChunk, *device));

  device->Reset();
  start = Clock::now();
  for (uint64_t offset = 0; offset < kLarge; offset += kChunk) {
    if (!WriteFile(filesystem, bench.value(), offset, data.data() + offset,
                   kChunk)) {
      return 1;
    }
  }
  results.push_back(Capture(
      "sequential_overwrite_8mib",
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - start)
                                .count()),
      kLarge, kLarge / kChunk, *device));

  constexpr int kFiles = 4;
  constexpr uint64_t kConcurrent = 4u << 20;
  std::vector<uint64_t> inos;
  for (int i = 0; i < kFiles; ++i) {
    auto ino = EnsureFile(filesystem, "ab-conc-" + std::to_string(i));
    if (!ino.ok()) return 1;
    inos.push_back(ino.value());
    for (uint64_t offset = 0; offset < kConcurrent; offset += kChunk) {
      if (!WriteFile(filesystem, ino.value(), offset, data.data(), kChunk)) {
        return 1;
      }
    }
  }
  device->Reset();
  start = Clock::now();
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kFiles; ++i) {
    threads.emplace_back([&, i]() {
      ready.fetch_add(1);
      while (!go.load()) std::this_thread::yield();
      for (uint64_t offset = 0; offset < kConcurrent; offset += kChunk) {
        if (!WriteFile(filesystem, inos[static_cast<size_t>(i)], offset,
                       data.data(), kChunk)) {
          failures.fetch_add(1);
        }
      }
    });
  }
  while (ready.load() < kFiles) std::this_thread::yield();
  go.store(true);
  for (auto& thread : threads) thread.join();
  if (failures.load() != 0) return 1;
  results.push_back(Capture(
      "four_file_concurrent_write_16mib",
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - start)
                                .count()),
      kConcurrent * kFiles, kFiles, *device));
  // Content check outside the timed window (positioned concurrent I/O must not
  // corrupt independent offsets).
  std::vector<uint8_t> concurrent_read(kConcurrent);
  for (int i = 0; i < kFiles; ++i) {
    auto read = filesystem.ReadFile(inos[static_cast<size_t>(i)], 0,
                                    concurrent_read.data(),
                                    concurrent_read.size());
    if (!read.ok() || read.value() != concurrent_read.size() ||
        !std::equal(concurrent_read.begin(), concurrent_read.end(),
                    data.begin())) {
      return 1;
    }
  }

  // Small-file workload: create 4 KiB, read back, unlink. Each operation is a
  // separate production API call, exactly what the WinFsp callbacks do.
  auto small_dir = filesystem.LookupName(filesystem.RootInode(), "ab-small");
  uint64_t small_ino = 0;
  if (small_dir.ok()) {
    small_ino = small_dir.value().ino;
  } else {
    auto made = filesystem.Mkdir(filesystem.RootInode(), "ab-small", 0755, 0, 0);
    if (!made.ok()) return 1;
    auto entry = filesystem.LookupName(filesystem.RootInode(), "ab-small");
    if (!entry.ok()) return 1;
    small_ino = entry.value().ino;
  }
  std::vector<uint8_t> small_data(4096, 0x5a);
  std::vector<uint8_t> small_read(4096);
  device->Reset();
  start = Clock::now();
  for (int i = 0; i < small_files; ++i) {
    const std::string name = "f" + std::to_string(i) + ".bin";
    auto created = filesystem.CreateRegularFile(small_ino, name, 0644, 0, 0);
    if (!created.ok()) return 1;
    if (!WriteFile(filesystem, created.value(), 0, small_data.data(),
                   small_data.size())) {
      return 1;
    }
    auto read = filesystem.ReadFile(created.value(), 0, small_read.data(),
                                    small_read.size());
    if (!read.ok() || small_read != small_data) return 1;
    auto removed = filesystem.Unlink(small_ino, name, /*defer_inode_free=*/false);
    if (!removed.ok()) return 1;
  }
  results.push_back(Capture(
      "small_file_create_write_read_delete_" + std::to_string(small_files),
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - start)
                                .count()),
      static_cast<uint64_t>(small_files) * 4096,
      static_cast<uint64_t>(small_files) * 4, *device));

  auto warm = filesystem.ListDirectory(small_ino);
  device->Reset();
  start = Clock::now();
  auto listed = filesystem.ListDirectory(small_ino);
  results.push_back(Capture(
      "directory_warm_list",
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - start)
                                .count()),
      0, listed.ok() ? listed.value().size() : 0, *device));

  std::printf("PERF-1 A/B benchmark label=%s\n", label.c_str());
  for (const Result& r : results) {
    const double seconds = static_cast<double>(r.ns) / 1e9;
    const double mbps =
        seconds > 0 ? (static_cast<double>(r.bytes) / (1 << 20)) / seconds : 0;
    std::printf("%-44s %9.2f MiB/s  dev_r=%-7llu dev_w=%-7llu "
                "large_w=%-6llu max_w=%llu\n",
                r.name.c_str(), mbps,
                static_cast<unsigned long long>(r.dev_read_calls),
                static_cast<unsigned long long>(r.dev_write_calls),
                static_cast<unsigned long long>(r.large_write_calls),
                static_cast<unsigned long long>(r.max_writes));
  }

  std::ostringstream json;
  json << "{\n  \"schema\":\"xfswin-perf-ab-1\",\n"
       << "  \"label\":\"" << JsonEscape(label) << "\",\n"
       << "  \"small_files\":" << small_files << ",\n"
       << "  \"hardware_concurrency\":" << std::thread::hardware_concurrency()
       << ",\n  \"workloads\":[\n";
  for (size_t i = 0; i < results.size(); ++i) {
    Emit(json, results[i], i + 1 == results.size());
  }
  json << "  ]\n}\n";
  if (!json_path.empty()) {
    std::ofstream out(json_path, std::ios::binary | std::ios::trunc);
    out << json.str();
  }
  std::fputs(json.str().c_str(), stdout);

  filesystem.WriteCleanUnmountRecord();
  return 0;
}
