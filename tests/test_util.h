// XFS for Windows - minimal test harness and helpers.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
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
    if (offset > data_.size() || size > data_.size() - offset) return false;
    std::memcpy(buffer, data_.data() + offset, size);
    return true;
  }
  bool Write(uint64_t offset, const void* buffer, size_t size) override {
    if (offset > data_.size() || size > data_.size() - offset) return false;
    std::memcpy(data_.data() + offset, buffer, size);
    return true;
  }
  bool Flush() override { return true; }
  uint64_t Size() const override { return data_.size(); }
  uint32_t SectorSize() const override { return sector_; }
  const xfs::DeviceInfo& Info() const override { return info_; }
  void Close() override {}

  std::vector<uint8_t>& bytes() { return data_; }

 private:
  std::vector<uint8_t> data_;
  uint32_t sector_;
  xfs::DeviceInfo info_;
};

// Creates a unique temporary image file of the given size (sparse).
std::filesystem::path MakeTempImage(uint64_t size_bytes);

}  // namespace xfstest
