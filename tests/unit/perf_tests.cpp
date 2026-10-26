// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// PERF-1 deterministic performance gates.
//
// These tests do not assert MB/s (hosted runners are noisy). They assert the
// properties that make throughput possible and are stable everywhere:
//   - a 1 MiB contiguous read/write uses a handful of device calls, not one
//     per filesystem block;
//   - independent inodes really overlap in the core write path;
//   - the same inode still serializes;
//   - allocation-size hints create unwritten extents without moving EOF;
//   - warm directory enumeration hits the cache.
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "support/fixture_device.h"
#include "support/fixture_image.h"
#include "test_util.h"
#include "xfs/filesystem.h"
#include "xfs/perf_stats.h"

namespace {

using xfstest::FixtureImage;
using xfstest::MemoryBlockDevice;

MemoryBlockDevice& Memory() {
  return static_cast<MemoryBlockDevice&>(FixtureImage::Instance().storage());
}

void EnablePerf() {
  xfs::PerfStats::Get().SetEnabled(true);
  xfs::PerfStats::Get().Reset();
}

std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>((i * 131u + seed) ^ (i >> 9));
  }
  return data;
}

}  // namespace

TEST(perf_contiguous_1mib_read_uses_few_device_calls) {
  FixtureImage::Instance().Reset();
  auto fs = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(fs.ok());
  const uint64_t ino = FixtureImage::Instance().info().file_ino;
  auto content = Pattern(1u << 20, 0x3c);
  auto wrote = fs.value()->WriteFileData(ino, 0, content.data(), content.size());
  CHECK(wrote.ok());

  std::vector<uint8_t> readback(content.size());
  // Warm inode + extent caches; the gate is about data transfer calls.
  auto warm = fs.value()->ReadFile(ino, 0, readback.data(), readback.size());
  CHECK(warm.ok());
  CHECK_EQ(warm.value(), content.size());

  EnablePerf();
  Memory().ResetCounters();
  auto read = fs.value()->ReadFile(ino, 0, readback.data(), readback.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), content.size());
  CHECK(readback == content);
  // At most a partial old extent plus the newly allocated run; certainly not
  // 256 per-block reads.
  CHECK_MESSAGE(Memory().read_calls() <= 4,
                "1 MiB read issued " +
                    std::to_string(Memory().read_calls()) +
                    " device reads");
  CHECK(Memory().large_read_calls() >= 1);
  CHECK(xfs::PerfStats::Get().max_concurrent_reads.load() >= 1);
}

TEST(perf_1mib_overwrite_coalesces_device_writes) {
  FixtureImage::Instance().Reset();
  auto fs = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(fs.ok());
  const uint64_t ino = FixtureImage::Instance().info().file_ino;
  auto content = Pattern(1u << 20, 0x71);
  auto first = fs.value()->WriteFileData(ino, 0, content.data(), content.size());
  CHECK(first.ok());

  // Now the whole range is allocated and written: this is the ordinary
  // overwrite hot path and must become roughly one large positioned write.
  EnablePerf();
  Memory().ResetCounters();
  auto again = fs.value()->WriteFileData(ino, 0, content.data(), content.size());
  CHECK(again.ok());
  CHECK_MESSAGE(Memory().large_write_calls() <= 2,
                "1 MiB overwrite issued " +
                    std::to_string(Memory().large_write_calls()) +
                    " large device writes");
  CHECK(Memory().write_bytes() < (2u << 20));
  CHECK(xfs::PerfStats::Get().journal_commits.load() >= 1);
}

TEST(perf_preallocation_keeps_eof_and_reads_zero) {
  FixtureImage::Instance().Reset();
  auto fs = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(fs.ok());
  auto created = fs.value()->CreateRegularFile(fs.value()->RootInode(),
                                               "prealloc.bin", 0644, 0, 0);
  CHECK(created.ok());
  const uint64_t ino = created.value();
  constexpr uint64_t kSize = 1u << 20;
  auto pre = fs.value()->AllocateUnwritten(ino, 0, kSize, /*keep_size=*/true);
  CHECK(pre.ok());
  CHECK_EQ(pre.value().size, 0u);
  CHECK(pre.value().nblocks >= kSize / fs.value()->geometry().block_size);

  std::vector<uint8_t> buffer(kSize, 0xa5);
  auto read = fs.value()->ReadFile(ino, 0, buffer.data(), buffer.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), 0u);

  auto data = Pattern(4096, 0x19);
  auto wrote = fs.value()->WriteFileData(ino, 0, data.data(), data.size());
  CHECK(wrote.ok());
  auto info = fs.value()->GetFileInfo(ino);
  CHECK(info.ok());
  CHECK_EQ(info.value().size, data.size());
  // The unwritten range beyond EOF is not exposed as data but stays allocated.
  CHECK(info.value().nblocks >= kSize / fs.value()->geometry().block_size);
}

TEST(perf_independent_inodes_write_concurrently) {
  FixtureImage::Instance().Reset();
  auto fs = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(fs.ok());
  constexpr uint64_t kSize = 512u << 10;
  auto a = fs.value()->CreateRegularFile(fs.value()->RootInode(), "conc-a.bin",
                                         0644, 0, 0);
  auto b = fs.value()->CreateRegularFile(fs.value()->RootInode(), "conc-b.bin",
                                         0644, 0, 0);
  CHECK(a.ok());
  CHECK(b.ok());
  auto content = Pattern(kSize, 0x2b);
  CHECK(fs.value()->WriteFileData(a.value(), 0, content.data(), kSize).ok());
  CHECK(fs.value()->WriteFileData(b.value(), 0, content.data(), kSize).ok());

  Memory().SetWriteDelay(std::chrono::microseconds(2000));
  EnablePerf();
  Memory().ResetCounters();
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  auto writer = [&](uint64_t ino) {
    ready.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    auto result = fs.value()->WriteFileData(ino, 0, content.data(), kSize);
    CHECK(result.ok());
  };
  std::thread ta(writer, a.value());
  std::thread tb(writer, b.value());
  while (ready.load() < 2) std::this_thread::yield();
  go.store(true);
  ta.join();
  tb.join();
  Memory().SetWriteDelay(std::chrono::microseconds(0));

  CHECK_MESSAGE(xfs::PerfStats::Get().max_concurrent_writes.load() >= 2,
                "independent inode writes did not overlap in the core path");
  CHECK_MESSAGE(Memory().max_concurrent_writes() >= 2,
                "independent inode writes did not overlap at the device");
  CHECK(xfs::PerfStats::Get().write_calls.load() >= 2);
}

TEST(perf_same_inode_writes_still_serialize) {
  FixtureImage::Instance().Reset();
  auto fs = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(fs.ok());
  auto created = fs.value()->CreateRegularFile(fs.value()->RootInode(),
                                               "same.bin", 0644, 0, 0);
  CHECK(created.ok());
  const uint64_t ino = created.value();
  constexpr uint64_t kSize = 512u << 10;
  auto content = Pattern(kSize, 0x55);
  CHECK(fs.value()->WriteFileData(ino, 0, content.data(), kSize).ok());

  Memory().SetWriteDelay(std::chrono::microseconds(2000));
  EnablePerf();
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  auto writer = [&](uint64_t offset) {
    ready.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    auto result = fs.value()->WriteFileData(ino, offset, content.data(),
                                            kSize / 2);
    CHECK(result.ok());
  };
  std::thread ta(writer, 0);
  std::thread tb(writer, kSize / 2);
  while (ready.load() < 2) std::this_thread::yield();
  go.store(true);
  ta.join();
  tb.join();
  Memory().SetWriteDelay(std::chrono::microseconds(0));

  CHECK_EQ(xfs::PerfStats::Get().max_concurrent_writes.load(), 1u);
  CHECK(xfs::PerfStats::Get().inode_lock_waits.load() >= 1);
}

TEST(perf_concurrent_mixed_io_stress) {
  FixtureImage::Instance().Reset();
  auto fs = FixtureImage::Instance().Mount(/*read_write=*/true);
  CHECK(fs.ok());
  constexpr int kFiles = 4;
  constexpr int kBlocks = 16;
  constexpr int kIterations = 40;
  constexpr uint64_t kBlock = 4096;
  std::vector<uint64_t> inos;
  for (int i = 0; i < kFiles; ++i) {
    auto created = fs.value()->CreateRegularFile(
        fs.value()->RootInode(), "stress-" + std::to_string(i), 0644, 0, 0);
    CHECK(created.ok());
    inos.push_back(created.value());
    auto data = Pattern(kBlocks * kBlock, static_cast<uint8_t>(i));
    CHECK(fs.value()->WriteFileData(created.value(), 0, data.data(),
                                    data.size())
              .ok());
  }
  auto dir = fs.value()->Mkdir(fs.value()->RootInode(), "stress-dir", 0755, 0,
                               0);
  CHECK(dir.ok());
  auto dir_entry = fs.value()->LookupName(fs.value()->RootInode(), "stress-dir");
  CHECK(dir_entry.ok());
  const uint64_t dir_ino = dir_entry.value().ino;

  EnablePerf();
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kFiles; ++i) {
    threads.emplace_back([&, i]() {
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        const int block = iteration % kBlocks;
        const auto data = Pattern(kBlock,
                                  static_cast<uint8_t>(i * 31 + iteration));
        auto wrote = fs.value()->WriteFileData(inos[i], block * kBlock,
                                               data.data(), data.size());
        if (!wrote.ok()) {
          failures.fetch_add(1);
          return;
        }
        std::vector<uint8_t> readback(kBlock);
        auto read = fs.value()->ReadFile(inos[i], block * kBlock,
                                         readback.data(), readback.size());
        if (!read.ok() || readback != data) failures.fetch_add(1);
      }
    });
  }
  threads.emplace_back([&]() {
    for (int i = 0; i < 20; ++i) {
      const std::string name = "n" + std::to_string(i);
      auto created = fs.value()->CreateRegularFile(dir_ino, name, 0644, 0, 0);
      if (!created.ok()) {
        failures.fetch_add(1);
        return;
      }
      const auto data = Pattern(kBlock, static_cast<uint8_t>(i));
      if (!fs.value()->WriteFileData(created.value(), 0, data.data(),
                                     data.size())
               .ok()) {
        failures.fetch_add(1);
        return;
      }
      std::vector<uint8_t> readback(kBlock);
      auto read = fs.value()->ReadFile(created.value(), 0, readback.data(),
                                       readback.size());
      if (!read.ok() || readback != data) failures.fetch_add(1);
      if (!fs.value()->Unlink(dir_ino, name, /*defer_inode_free=*/false).ok()) {
        failures.fetch_add(1);
        return;
      }
    }
  });
  for (auto& thread : threads) thread.join();
  CHECK_EQ(failures.load(), 0);
  CHECK(fs.value()->Flush().ok());

  for (int i = 0; i < kFiles; ++i) {
    std::vector<uint8_t> data(kBlocks * kBlock);
    auto read = fs.value()->ReadFile(inos[i], 0, data.data(), data.size());
    CHECK(read.ok());
    CHECK_EQ(read.value(), data.size());
    for (int block = 0; block < kBlocks; ++block) {
      int last = -1;
      for (int iteration = block; iteration < kIterations; iteration += kBlocks) {
        last = iteration;
      }
      const auto expected = Pattern(kBlock,
                                    static_cast<uint8_t>(i * 31 + last));
      CHECK(std::memcmp(data.data() + block * kBlock, expected.data(),
                        kBlock) == 0);
    }
  }

  xfs::CheckReport report = fs.value()->CheckConsistency(xfs::CheckOptions{});
  CHECK_EQ(report.Overall(), xfs::CheckStatus::kOk);
}

TEST(perf_deferred_timestamps_are_visible_and_flushed) {
  FixtureImage::Instance().Reset();
  const uint64_t ino = FixtureImage::Instance().info().file_ino;
  {
    auto fs = FixtureImage::Instance().Mount(/*read_write=*/true);
    CHECK(fs.ok());
    xfs::XfsTimestamp old_time;
    old_time.sec = 1000;
    old_time.nsec = 0;
    CHECK(fs.value()
              ->SetFileTimes(ino, old_time, old_time, old_time)
              .ok());
    std::vector<uint8_t> data(4096, 0x77);
    EnablePerf();
    auto wrote = fs.value()->WriteFileData(ino, 0, data.data(), data.size(),
                                           /*append=*/false,
                                           /*defer_timestamps=*/true);
    CHECK(wrote.ok());
    // The timestamp is visible immediately through the inode overlay, but no
    // metadata transaction has been committed yet.
    auto info = fs.value()->GetFileInfo(ino);
    CHECK(info.ok());
    CHECK(info.value().mtime.sec > old_time.sec);
    CHECK_EQ(xfs::PerfStats::Get().journal_commits.load(), 0u);
    CHECK(fs.value()->Flush().ok());
    CHECK(xfs::PerfStats::Get().journal_commits.load() >= 1u);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
  }
  {
    auto fs = FixtureImage::Instance().Mount(/*read_write=*/false);
    CHECK(fs.ok());
    auto info = fs.value()->GetFileInfo(ino);
    CHECK(info.ok());
    CHECK(info.value().mtime.sec > 1000);
  }
}

TEST(perf_warm_directory_enumeration_hits_cache) {
  FixtureImage::Instance().Reset();
  auto fs = FixtureImage::Instance().Mount(/*read_write=*/false);
  CHECK(fs.ok());
  auto first = fs.value()->ListDirectory(fs.value()->RootInode());
  CHECK(first.ok());
  EnablePerf();
  auto second = fs.value()->ListDirectory(fs.value()->RootInode());
  CHECK(second.ok());
  CHECK_EQ(first.value().size(), second.value().size());
  CHECK(xfs::PerfStats::Get().directory_cache_hits.load() >= 1);
  CHECK_EQ(xfs::PerfStats::Get().directory_cache_misses.load(), 0u);
}
