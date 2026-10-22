// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>

#include "support/fixture_device.h"
#include "support/fixture_image.h"
#include "test_util.h"
#include "xfs/ag.h"
#include "xfs/alloc.h"
#include "xfs/btree.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/formatter.h"
#include "xfs/ialloc.h"
#include "xfs/image_device.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"

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

// Writes a small modern-inode fixture whose initial filesystem has a fully
// allocated formatter chunk plus a later sparse chunk (nonzero holemask). CI
// mounts and runs xfs_repair on this image, so the sparse record, hole
// ownership, ICREATE geometry and allocator free path are checked by the
// Linux oracles rather than only by the Windows unit test that builds it.
int MakeRw6SparseFixture(const std::string& path_string) {
  using namespace xfs;
  const uint64_t size_bytes = 512ull << 20;
  std::string error;
  const auto* p = reinterpret_cast<const char8_t*>(path_string.data());
  std::filesystem::path path(std::u8string(p, path_string.size()));
  auto image = ImageBlockDevice::Create(path, size_bytes, 512, &error);
  if (!image) {
    std::fprintf(stderr, "cannot create %s: %s\n", path_string.c_str(),
                 error.c_str());
    return 1;
  }

  mkfs::FormatOptions options;
  options.block_size = 1024;
  options.inode_size = 512;
  options.quiet = true;
  options.finobt = true;
  options.inobtcount = true;
  options.sparse_inodes = true;
  options.bigtime = true;
  options.nrext64 = true;
  auto formatted = mkfs::Format(*image, options);
  if (!formatted.ok()) {
    std::fprintf(stderr, "sparse formatter failed: %s\n",
                 formatted.error().Format().c_str());
    image->Close();
    return 1;
  }

  MountOptions mount_options;
  mount_options.read_write = true;
  auto borrowed = std::make_unique<xfstest::BorrowedBlockDevice>(image.get());
  auto mounted = XfsFilesystem::Mount(std::move(borrowed), mount_options);
  if (!mounted.ok()) {
    std::fprintf(stderr, "sparse mount failed: %s\n",
                 mounted.error().Format().c_str());
    image->Close();
    return 1;
  }
  XfsFilesystem& fs = *mounted.value();
  const XfsGeometry& geo = fs.geometry();
  const XfsSuperblock& sb = fs.superblock();

  auto fail_error = [&](const char* what, const ErrorContext& error) {
    std::fprintf(stderr, "%s: %s\n", what, error.Format().c_str());
    return 1;
  };
  auto fail = [&](const char* what, const auto& value) {
    if constexpr (requires { value.error(); }) {
      return fail_error(what, value.error());
    } else {
      return fail_error(what, value);
    }
  };

  // Fill the formatter chunk so the next chunk must be a sparse allocation.
  {
    auto txn = fs.BeginTransaction();
    if (!txn.ok()) return fail("sparse transaction", txn.error());
    BlockAllocator alloc(&txn.value(), image.get(), &geo);
    InodeAllocator inodes(&txn.value(), &alloc, image.get(), &sb, &geo);
    for (uint32_t i = 0; i < 61; ++i) {
      uint64_t ino = 0;
      Status s = inodes.AllocateInode(0, 0, &ino);
      if (!s.ok()) return fail("sparse fill allocation", s);
    }
    Status s = alloc.Commit();
    if (!s.ok()) return fail("sparse fill block commit", s);
    s = inodes.Commit();
    if (!s.ok()) return fail("sparse fill inode commit", s);
    s = fs.CommitTransaction(txn.value());
    if (!s.ok()) return fail("sparse fill transaction commit", s);
  }

  // Read the AG0 free extents. The helper walks the bnobt from disk, so this
  // also exercises the free-space metadata the allocator just wrote.
  struct Extent {
    uint32_t start;
    uint32_t len;
  };
  std::vector<Extent> extents;
  {
    auto agf = ReadAgf(*image, geo, 0);
    if (!agf.ok()) return fail("sparse read AGF", agf.error());
    BtreeSpec spec;
    spec.name = "bnobt";
    spec.format = BtreeFormat::kShort;
    spec.block_size = geo.block_size;
    spec.key_size = ondisk::btree::kAllocKeySize;
    spec.ptr_size = ondisk::btree::kAllocPtrSize;
    spec.rec_size = ondisk::btree::kAllocRecSize;
    spec.magic = ondisk::kBtbCrcMagic;
    spec.null_ptr = ondisk::btree::kNullAgBlock;
    spec.expected_owner = 0;
    spec.owner_size = 4;
    spec.uuid = geo.meta_uuid;
    spec.ptr_to_daddr = [&](uint64_t agbno, uint64_t* daddr) {
      if (agbno >= geo.AgBlocksFor(0)) return false;
      *daddr = geo.FsbToByte(geo.AgFsb(0, static_cast<uint32_t>(agbno))) / 512;
      return true;
    };
    BtreeWalkCallbacks cb;
    cb.read_block = [&](uint64_t agbno, std::vector<uint8_t>* buf) {
      if (agbno >= geo.AgBlocksFor(0)) return false;
      buf->resize(geo.block_size);
      return image->Read(geo.FsbToByte(geo.AgFsb(
                             0, static_cast<uint32_t>(agbno))),
                         buf->data(), buf->size());
    };
    cb.ptr_in_range = [&](uint64_t agbno) {
      return agbno < geo.AgBlocksFor(0);
    };
    cb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                   const uint8_t* buf) -> Status {
      if (meta.level != 0) return OkStatus();
      for (uint16_t i = 0; i < meta.numrecs; ++i) {
        const uint8_t* rec = BtreeLeafRec(buf, meta, spec, i);
        extents.push_back({GetBE32(rec), GetBE32(rec + 4)});
      }
      return OkStatus();
    };
    Status s = WalkBtree(spec, agf.value().bno_root, cb);
    if (!s.ok()) return fail("sparse bnobt walk", s);
  }

  // Pick a cluster-sized hole inside one of the free extents, then consume all
  // AG0 free space. A full chunk can no longer fit; the sparse allocator can
  // still use the cluster-sized hole.
  const uint32_t spino = 16;  // 1 KiB blocks, 512-byte inodes: 16-block cluster
  uint32_t target_start = 0;
  bool found = false;
  for (const Extent& extent : extents) {
    const uint32_t aligned = static_cast<uint32_t>(
        (static_cast<uint64_t>(extent.start) + 15) & ~uint64_t{15});
    if (aligned >= 32 && aligned + spino <= extent.start + extent.len) {
      target_start = aligned;
      found = true;
      break;
    }
  }
  if (!found) {
    std::fprintf(stderr, "sparse fixture: no cluster hole in AG0\n");
    return 1;
  }
  for (const Extent& extent : extents) {
    auto txn = fs.BeginTransaction();
    if (!txn.ok()) return fail("sparse consume transaction", txn.error());
    BlockAllocator alloc(&txn.value(), image.get(), &geo);
    Allocation allocation;
    Status s = alloc.AllocateFromAg(0, extent.len, 1, &allocation);
    if (!s.ok()) return fail("sparse consume allocation", s);
    if (allocation.len != extent.len) {
      return fail_error("sparse consume allocation",
                        XFS_ERR(XfsError::kInternal,
                                "allocator returned the wrong extent length"));
    }
    s = alloc.Commit();
    if (!s.ok()) return fail("sparse consume block commit", s);
    s = fs.CommitTransaction(txn.value());
    if (!s.ok()) return fail("sparse consume transaction commit", s);
  }
  {
    auto txn = fs.BeginTransaction();
    if (!txn.ok()) return fail("sparse hole transaction", txn.error());
    BlockAllocator alloc(&txn.value(), image.get(), &geo);
    Status s = alloc.Free(geo.AgFsb(0, target_start), spino);
    if (!s.ok()) return fail("sparse hole free", s);
    s = alloc.Commit();
    if (!s.ok()) return fail("sparse hole block commit", s);
    s = fs.CommitTransaction(txn.value());
    if (!s.ok()) return fail("sparse hole transaction commit", s);
  }
  {
    auto txn = fs.BeginTransaction();
    if (!txn.ok()) return fail("sparse chunk transaction", txn.error());
    BlockAllocator alloc(&txn.value(), image.get(), &geo);
    InodeAllocator inodes(&txn.value(), &alloc, image.get(), &sb, &geo);
    Status s = inodes.PreallocateChunk(0);
    if (!s.ok()) return fail("sparse chunk allocation", s);
    s = alloc.Commit();
    if (!s.ok()) return fail("sparse chunk block commit", s);
    s = inodes.Commit();
    if (!s.ok()) return fail("sparse chunk inode commit", s);
    s = fs.CommitTransaction(txn.value());
    if (!s.ok()) return fail("sparse chunk transaction commit", s);
  }

  // Allocate one inode from the sparse chunk so the fixture has both a hole
  // and a real backed inode outside the formatter chunk.
  auto created = fs.CreateRegularFile(fs.RootInode(), "sparse.bin", 0644, 0, 0);
  if (!created.ok()) return fail("sparse file create", created.error());
  Status clean = fs.WriteCleanUnmountRecord();
  if (!clean.ok()) return fail("sparse clean unmount", clean);
  CheckReport report = fs.CheckConsistency(CheckOptions{});
  if (report.Overall() != CheckStatus::kOk) {
    std::fprintf(stderr, "sparse fixture check failed: %s\n",
                 report.Summary().c_str());
    for (const CheckItem& item : report.items) {
      if (item.status != CheckStatus::kOk) {
        std::fprintf(stderr, "  %s %s: %s\n", ToString(item.status),
                     item.name.c_str(), item.message.c_str());
      }
    }
    return 1;
  }

  mounted.value().reset();
  if (!image->Flush()) {
    std::fprintf(stderr, "sparse flush failed: %s\n",
                 image->LastError().c_str());
    image->Close();
    return 1;
  }
  image->Close();
  std::printf("rw6 sparse fixture: %s\n  block size: 1024\n"
              "  sparse hole at AG0 block %u (%u blocks)\n",
              path_string.c_str(), target_start, spino);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--make-fixture") == 0) {
    return MakeFixture(argv[2]);
  }
  if (argc == 3 && std::strcmp(argv[1], "--make-rw6-sparse") == 0) {
    return MakeRw6SparseFixture(argv[2]);
  }
  std::string filter;
  if (argc == 2 && std::strncmp(argv[1], "--filter=", 9) == 0) {
    filter = argv[1] + 9;
  }
  if (argc >= 2 &&
      (std::strcmp(argv[1], "--help") == 0 ||
       std::strcmp(argv[1], "-h") == 0)) {
    std::cout << "usage: xfs_tests [--make-fixture <image>] "
                 "[--make-rw6-sparse <image>] [--filter=<substring>]\n"
                 "  no arguments: run the test suite\n"
                 "  --make-fixture <image>: write a fixture image (one "
                 "consistent filesystem with one file) for Linux verification\n"
                 "  --make-rw6-sparse <image>: write a modern-inode filesystem\n"
                 "      with a real sparse inode chunk for Linux verification\n"
                 "  --filter=<substring>: only run tests whose name contains "
                 "the substring\n";
    return 0;
  }

  xfs::log::SetLevel(xfs::log::Level::kError);
  auto& tests = xfstest::Registry();
  if (filter.empty()) {
    std::cout << "running " << tests.size() << " tests\n";
  } else {
    std::cout << "running tests matching '" << filter << "'\n";
  }
  int failed = 0;
  int ran = 0;
  for (const auto& test : tests) {
    if (!filter.empty() &&
        std::string(test.name).find(filter) == std::string::npos) {
      continue;
    }
    ++ran;
    try {
      test.fn();
      std::cout << "  [ OK ] " << test.name << "\n";
    } catch (const std::exception& e) {
      std::cout << "  [FAIL] " << test.name << ": " << e.what() << "\n";
      ++failed;
    }
  }
  std::cout << (failed == 0 ? "all tests passed" : "TESTS FAILED") << " ("
            << ran << " tests run)\n";
  return failed == 0 ? 0 : 1;
}
