// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

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
#include "xfs/xattr.h"

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

namespace di = xfs::ondisk::inode;

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

  // Fill the formatter chunk with real, initialized file inodes (not just
  // allocated bits), so an allocated inode cannot have mode zero when
  // xfs_repair scans the fixture.
  for (uint32_t i = 0; i < 61; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "fill%02u.bin", i);
    auto created = fs.CreateRegularFile(fs.RootInode(), name, 0644, 0, 0);
    if (!created.ok()) return fail("sparse fill create", created.error());
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

// Test-support RW-6C image with the same logical content as the Linux fixture
// in tests/scripts/make_rw6c_linux.sh.  It is not Linux evidence; it lets the
// Windows-side mutation, crash and ENOSPC tools be exercised locally without a
// Linux host.
int MakeRw6cFixture(const std::string& path_string) {
  using namespace xfs;
  constexpr uint32_t kBlock = 1024;
  auto pattern = [](size_t size, uint32_t seed) {
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i < size; ++i) {
      out[i] = static_cast<uint8_t>((i * 37 + seed * 17 + 11) & 0xFF);
    }
    return out;
  };
  const uint64_t size_bytes = 384ull << 20;
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
  options.block_size = kBlock;
  options.inode_size = 512;
  options.quiet = true;
  options.finobt = true;
  options.inobtcount = true;
  options.sparse_inodes = true;
  options.bigtime = true;
  options.nrext64 = true;
  options.rmapbt = true;
  options.reflink = true;
  auto formatted = mkfs::Format(*image, options);
  if (!formatted.ok()) {
    std::fprintf(stderr, "rw6c format failed: %s\n",
                 formatted.error().Format().c_str());
    image->Close();
    return 1;
  }

  MountOptions mount_options;
  mount_options.read_write = true;
  auto borrowed = std::make_unique<xfstest::BorrowedBlockDevice>(image.get());
  auto mounted = XfsFilesystem::Mount(std::move(borrowed), mount_options);
  if (!mounted.ok()) {
    std::fprintf(stderr, "rw6c mount failed: %s\n",
                 mounted.error().Format().c_str());
    image->Close();
    return 1;
  }
  XfsFilesystem& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  auto write = [&](uint64_t ino, uint64_t offset,
                   const std::vector<uint8_t>& data) -> bool {
    auto status = fs.WriteFileData(ino, offset, data.data(), data.size());
    if (!status.ok()) {
      std::fprintf(stderr, "rw6c write failed: %s\n",
                   status.error().Format().c_str());
      return false;
    }
    return true;
  };
  auto create = [&](const std::string& name, uint64_t* ino) -> bool {
    auto file = fs.CreateRegularFile(root, name, 0644, 0, 0);
    if (!file.ok()) {
      std::fprintf(stderr, "rw6c create %s failed: %s\n", name.c_str(),
                   file.error().Format().c_str());
      return false;
    }
    *ino = file.value();
    return true;
  };
  if (!fs.Mkdir(root, "base", 0755, 0, 0).ok() ||
      !fs.Mkdir(root, "emptydir", 0755, 0, 0).ok()) {
    std::fprintf(stderr, "rw6c mkdir failed\n");
    return 1;
  }
  auto base = fs.LookupPath("/base");
  if (!base.ok()) return 1;
  const uint64_t base_ino = base.value().ino;
  auto create_in = [&](const std::string& name, uint64_t* ino) -> bool {
    auto file = fs.CreateRegularFile(base_ino, name, 0644, 0, 0);
    if (!file.ok()) {
      std::fprintf(stderr, "rw6c create /base/%s failed: %s\n", name.c_str(),
                   file.error().Format().c_str());
      return false;
    }
    *ino = file.value();
    return true;
  };

  uint64_t source = 0, clone_a = 0, clone_b = 0, cow = 0, three = 0;
  if (!create_in("source.bin", &source) ||
      !write(source, 0, pattern(16 * kBlock, 11))) return 1;
  auto clone = [&](uint64_t src, uint64_t dst) -> bool {
    auto cloned = fs.ReflinkRange(src, 0, dst, 0, 0);
    if (!cloned.ok()) {
      std::fprintf(stderr, "rw6c clone failed: %s\n",
                   cloned.error().Format().c_str());
      return false;
    }
    return true;
  };
  if (!create_in("clone-a.bin", &clone_a) || !clone(source, clone_a) ||
      !create_in("clone-b.bin", &clone_b) || !clone(source, clone_b) ||
      !create_in("cow.bin", &cow) || !clone(source, cow) ||
      !create_in("three.bin", &three) || !clone(source, three)) return 1;
  if (!write(cow, 2 * kBlock, pattern(kBlock, 73))) return 1;

  uint64_t partial = 0, partial_clone = 0, sparse = 0, sparse_clone = 0;
  if (!create_in("partial.bin", &partial) ||
      !write(partial, 0, pattern(4 * kBlock + 300, 29)) ||
      !create_in("partial-clone.bin", &partial_clone) ||
      !clone(partial, partial_clone)) return 1;
  if (!create_in("sparse.bin", &sparse) ||
      !write(sparse, 2 * kBlock, pattern(2 * kBlock, 41)) ||
      !create_in("sparse-clone.bin", &sparse_clone) ||
      !clone(sparse, sparse_clone)) return 1;

  for (uint32_t i = 0; i < 100; ++i) {
    char src_name[16], gap_name[16], dst_name[16];
    std::snprintf(src_name, sizeof(src_name), "s%03u.bin", i);
    std::snprintf(gap_name, sizeof(gap_name), "g%03u.bin", i);
    std::snprintf(dst_name, sizeof(dst_name), "d%03u.bin", i);
    uint64_t src = 0, gap = 0, dst = 0;
    if (!create_in(src_name, &src) ||
        !write(src, 0, pattern(kBlock, 200 + i)) ||
        !create_in(gap_name, &gap) ||
        !write(gap, 0, pattern(kBlock, 500 + i)) ||
        !create_in(dst_name, &dst) || !clone(src, dst)) return 1;
  }

  Status clean = fs.WriteCleanUnmountRecord();
  if (!clean.ok()) {
    std::fprintf(stderr, "rw6c clean unmount failed: %s\n",
                 clean.error().Format().c_str());
    return 1;
  }
  mounted.value().reset();
  if (!image->Flush()) {
    std::fprintf(stderr, "rw6c flush failed: %s\n",
                 image->LastError().c_str());
    image->Close();
    return 1;
  }
  image->Close();
  std::printf("rw6c fixture: %s (reflink=1,rmapbt=1,1 KiB blocks)\n",
              path_string.c_str());
  return 0;
}

// Test-support RW-6D image: a Windows-formatted 1 KiB-block filesystem with
// every attribute-fork representation. It is not Linux evidence; CI mounts it
// on Linux and runs xfs_repair, and the Windows-side mutation/crash/ENOSPC
// tools use it as a base. The manifest records VFS names, values (hex) and
// formats so both the independent Python probe and Linux can compare.
int MakeRw6dFixture(const std::string& path_string) {
  using namespace xfs;
  constexpr uint32_t kBlock = 1024;
  auto pattern = [](size_t size, uint32_t seed) {
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i < size; ++i) {
      out[i] = static_cast<uint8_t>((i * 37 + seed * 17 + 11) & 0xFF);
    }
    return out;
  };
  const uint64_t size_bytes = 384ull << 20;
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
  options.block_size = kBlock;
  options.inode_size = 512;
  options.quiet = true;
  options.finobt = true;
  options.inobtcount = true;
  options.sparse_inodes = true;
  options.bigtime = true;
  options.nrext64 = true;
  options.rmapbt = true;
  options.reflink = true;
  auto formatted = mkfs::Format(*image, options);
  if (!formatted.ok()) {
    std::fprintf(stderr, "rw6d format failed: %s\n",
                 formatted.error().Format().c_str());
    image->Close();
    return 1;
  }

  MountOptions mount_options;
  mount_options.read_write = true;
  auto borrowed = std::make_unique<xfstest::BorrowedBlockDevice>(image.get());
  auto mounted = XfsFilesystem::Mount(std::move(borrowed), mount_options);
  if (!mounted.ok()) {
    std::fprintf(stderr, "rw6d mount failed: %s\n",
                 mounted.error().Format().c_str());
    image->Close();
    return 1;
  }
  XfsFilesystem& fs = *mounted.value();
  const uint64_t root = fs.RootInode();
  auto check = [](const char* what, const auto& result) {
    if (result.ok()) return true;
    std::fprintf(stderr, "%s: %s\n", what, result.error().Format().c_str());
    return false;
  };
  auto write = [&](uint64_t ino, uint64_t offset,
                   const std::vector<uint8_t>& data) {
    auto status = fs.WriteFileData(ino, offset, data.data(), data.size());
    if (!status.ok()) {
      std::fprintf(stderr, "rw6d write failed: %s\n",
                   status.error().Format().c_str());
      return false;
    }
    return true;
  };
  auto create = [&](const std::string& name, uint64_t* ino) {
    auto file = fs.CreateRegularFile(root, name, 0644, 0, 0);
    if (!file.ok()) {
      std::fprintf(stderr, "rw6d create %s failed: %s\n", name.c_str(),
                   file.error().Format().c_str());
      return false;
    }
    *ino = file.value();
    return true;
  };
  auto set = [&](uint64_t ino, std::string_view name,
                 const std::vector<uint8_t>& value) {
    auto stored = fs.SetXattr(ino, name, value.data(), value.size(),
                              XfsFilesystem::XattrSetMode::kUpsert);
    if (!stored.ok()) {
      std::fprintf(stderr, "rw6d set %.*s failed: %s\n",
                   static_cast<int>(name.size()), name.data(),
                   stored.error().Format().c_str());
      return false;
    }
    return true;
  };
  struct FixtureFile {
    std::string path;
    uint64_t ino;
  };
  std::vector<FixtureFile> files;
  auto track = [&](const char* name, uint64_t ino) {
    files.push_back({name, ino});
  };

  uint64_t short_ino = 0;
  if (!create("short", &short_ino) || !write(short_ino, 0, pattern(4096, 1)) ||
      !set(short_ino, "user.alpha", {'h', 'e', 'l', 'l', 'o'}) ||
      !set(short_ino, "user.beta", pattern(23, 0x0101)) ||
      !set(short_ino, "trusted.owner", {'t', 'r', 'u', 's', 't'}) ||
      !set(short_ino, "security.selinux", {'s', 0, 'e', 'l'}) ||
      !set(short_ino, "system.posix_acl_access", {'a', 'c', 'l'})) {
    return 1;
  }
  track("/short", short_ino);

  uint64_t leaf_ino = 0;
  if (!create("leaf", &leaf_ino) || !write(leaf_ino, 0, pattern(4096, 2))) {
    return 1;
  }
  for (uint32_t i = 0; i < 5; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "user.l%02u", i);
    if (!set(leaf_ino, name, pattern(100, 0x2000 + i))) return 1;
  }
  track("/leaf", leaf_ino);

  uint64_t node_ino = 0;
  if (!create("node", &node_ino) || !write(node_ino, 0, pattern(4096, 3))) {
    return 1;
  }
  for (uint32_t i = 0; i < 140; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "user.n%03u", i);
    if (!set(node_ino, name, pattern(600, 0x9000 + i))) return 1;
  }
  track("/node", node_ino);

  uint64_t remote_ino = 0;
  if (!create("remote", &remote_ino) ||
      !write(remote_ino, 0, pattern(4096, 4)) ||
      !set(remote_ino, "user.local", pattern(31, 0x7100)) ||
      !set(remote_ino, "user.remote", pattern(3000, 0x6d02)) ||
      !set(remote_ino, "user.remote2", pattern(9000, 0x6d03))) {
    return 1;
  }
  track("/remote", remote_ino);

  uint64_t data_ino = 0;
  if (!create("data", &data_ino) || !write(data_ino, 0, pattern(4096, 5))) {
    return 1;
  }
  for (uint32_t i = 1; i <= 15; ++i) {
    auto allocated = fs.AllocateUnwritten(
        data_ino, static_cast<uint64_t>(i) * 4096, 512);
    if (!check("rw6d data extent", allocated)) return 1;
  }
  if (!set(data_ino, "user.keep", pattern(200, 0xd00d))) return 1;
  track("/data", data_ino);

  uint64_t filler_ino = 0;
  if (!create("filler", &filler_ino)) return 1;
  uint64_t bmbt_ino = 0;
  if (!create("bmbt", &bmbt_ino) || !write(bmbt_ino, 0, pattern(4096, 6))) {
    return 1;
  }
  for (uint32_t i = 0; i < 80; ++i) {
    if (i >= 8) {
      auto allocated = fs.AllocateUnwritten(
          filler_ino, static_cast<uint64_t>(i) * 512, 512);
      if (!check("rw6d filler extent", allocated)) return 1;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "user.b%03u", i);
    if (!set(bmbt_ino, name, pattern(180, 0xe000 + i))) return 1;
  }
  track("/bmbt", bmbt_ino);

  auto mkdir = fs.Mkdir(root, "dir", 0755, 0, 0);
  if (!check("rw6d mkdir", mkdir)) return 1;
  auto dir = fs.LookupPath("/dir");
  if (!dir.ok()) {
    std::fprintf(stderr, "rw6d lookup /dir failed: %s\n",
                 dir.error().Format().c_str());
    return 1;
  }
  if (!set(dir.value().ino, "user.dir", pattern(80, 0x3300)) ||
      !set(dir.value().ino, "trusted.dir", pattern(40, 0x3301))) {
    return 1;
  }
  track("/dir", dir.value().ino);

  auto link = fs.CreateSymlink(root, "link", "target", 0777, 0, 0);
  if (!check("rw6d symlink", link)) return 1;
  if (!set(link.value(), "user.link", pattern(64, 0x4400)) ||
      !set(link.value(), "trusted.link", pattern(32, 0x4401))) {
    return 1;
  }
  track("/link", link.value());

  uint64_t del_ino = 0;
  if (!create("del", &del_ino) || !write(del_ino, 0, pattern(2048, 7))) {
    return 1;
  }
  for (uint32_t i = 0; i < 40; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "user.d%02u", i);
    if (!set(del_ino, name, pattern(300, 0xf000 + i))) return 1;
  }
  track("/del", del_ino);

  uint64_t rename_ino = 0;
  if (!create("rename", &rename_ino) ||
      !write(rename_ino, 0, pattern(1024, 8)) ||
      !set(rename_ino, "user.r0", pattern(50, 0x8800)) ||
      !set(rename_ino, "user.r1", pattern(50, 0x8801))) {
    return 1;
  }
  track("/rename", rename_ino);

  uint64_t linkme_ino = 0;
  if (!create("linkme", &linkme_ino) ||
      !write(linkme_ino, 0, pattern(1024, 9)) ||
      !set(linkme_ino, "user.h0", pattern(50, 0x9900))) {
    return 1;
  }
  track("/linkme", linkme_ino);

  // Gather the manifest before the clean-unmount record; the content on disk
  // is already final and the probe reads metadata, not the log.
  auto format_name = [](XattrForkFormat format) {
    switch (format) {
      case XattrForkFormat::kNone: return "none";
      case XattrForkFormat::kShortform: return "shortform";
      case XattrForkFormat::kLeaf: return "leaf";
      case XattrForkFormat::kNode: return "node";
      case XattrForkFormat::kExtentsEmpty: return "extents-empty";
    }
    return "unknown";
  };
  auto json_escape = [](const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out.push_back(static_cast<char>(c));
          }
      }
    }
    return out;
  };
  auto hex = [](const std::vector<uint8_t>& value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (uint8_t b : value) {
      out.push_back(kDigits[b >> 4]);
      out.push_back(kDigits[b & 0xF]);
    }
    return out;
  };

  const std::string manifest_path = path_string + ".manifest.json";
  std::ofstream manifest(manifest_path, std::ios::binary);
  if (!manifest) {
    std::fprintf(stderr, "cannot create %s\n", manifest_path.c_str());
    return 1;
  }
  manifest << "{\n  \"kind\": \"rw6d-windows-fixture\",\n"
              "  \"block_size\": 1024,\n  \"inodes\": [\n";
  bool first_file = true;
  for (const FixtureFile& f : files) {
    auto inode = fs.OpenInode(f.ino);
    if (!check("rw6d open inode", inode)) return 1;
    auto fork = ReadXattrFork(*image, fs.geometry(), inode.value());
    if (!check("rw6d read attr fork", fork)) return 1;
    auto entries = fs.ListXattrs(f.ino);
    if (!check("rw6d list attrs", entries)) return 1;
    if (!first_file) manifest << ",\n";
    first_file = false;
    manifest << "    {\"ino\": " << f.ino << ", \"path\": \""
             << json_escape(f.path) << "\", \"format\": \""
             << format_name(fork.value().format) << "\", \"root_level\": "
             << fork.value().root_level << ", \"aformat\": "
             << static_cast<unsigned>(fork.value().aformat)
             << ", \"attributes\": [";
    bool first_attr = true;
    for (const XattrEntry& e : entries.value()) {
      if (!first_attr) manifest << ", ";
      first_attr = false;
      manifest << "{\"name\": \"" << json_escape(e.DisplayName())
               << "\", \"length\": " << e.value.size()
               << ", \"value_hex\": \"" << hex(e.value) << "\"}";
    }
    manifest << "]}";
  }
  manifest << "\n  ],\n  \"deleted\": []\n}\n";
  manifest.close();

  // Verify the representations this fixture exists to exercise.
  auto require_format = [&](uint64_t ino, XattrForkFormat want,
                            const char* what) {
    auto inode = fs.OpenInode(ino);
    if (!check("rw6d format inode", inode)) return false;
    auto fork = ReadXattrFork(*image, fs.geometry(), inode.value());
    if (!check("rw6d format fork", fork)) return false;
    if (fork.value().format != want) {
      std::fprintf(stderr, "rw6d %s: format %s, expected %s\n", what,
                   format_name(fork.value().format), format_name(want));
      return false;
    }
    return true;
  };
  if (!require_format(short_ino, XattrForkFormat::kShortform, "/short") ||
      !require_format(leaf_ino, XattrForkFormat::kLeaf, "/leaf") ||
      !require_format(node_ino, XattrForkFormat::kNode, "/node")) {
    return 1;
  }
  {
    auto inode = fs.OpenInode(node_ino);
    if (!inode.ok()) return 1;
    auto fork = ReadXattrFork(*image, fs.geometry(), inode.value());
    if (!fork.ok() || fork.value().root_level < 2) {
      std::fprintf(stderr, "rw6d /node did not reach a multi-level DA tree\n");
      return 1;
    }
    auto remote = fs.OpenInode(remote_ino);
    if (!remote.ok()) return 1;
    auto remote_fork = ReadXattrFork(*image, fs.geometry(), remote.value());
    if (!remote_fork.ok() || remote_fork.value().remote_blocks == 0) {
      std::fprintf(stderr, "rw6d /remote did not allocate remote values\n");
      return 1;
    }
    auto bmbt = fs.OpenInode(bmbt_ino);
    if (!bmbt.ok()) return 1;
    auto bmbt_fork = ReadXattrFork(*image, fs.geometry(), bmbt.value());
    if (!bmbt_fork.ok() ||
        bmbt_fork.value().aformat != di::kFormatBtree) {
      std::fprintf(stderr, "rw6d /bmbt did not reach an attribute BMBT\n");
      return 1;
    }
    auto data = fs.OpenInode(data_ino);
    if (!data.ok() || data.value().format != di::kFormatBtree) {
      std::fprintf(stderr, "rw6d /data did not convert to a data btree\n");
      return 1;
    }
  }

  Status clean = fs.WriteCleanUnmountRecord();
  if (!clean.ok()) {
    std::fprintf(stderr, "rw6d clean unmount failed: %s\n",
                 clean.error().Format().c_str());
    return 1;
  }
  mounted.value().reset();
  if (!image->Flush()) {
    std::fprintf(stderr, "rw6d flush failed: %s\n", image->LastError().c_str());
    image->Close();
    return 1;
  }
  image->Close();
  std::printf("rw6d fixture: %s (1 KiB blocks, %zu files)\n",
              path_string.c_str(), files.size());
  std::printf("manifest: %s\n", manifest_path.c_str());
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
  if (argc == 3 && std::strcmp(argv[1], "--make-rw6c-fixture") == 0) {
    return MakeRw6cFixture(argv[2]);
  }
  if (argc == 3 && std::strcmp(argv[1], "--make-rw6d-fixture") == 0) {
    return MakeRw6dFixture(argv[2]);
  }
  std::string filter;
  if (argc == 2 && std::strncmp(argv[1], "--filter=", 9) == 0) {
    filter = argv[1] + 9;
  }
  if (argc >= 2 &&
      (std::strcmp(argv[1], "--help") == 0 ||
       std::strcmp(argv[1], "-h") == 0)) {
    std::cout << "usage: xfs_tests [--make-fixture <image>] "
                 "[--make-rw6-sparse <image>] "
                 "[--make-rw6c-fixture <image>] "
                 "[--make-rw6d-fixture <image>] [--filter=<substring>]\n"
                 "  no arguments: run the test suite\n"
                 "  --make-fixture <image>: write a fixture image (one "
                 "consistent filesystem with one file) for Linux verification\n"
                 "  --make-rw6-sparse <image>: write a modern-inode filesystem\n"
                 "      with a real sparse inode chunk for Linux verification\n"
                 "  --make-rw6c-fixture <image>: write a Windows-generated\n"
                 "      reflink+rmapbt image with the RW-6C logical fixture\n"
                 "  --make-rw6d-fixture <image>: write a Windows-generated\n"
                 "      1 KiB-block image with shortform/leaf/node/multi-level\n"
                 "      node, remote, attribute-BMBT and namespace attributes\n"
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
