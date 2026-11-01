// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// UX-1: read-only discovery, partition views and drive-letter allocation.
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "support/fixture_device.h"
#include "support/fixture_image.h"
#include "test_util.h"
#include "xfs/crc32c.h"
#include "xfs/device_discovery.h"
#include "xfs/drive_letter.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/ondisk.h"
#include "xfs/partition_device.h"

namespace {

using xfs::DeviceClass;
using xfs::DeviceBusType;
using xfs::DiscoveryOptions;
using xfs::DriveLetterKind;
using xfs::DriveLetterUsage;
using xfs::OffsetBlockDevice;
using xfs::ProbeXfsFilesystem;
using xfs::XfsProbeState;

// Builds a complete valid fixture in memory (300 MiB: the formatter profile
// needs the 64 MiB log).
std::unique_ptr<xfstest::MemoryBlockDevice> MakeFixture(
    xfs::Uuid* uuid_out = nullptr) {
  constexpr uint64_t kSize = 300ull << 20;
  auto device = std::make_unique<xfstest::MemoryBlockDevice>(kSize);
  xfstest::FixtureOptions options;
  options.size_bytes = kSize;
  options.uuid = {0x31, 0x42, 0x53, 0x64, 0x75, 0x86, 0x47, 0x98,
                  0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f, 0x10};
  xfstest::FixtureInfo info;
  CHECK(xfstest::BuildFixtureImage(*device, options, &info).ok());
  device->ResetCounters();
  if (uuid_out) *uuid_out = options.uuid;
  return device;
}

constexpr size_t kSbCrcOffset = xfs::ondisk::sb::kCrc;
constexpr size_t kSbIncompatOffset = xfs::ondisk::sb::kFeaturesIncompat;

}  // namespace

TEST(ux_offset_device_bounds_and_alignment) {
  auto base = std::make_unique<xfstest::MemoryBlockDevice>(1 << 20);
  for (size_t i = 0; i < base->bytes().size(); ++i) {
    base->bytes()[i] = static_cast<uint8_t>(i * 17 + 3);
  }
  const uint8_t* raw = base->bytes().data();
  auto view = OffsetBlockDevice::Create(std::move(base), 2 * 4096, 4096);
  CHECK(view.ok());
  uint8_t buf[512];
  CHECK(view.value()->Read(0, buf, sizeof(buf)));
  CHECK(std::memcmp(buf, raw + 2 * 4096, sizeof(buf)) == 0);
  CHECK(view.value()->Size() == 4096);
  CHECK(!view.value()->Read(4096 - 256, buf, 512));
  uint8_t replacement[512];
  std::memset(replacement, 0xAB, sizeof(replacement));
  CHECK(view.value()->Write(0, replacement, sizeof(replacement)));
  CHECK(view.value()->Inner().Size() == (1u << 20));
  view.value()->Close();

  auto misaligned = OffsetBlockDevice::Create(
      std::make_unique<xfstest::MemoryBlockDevice>(1 << 20), 123, 4096);
  CHECK(!misaligned.ok());

  auto beyond = OffsetBlockDevice::Create(
      std::make_unique<xfstest::MemoryBlockDevice>(1 << 20), 1 << 20, 4096);
  CHECK(!beyond.ok());

  auto empty = OffsetBlockDevice::Create(
      std::make_unique<xfstest::MemoryBlockDevice>(1 << 20), 0, 0);
  CHECK(!empty.ok());
}

TEST(ux_probe_valid_fixture_is_read_only) {
  xfs::Uuid uuid{};
  auto device = MakeFixture(&uuid);
  device->ResetCounters();
  DiscoveryOptions options;
  auto probe = ProbeXfsFilesystem(*device, 0, device->Size(), options);
  CHECK(probe.state == XfsProbeState::kValid);
  CHECK(probe.magic_seen);
  CHECK(probe.error_code == xfs::XfsError::kOk);
  CHECK(xfs::UuidEqual(probe.superblock.uuid, uuid));
  CHECK(probe.filesystem_bytes > 0);
  CHECK(probe.filesystem_bytes <= device->Size());
  CHECK(probe.free_bytes < probe.filesystem_bytes);
  CHECK(probe.log_state == xfs::LogState::kClean ||
        probe.log_state == xfs::LogState::kEmpty);
  CHECK(probe.writable_profile);
  CHECK_EQ(device->write_calls(), 0u);
  CHECK_EQ(device->flush_calls(), 0u);
}

TEST(ux_probe_no_xfs_is_not_reported_as_xfs) {
  xfstest::MemoryBlockDevice device(1 << 20);
  DiscoveryOptions options;
  auto probe = ProbeXfsFilesystem(device, 0, device.Size(), options);
  CHECK(probe.state == XfsProbeState::kNoXfs);
  CHECK(!probe.magic_seen);
  CHECK_EQ(probe.filesystem_bytes, 0u);
}

TEST(ux_probe_corrupt_superblock_is_rejected) {
  auto device = MakeFixture();
  // Flip one CRC byte: the magic stays valid, which is exactly the cheap
  // filter a discovery implementation must not trust.
  device->bytes()[kSbCrcOffset] ^= 0xFF;
  DiscoveryOptions options;
  auto probe = ProbeXfsFilesystem(*device, 0, device->Size(), options);
  CHECK(probe.state == XfsProbeState::kCorrupt);
  CHECK(probe.magic_seen);
  CHECK(probe.error_code == xfs::XfsError::kBadCrc);
  CHECK_EQ(device->write_calls(), 0u);
}

TEST(ux_probe_unsupported_feature_is_distinct) {
  auto device = MakeFixture();
  uint8_t* sb = device->bytes().data();
  const uint32_t incompat = xfs::GetBE32(sb, kSbIncompatOffset);
  xfs::PutBE32(sb, kSbIncompatOffset,
               incompat | xfs::ondisk::kIncompatMetadir);
  xfs::UpdateXfsChecksum(sb, 512, kSbCrcOffset);
  DiscoveryOptions options;
  auto probe = ProbeXfsFilesystem(*device, 0, device->Size(), options);
  CHECK(probe.state == XfsProbeState::kUnsupported);
  CHECK(probe.magic_seen);
  CHECK(probe.error_code == xfs::XfsError::kUnsupportedFeature);
  CHECK_EQ(device->write_calls(), 0u);
}

TEST(ux_probe_partition_offset) {
  auto source = MakeFixture();
  constexpr uint64_t kOffset = 1ull << 20;
  auto target = std::make_unique<xfstest::MemoryBlockDevice>(
      kOffset + source->Size());
  std::memcpy(target->bytes().data() + kOffset, source->bytes().data(),
              source->bytes().size());
  DiscoveryOptions options;
  auto probe = ProbeXfsFilesystem(*target, kOffset, source->Size(), options);
  CHECK(probe.state == XfsProbeState::kValid);
  CHECK(probe.filesystem_bytes > 0);
  CHECK(probe.filesystem_bytes <= source->Size());
  // A partition that claims to be smaller than the filesystem is corrupt.
  auto truncated = ProbeXfsFilesystem(*target, kOffset, 1ull << 20, options);
  CHECK(truncated.state == XfsProbeState::kCorrupt);
  CHECK(truncated.magic_seen);
  CHECK_EQ(target->write_calls(), 0u);
}

TEST(ux_probe_dirty_journal_is_reported) {
  auto device = MakeFixture();
  {
    auto borrowed = std::make_unique<xfstest::BorrowedBlockDevice>(device.get());
    xfs::MountOptions mount_options;
    mount_options.read_write = true;
    auto mounted =
        xfs::XfsFilesystem::Mount(std::move(borrowed), mount_options);
    CHECK(mounted.ok());
    auto created = mounted.value()->CreateRegularFile(
        mounted.value()->RootInode(), "dirty-file", 0644, 0, 0);
    CHECK(created.ok());
    // Destroy without WriteCleanUnmountRecord: the journal now has committed
    // records but no unmount record.
  }
  device->ResetCounters();
  DiscoveryOptions options;
  auto probe = ProbeXfsFilesystem(*device, 0, device->Size(), options);
  CHECK(probe.state == XfsProbeState::kValid);
  CHECK(probe.log_state == xfs::LogState::kDirty);
  CHECK_EQ(device->write_calls(), 0u);
  CHECK_EQ(device->flush_calls(), 0u);
}

TEST(ux_drive_letter_auto_and_preferred) {
  DriveLetterUsage usage;
  auto choice = xfs::ChooseDriveLetter("", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "E:");

  usage.used['E' - 'A'] = true;
  usage.kinds['E' - 'A'] = DriveLetterKind::kNetwork;
  choice = xfs::ChooseDriveLetter("", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "F:");

  // An explicit preferred letter is honored when free and normalized.
  choice = xfs::ChooseDriveLetter("g", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "G:");

  // The preferred letter being occupied never evicts the existing device.
  usage.used['G' - 'A'] = true;
  choice = xfs::ChooseDriveLetter("G:", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "H:");
  CHECK(choice.value().preferred_unavailable);
  CHECK(choice.value().note.find("already in use") != std::string::npos);
}

TEST(ux_drive_letter_auto_order_and_exhaustion) {
  DriveLetterUsage usage;
  for (char c = 'E'; c <= 'Z'; ++c) usage.used[c - 'A'] = true;
  auto choice = xfs::ChooseDriveLetter("", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "D:");
  usage.used['D' - 'A'] = true;
  choice = xfs::ChooseDriveLetter("", usage);
  CHECK(!choice.ok());
  CHECK(choice.error().code == xfs::XfsError::kNoSpace);
}

TEST(ux_drive_letter_validation) {
  CHECK(!xfs::NormalizeDriveLetter("AA:").ok());
  CHECK(!xfs::NormalizeDriveLetter("1:").ok());
  CHECK(!xfs::NormalizeDriveLetter("E:\\").ok());
  CHECK_STR_EQ(xfs::NormalizeDriveLetter("e:").value(), "E:");
  CHECK_STR_EQ(xfs::NormalizeDriveLetter("e").value(), "E:");
  CHECK(xfs::IsValidDriveLetter("Z:"));
  CHECK(!xfs::IsValidDriveLetter("Z"));
}