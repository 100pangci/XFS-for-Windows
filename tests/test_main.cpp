// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>

#include "support/fixture_image.h"
#include "test_util.h"
#include "xfs/image_device.h"
#include "xfs/log.h"

namespace xfstest {

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

std::filesystem::path MakeTempImage(uint64_t size_bytes) {
  static std::mt19937_64 rng{std::random_device{}()};
  auto dir = std::filesystem::temp_directory_path() / "xfswin-tests";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  auto path = dir / ("img-" + std::to_string(rng()) + ".img");
  std::filesystem::remove(path, ec);
  // Create the file first; some standard library implementations refuse to
  // resize a file that does not exist.
  {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return {};
    std::fclose(f);
  }
  std::filesystem::resize_file(path, size_bytes, ec);
  if (ec) {
    std::fprintf(stderr, "resize_file(%s) failed: %s\n",
                 path.string().c_str(), ec.message().c_str());
    return {};
  }
  return path;
}

}  // namespace xfstest

namespace {

// Writes a fixture image (a consistent filesystem with one file) to `path`.
// CI runs this and then verifies the image with Linux xfs_repair/mount and the
// independent Python probe, so the fixture cannot "pass" by sharing a bug with
// the reader.
int MakeFixture(const std::string& path_string) {
  const auto* p = reinterpret_cast<const char8_t*>(path_string.data());
  std::filesystem::path path(std::u8string(p, path_string.size()));
  std::string error;
  auto device = xfs::ImageBlockDevice::Create(path, 300ull << 20, 512, &error);
  if (!device) {
    std::fprintf(stderr, "cannot create %s: %s\n", path_string.c_str(),
                 error.c_str());
    return 1;
  }
  xfstest::FixtureOptions options;
  options.size_bytes = 300ull << 20;
  options.uuid = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x47, 0x88,
                  0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00};
  xfstest::FixtureInfo info;
  xfs::Status status = xfstest::BuildFixtureImage(*device, options, &info);
  if (!status.ok()) {
    std::fprintf(stderr, "fixture build failed: %s\n",
                 status.error().Format().c_str());
    device->Close();
    return 1;
  }
  if (!device->Flush()) {
    std::fprintf(stderr, "fixture flush failed: %s\n",
                 device->LastError().c_str());
    device->Close();
    return 1;
  }
  device->Close();
  std::printf(
      "fixture: %s\n  block size : %llu\n  file       : /%s (inode %llu)\n"
      "  size       : %llu bytes\n  extent     : fsb %llu, %llu blocks\n"
      "  pattern   : byte(i) = ((i * 167) ^ (i >> 7) ^ 0x5A) & 0xFF\n",
      path_string.c_str(), static_cast<unsigned long long>(info.block_size),
      options.file_name.c_str(),
      static_cast<unsigned long long>(info.file_ino),
      static_cast<unsigned long long>(info.file_size),
      static_cast<unsigned long long>(info.data_fsb),
      static_cast<unsigned long long>(info.data_blocks));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--make-fixture") == 0) {
    return MakeFixture(argv[2]);
  }
  if (argc >= 2 &&
      (std::strcmp(argv[1], "--help") == 0 ||
       std::strcmp(argv[1], "-h") == 0)) {
    std::cout << "usage: xfs_tests [--make-fixture <image>]\n"
                 "  no arguments: run the test suite\n"
                 "  --make-fixture <image>: write a fixture image (one "
                 "consistent filesystem with one file) for Linux verification\n";
    return 0;
  }

  xfs::log::SetLevel(xfs::log::Level::kError);
  auto& tests = xfstest::Registry();
  std::cout << "running " << tests.size() << " tests\n";
  int failed = 0;
  for (const auto& test : tests) {
    try {
      test.fn();
      std::cout << "  [ OK ] " << test.name << "\n";
    } catch (const std::exception& e) {
      std::cout << "  [FAIL] " << test.name << ": " << e.what() << "\n";
      ++failed;
    }
  }
  std::cout << (failed == 0 ? "all tests passed\n" : "TESTS FAILED\n");
  return failed == 0 ? 0 : 1;
}
