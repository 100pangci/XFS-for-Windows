#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>

#include "test_util.h"
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

int main() {
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
