#include <filesystem>
#include <memory>

#include "test_util.h"
#include "xfs/block_device.h"
#include "xfs/filesystem.h"
#include "xfs/formatter.h"
#include "xfs/image_device.h"
#include "xfs/log_checker.h"
#include "xfs/superblock.h"

using namespace xfs;

namespace {

std::filesystem::path FormatFreshImage(uint64_t size = 300ull << 20) {
  auto path = xfstest::MakeTempImage(size);
  auto device = ImageBlockDevice::Open(path, /*writable=*/true, 512);
  if (!device) return {};
  mkfs::FormatOptions options;
  options.quiet = true;
  auto report = mkfs::Format(*device, options);
  if (!report.ok()) {
    std::filesystem::remove(path);
    return {};
  }
  return path;
}

}  // namespace

TEST(formatter_roundtrip) {
  auto path = FormatFreshImage();
  CHECK(!path.empty());

  auto device = ImageBlockDevice::Open(path, false, 512);
  CHECK(device != nullptr);
  MountOptions mount_options;
  auto fs = XfsFilesystem::Mount(std::move(device), mount_options);
  CHECK(fs.ok());
  if (!fs.ok()) {
    std::filesystem::remove(path);
    return;
  }

  const XfsGeometry& geo = fs.value()->geometry();
  CHECK_EQ(geo.block_size, 4096u);
  CHECK_EQ(geo.sector_size, 512u);
  CHECK_EQ(geo.inode_size, 512u);
  CHECK_EQ(geo.agcount, 4u);
  CHECK_EQ(geo.agblocks, 19200u);
  // mkfs writes a nonzero realtime extent size even without an rt device.
  CHECK_EQ(fs.value()->superblock().rextsize, 1u);
  CHECK_EQ(fs.value()->superblock().rootino, 64u);

  auto root = fs.value()->LookupPath("/");
  CHECK(root.ok());
  CHECK(root.value().IsDir());
  CHECK_EQ(root.value().nlink, 2u);
  CHECK_EQ(root.value().size, 6u);

  auto entries = fs.value()->ListDirectory(root.value().ino);
  CHECK(entries.ok());
  CHECK_EQ(entries.value().size(), 2u);
  bool has_dot = false, has_dotdot = false;
  for (const auto& e : entries.value()) {
    if (e.name == ".") has_dot = true;
    if (e.name == "..") has_dotdot = true;
  }
  CHECK(has_dot && has_dotdot);

  auto dotdot = fs.value()->LookupName(root.value().ino, "..");
  CHECK(dotdot.ok());
  CHECK_EQ(dotdot.value().ino, 64u);

  // Secondary superblocks must be readable copies.
  for (uint32_t agno = 1; agno < geo.agcount; ++agno) {
    uint64_t offset = geo.AgByteOffset(agno);
    auto sb = ReadSuperblock(fs.value()->device(), offset, true);
    CHECK(sb.ok());
    CHECK_EQ(sb.value().rootino, 64u);
  }

  // The formatter writes a clean log.
  auto log = CheckLogClean(fs.value()->device(), geo, fs.value()->superblock());
  CHECK(log.ok());
  CHECK(log.value().state == LogState::kClean);
  CHECK_EQ(log.value().newest_record_lsn, (1ull << 32));

  CheckReport report = fs.value()->CheckConsistency(CheckOptions{});
  if (report.Overall() != CheckStatus::kOk) {
    for (const auto& item : report.items) {
      std::printf("check item %s: %s %s\n", item.name.c_str(),
                  ToString(item.status), item.message.c_str());
    }
  }
  CHECK(report.Overall() == CheckStatus::kOk);

  fs.value().reset();
  std::filesystem::remove(path);
}

TEST(formatter_rejects_tiny_device) {
  auto path = xfstest::MakeTempImage(50ull << 20);
  auto device = ImageBlockDevice::Open(path, true, 512);
  CHECK(device != nullptr);
  mkfs::FormatOptions options;
  options.quiet = true;
  auto report = mkfs::Format(*device, options);
  CHECK(!report.ok());
  CHECK(report.error().code == XfsError::kUnsupportedGeometry);
  device->Close();
  device.reset();
  std::filesystem::remove(path);
}

TEST(formatter_geometry_scales) {
  // 1.2 GiB sparse image: 4 AGs, log ratio larger than the 64 MiB minimum.
  auto path = FormatFreshImage(1200ull << 20);
  CHECK(!path.empty());
  auto device = ImageBlockDevice::Open(path, false, 512);
  auto sb = ReadSuperblock(*device, 0, true);
  CHECK(sb.ok());
  CHECK_EQ(sb.value().agcount, 4u);
  CHECK_EQ(sb.value().blocksize, 4096u);
  device->Close();
  device.reset();
  std::filesystem::remove(path);
}

TEST(formatter_4k_sector_roundtrip) {
  // 4 KiB logical sectors move AGF/AGI/AGFL into blocks 1-3 and can split the
  // initial free extent around the inode chunk (alignment gap).
  auto device =
      std::make_unique<xfstest::MemoryBlockDevice>(300ull << 20, 4096);
  mkfs::FormatOptions options;
  options.quiet = true;
  auto report = mkfs::Format(*device, options);
  CHECK(report.ok());
  CHECK_EQ(report.value().sector_size, 4096u);
  // first_bno = prealloc(7) + freelist(4) = 11 -> aligned to 4 = 12,
  // root inode = 12 << inopblog(3) = 96.
  CHECK_EQ(report.value().root_ino, 96u);

  MountOptions mount_options;
  auto fs = XfsFilesystem::Mount(std::move(device), mount_options);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  const XfsGeometry& geo = fs.value()->geometry();
  CHECK_EQ(geo.sector_size, 4096u);
  auto root = fs.value()->LookupPath("/");
  CHECK(root.ok());
  CHECK_EQ(root.value().ino, 96u);
  auto entries = fs.value()->ListDirectory(root.value().ino);
  CHECK(entries.ok());
  CHECK_EQ(entries.value().size(), 2u);
  CheckReport report2 = fs.value()->CheckConsistency(CheckOptions{});
  CHECK(report2.Overall() == CheckStatus::kOk);
  if (report2.Overall() != CheckStatus::kOk) {
    for (const auto& item : report2.items) {
      std::printf("4k check item %s: %s %s\n", item.name.c_str(),
                  ToString(item.status), item.message.c_str());
    }
  }
  fs.value().reset();
}
