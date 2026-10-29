// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <cstring>
#include <filesystem>
#include <memory>

#include "test_util.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/formatter.h"
#include "xfs/image_device.h"
#include "xfs/log_checker.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"

using namespace xfs;

namespace {

std::filesystem::path FormatFreshImage() {
  auto path = xfstest::MakeTempImage(300ull << 20);
  auto device = ImageBlockDevice::Open(path, /*writable=*/true, 512);
  if (!device) return {};
  mkfs::FormatOptions options;
  options.quiet = true;
  auto report = mkfs::Format(*device, options);
  device->Close();
  device.reset();
  if (!report.ok()) return {};
  return path;
}

bool PatchByte(const std::filesystem::path& path, uint64_t offset,
               uint8_t value) {
  auto device = ImageBlockDevice::Open(path, true, 512);
  if (!device) return false;
  bool ok = device->Write(offset, &value, 1) && device->Flush();
  device->Close();
  return ok;
}

Result<std::unique_ptr<XfsFilesystem>> MountImage(
    const std::filesystem::path& path, bool force = false) {
  auto device = ImageBlockDevice::Open(path, false, 512);
  if (!device) {
    return XFS_ERR(XfsError::kIoError, "cannot open image");
  }
  MountOptions options;
  options.force_unsafe_ro = force;
  return XfsFilesystem::Mount(std::move(device), options);
}

void Drop(Result<std::unique_ptr<XfsFilesystem>>& fs) {
  if (fs.ok()) fs.value().reset();
}

void RemoveImage(const std::filesystem::path& path) {
  std::filesystem::remove(path);
}

// Known layout for a freshly formatted 300 MiB image (4 KiB blocks, 512 byte
// sectors, 4 AGs of 19200 blocks, log in AG2).
constexpr uint64_t kAgfCrcOffset = 512 + ondisk::agf::kCrc;  // AG0 AGF crc
constexpr uint64_t kBnoRootOffset = 4096;                    // AG0 bno root
constexpr uint64_t kInobtRootOffset = 3 * 4096;              // AG0 inobt root
constexpr uint64_t kRootInodeOffset = 8 * 4096;              // AG0 block 8

}  // namespace

TEST(corrupt_superblock_magic) {
  auto path = FormatFreshImage();
  CHECK(!path.empty());
  auto device = ImageBlockDevice::Open(path, true, 512);
  uint8_t zero[4] = {};
  CHECK(device->Write(0, zero, 4));
  device->Close();

  auto fs = MountImage(path);
  CHECK(!fs.ok());
  CHECK(fs.error().code == XfsError::kBadMagic);
  Drop(fs);
  RemoveImage(path);
}

TEST(corrupt_superblock_crc) {
  auto path = FormatFreshImage();
  CHECK(!path.empty());
  CHECK(PatchByte(path, ondisk::sb::kFname, 'X'));
  auto fs = MountImage(path);
  CHECK(!fs.ok());
  CHECK(fs.error().code == XfsError::kBadCrc);
  Drop(fs);
  RemoveImage(path);
}

TEST(corrupt_root_inode_magic) {
  auto path = FormatFreshImage();
  CHECK(!path.empty());
  CHECK(PatchByte(path, kRootInodeOffset, 0));
  auto fs = MountImage(path);
  CHECK(!fs.ok());
  CHECK(fs.error().code == XfsError::kBadMagic);
  Drop(fs);
  RemoveImage(path);
}

TEST(corrupt_agf_crc_is_detected_by_check) {
  auto path = FormatFreshImage();
  CHECK(!path.empty());
  auto device = ImageBlockDevice::Open(path, true, 512);
  uint8_t byte = 0;
  CHECK(device->Read(kAgfCrcOffset, &byte, 1));
  byte ^= 0xFF;
  CHECK(device->Write(kAgfCrcOffset, &byte, 1));
  device->Close();

  auto fs = MountImage(path);
  CHECK(fs.ok());  // mount does not read the AGF
  CheckReport report = fs.value()->CheckConsistency(CheckOptions{});
  CHECK(report.Overall() == CheckStatus::kCorrupt);
  Drop(fs);
  RemoveImage(path);
}

TEST(corrupt_bnobt_magic_is_detected_by_check) {
  auto path = FormatFreshImage();
  CHECK(!path.empty());
  auto device = ImageBlockDevice::Open(path, true, 512);
  uint32_t zero = 0;
  CHECK(device->Write(kBnoRootOffset, &zero, 4));
  device->Close();

  auto fs = MountImage(path);
  CHECK(fs.ok());
  CheckReport report = fs.value()->CheckConsistency(CheckOptions{});
  CHECK(report.Overall() == CheckStatus::kCorrupt);
  Drop(fs);
  RemoveImage(path);
}

TEST(corrupt_inobt_numrecs_is_detected_by_check) {
  auto path = FormatFreshImage();
  CHECK(!path.empty());
  auto device = ImageBlockDevice::Open(path, true, 512);
  uint16_t bogus = 0x7FFF;
  CHECK(device->Write(kInobtRootOffset + 6, &bogus, 2));
  device->Close();

  auto fs = MountImage(path);
  CHECK(fs.ok());
  CheckReport report = fs.value()->CheckConsistency(CheckOptions{});
  CHECK(report.Overall() == CheckStatus::kCorrupt);
  Drop(fs);
  RemoveImage(path);
}

TEST(unknown_log_content_refuses_mount_unless_forced) {
  auto path = FormatFreshImage();
  CHECK(!path.empty());

  {
    auto device = ImageBlockDevice::Open(path, true, 512);
    auto sb = ReadSuperblock(*device, 0, true);
    CHECK(sb.ok());
    XfsGeometry geo = MakeGeometry(sb.value());
    uint32_t zero = 0;
    CHECK(device->Write(geo.FsbToByte(sb.value().logstart), &zero, 4));
    device->Close();
  }

  {
    auto fs = MountImage(path, false);
    CHECK(!fs.ok());
    CHECK(fs.error().code == XfsError::kDirtyLog);
    Drop(fs);
  }
  {
    auto fs = MountImage(path, true);
    CHECK(fs.ok());
    CHECK(!fs.value()->warnings().empty());
    Drop(fs);
  }
  RemoveImage(path);
}
