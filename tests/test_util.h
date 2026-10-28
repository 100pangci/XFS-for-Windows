// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - minimal test harness and helpers.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "xfs/block_device.h"

namespace xfstest {

struct TestFailure : std::exception {
  std::string message;
  explicit TestFailure(std::string m) : message(std::move(m)) {}
  const char* what() const noexcept override { return message.c_str(); }
};

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& Registry();

struct Registration {
  Registration(const char* name, void (*fn)()) {
    Registry().push_back({name, fn});
  }
};

#define TEST(name)                                              \
  static void test_##name();                                    \
  static ::xfstest::Registration reg_##name(#name, test_##name); \
  static void test_##name()

inline void Fail(const char* file, int line, const std::string& what) {
  char buffer[2048];
  std::snprintf(buffer, sizeof(buffer), "%s:%d: %s", file, line,
                what.c_str());
  throw TestFailure(buffer);
}

#define CHECK(cond)                                          \
  do {                                                       \
    if (!(cond)) {                                           \
      ::xfstest::Fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
    }                                                        \
  } while (0)

#define CHECK_MESSAGE(cond, message)                                      \
  do {                                                                  \
    if (!(cond)) {                                                      \
      ::xfstest::Fail(__FILE__, __LINE__, std::string(message));        \
    }                                                                   \
  } while (0)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    auto va = (a);                                                        \
    auto vb = (b);                                                        \
    if (!(va == vb)) {                                                    \
      ::xfstest::Fail(__FILE__, __LINE__,                                 \
                      "CHECK_EQ failed: " #a " != " #b);                  \
    }                                                                     \
  } while (0)

#define CHECK_STR_EQ(a, b)                                               \
  do {                                                                   \
    std::string va = (a);                                                \
    std::string vb = (b);                                                \
    if (va != vb) {                                                      \
      ::xfstest::Fail(__FILE__, __LINE__,                                \
                      "CHECK_STR_EQ failed: '" + va + "' != '" + vb + "'"); \
    }                                                                    \
  } while (0)

// A simple in-memory block device for parser unit tests.
class MemoryBlockDevice final : public xfs::BlockDevice {
 public:
  explicit MemoryBlockDevice(uint64_t size, uint32_t sector = 512)
      : data_(size, 0), sector_(sector) {
    info_.size_bytes = size;
    info_.logical_sector_size = sector;
    info_.physical_sector_size = sector;
    info_.writable = true;
    info_.description = "memory";
  }

  bool Read(uint64_t offset, void* buffer, size_t size) override {
    const uint64_t active = active_reads_.fetch_add(1) + 1;
    UpdatePeak(max_reads_, active);
    if (read_delay_us_.load() > 0) {
      std::this_thread::sleep_for(
          std::chrono::microseconds(read_delay_us_.load()));
    }
    read_calls_.fetch_add(1);
    read_bytes_.fetch_add(size);
    if (size >= 65536) large_read_calls_.fetch_add(1);
    bool ok = true;
    if (offset > data_.size() || size > data_.size() - offset) {
      ok = false;
    } else {
      std::memcpy(buffer, data_.data() + offset, size);
    }
    active_reads_.fetch_sub(1);
    return ok;
  }
  bool Write(uint64_t offset, const void* buffer, size_t size) override {
    const uint64_t active = active_writes_.fetch_add(1) + 1;
    UpdatePeak(max_writes_, active);
    if (write_delay_us_.load() > 0) {
      std::this_thread::sleep_for(
          std::chrono::microseconds(write_delay_us_.load()));
    }
    write_calls_.fetch_add(1);
    write_bytes_.fetch_add(size);
    if (size >= 65536) large_write_calls_.fetch_add(1);
    bool ok = true;
    if (offset > data_.size() || size > data_.size() - offset) {
      ok = false;
    } else {
      std::memcpy(data_.data() + offset, buffer, size);
    }
    active_writes_.fetch_sub(1);
    return ok;
  }
  bool Flush() override {
    flush_calls_.fetch_add(1);
    return true;
  }
  uint64_t Size() const override { return data_.size(); }
  uint32_t SectorSize() const override { return sector_; }
  const xfs::DeviceInfo& Info() const override { return info_; }
  void Close() override {}

  std::vector<uint8_t>& bytes() { return data_; }

  void ResetCounters() {
    read_calls_ = 0;
    read_bytes_ = 0;
    write_calls_ = 0;
    write_bytes_ = 0;
    large_read_calls_ = 0;
    large_write_calls_ = 0;
    flush_calls_ = 0;
    max_reads_ = 0;
    max_writes_ = 0;
  }
  void SetWriteDelay(std::chrono::microseconds delay) {
    write_delay_us_ = delay.count();
  }
  void SetReadDelay(std::chrono::microseconds delay) {
    read_delay_us_ = delay.count();
  }
  uint64_t read_calls() const { return read_calls_.load(); }
  uint64_t read_bytes() const { return read_bytes_.load(); }
  uint64_t write_calls() const { return write_calls_.load(); }
  uint64_t write_bytes() const { return write_bytes_.load(); }
  uint64_t large_read_calls() const { return large_read_calls_.load(); }
  uint64_t large_write_calls() const { return large_write_calls_.load(); }
  uint64_t flush_calls() const { return flush_calls_.load(); }
  uint64_t max_concurrent_reads() const { return max_reads_.load(); }
  uint64_t max_concurrent_writes() const { return max_writes_.load(); }

 private:
  static void UpdatePeak(std::atomic<uint64_t>& peak, uint64_t value) {
    uint64_t current = peak.load();
    while (value > current &&
           !peak.compare_exchange_weak(current, value)) {
    }
  }

  std::vector<uint8_t> data_;
  uint32_t sector_;
  xfs::DeviceInfo info_;
  std::atomic<uint64_t> read_calls_{0};
  std::atomic<uint64_t> read_bytes_{0};
  std::atomic<uint64_t> write_calls_{0};
  std::atomic<uint64_t> write_bytes_{0};
  std::atomic<uint64_t> large_read_calls_{0};
  std::atomic<uint64_t> large_write_calls_{0};
  std::atomic<uint64_t> flush_calls_{0};
  std::atomic<uint64_t> active_reads_{0};
  std::atomic<uint64_t> active_writes_{0};
  std::atomic<uint64_t> max_reads_{0};
  std::atomic<uint64_t> max_writes_{0};
  std::atomic<int64_t> read_delay_us_{0};
  std::atomic<int64_t> write_delay_us_{0};
};

// Creates a unique temporary image file of the given size (sparse).
std::filesystem::path MakeTempImage(uint64_t size_bytes);

}  // namespace xfstest
