// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// UX-1: read-only discovery, partition views and drive-letter allocation.
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "support/fixture_device.h"
#include "support/fixture_image.h"
#include "test_util.h"
#include "xfs/automount_config.h"
#include "xfs/automount_engine.h"
#include "xfs/crc32c.h"
#include "xfs/device_discovery.h"
#include "xfs/device_selection.h"
#include "xfs/drive_letter.h"
#include "xfs/drive_letter_takeover.h"
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
  // The journal scan must read through the same partition offset as the
  // superblock. Reading the log at whole-disk coordinates finds neither the
  // clean unmount record nor a dirty transaction and reports "unknown".
  CHECK(probe.log_state == xfs::LogState::kClean ||
        probe.log_state == xfs::LogState::kEmpty);
  // A partition that claims to be smaller than the filesystem is corrupt.
  auto truncated = ProbeXfsFilesystem(*target, kOffset, 1ull << 20, options);
  CHECK(truncated.state == XfsProbeState::kCorrupt);
  CHECK(truncated.magic_seen);

  // The same offset must let the scanner find a real transaction record.
  {
    auto borrowed = std::make_unique<xfstest::BorrowedBlockDevice>(source.get());
    xfs::MountOptions mount_options;
    mount_options.read_write = true;
    auto mounted =
        xfs::XfsFilesystem::Mount(std::move(borrowed), mount_options);
    CHECK(mounted.ok());
    auto created = mounted.value()->CreateRegularFile(
        mounted.value()->RootInode(), "dirty-partition-file", 0644, 0, 0);
    CHECK(created.ok());
    // Destroy without WriteCleanUnmountRecord: the newest log record is a
    // transaction, and discovery must report it as dirty at this offset.
  }
  std::memcpy(target->bytes().data() + kOffset, source->bytes().data(),
              source->bytes().size());
  auto dirty = ProbeXfsFilesystem(*target, kOffset, source->Size(), options);
  CHECK(dirty.state == XfsProbeState::kValid);
  CHECK(dirty.log_state == xfs::LogState::kDirty);
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

TEST(ux_drive_letter_auto_prefers_the_lowest_free_letter) {
  DriveLetterUsage usage;
  // C: is the system drive and is never chosen automatically; D: is the first
  // automatic candidate even when it is free.
  auto choice = xfs::ChooseDriveLetter("", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "D:");
  CHECK(!choice.value().preferred_unavailable);

  usage.used['D' - 'A'] = true;
  choice = xfs::ChooseDriveLetter("", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "E:");

  usage.used['E' - 'A'] = true;
  choice = xfs::ChooseDriveLetter("", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "F:");
}

TEST(ux_drive_letter_occupied_kinds_block_the_letter) {
  DriveLetterUsage usage;
  usage.used['D' - 'A'] = true;
  usage.kinds['D' - 'A'] = DriveLetterKind::kNetwork;
  usage.used['E' - 'A'] = true;
  usage.kinds['E' - 'A'] = DriveLetterKind::kSubst;
  usage.used['F' - 'A'] = true;
  usage.kinds['F' - 'A'] = DriveLetterKind::kCdrom;
  const auto choice = xfs::ChooseDriveLetter("", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "G:");
}

TEST(ux_drive_letter_preferred_and_fallback) {
  DriveLetterUsage usage;
  // A free preferred letter wins over the automatic order.
  auto choice = xfs::ChooseDriveLetter("g", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "G:");
  CHECK(!choice.value().preferred_unavailable);

  // An occupied preferred letter falls back to the first free letter from
  // D: through Z:, even when that letter is below the preference.
  usage.used['G' - 'A'] = true;
  usage.kinds['G' - 'A'] = DriveLetterKind::kNetwork;
  choice = xfs::ChooseDriveLetter("G:", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "D:");
  CHECK(choice.value().preferred_unavailable);
  CHECK(choice.value().note.find("already in use") != std::string::npos);

  // The fallback skips occupied letters in ascending order.
  usage.used['D' - 'A'] = true;
  choice = xfs::ChooseDriveLetter("E:", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "E:");
  CHECK(!choice.value().preferred_unavailable);
  usage.used['E' - 'A'] = true;
  choice = xfs::ChooseDriveLetter("E:", usage);
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "F:");
  CHECK(choice.value().preferred_unavailable);
}

TEST(ux_drive_letter_auto_order_and_exhaustion) {
  DriveLetterUsage usage;
  for (char c = 'D'; c <= 'Z'; ++c) usage.used[c - 'A'] = true;
  auto choice = xfs::ChooseDriveLetter("", usage);
  CHECK(!choice.ok());
  CHECK(choice.error().code == xfs::XfsError::kNoSpace);

  // An occupied preferred letter uses the same exhausted D:..Z: range.
  choice = xfs::ChooseDriveLetter("E:", usage);
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
  CHECK(xfs::IsDriveLetterRoot("F:"));
  CHECK(xfs::IsDriveLetterRoot("f:\\"));
  CHECK(!xfs::IsDriveLetterRoot("F:\\dir"));
  CHECK(!xfs::IsDriveLetterRoot("\\\\?\\Volume{test}\\"));
}

TEST(ux_drive_letter_takeover_priority) {
  DriveLetterUsage usage;
  usage.used['F' - 'A'] = true;  // the RAW volume's Windows letter

  // No preference: take over the existing same-volume letter.
  auto choice = xfs::ChooseDriveLetter("", usage, "F:");
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "F:");
  CHECK_STR_EQ(choice.value().release_drive, "F:");

  // A free preferred letter wins, but the original RAW letter is still
  // released so no unusable RAW drive remains.
  choice = xfs::ChooseDriveLetter("G:", usage, "F:");
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "G:");
  CHECK_STR_EQ(choice.value().release_drive, "F:");
  CHECK(choice.value().note.find("released") != std::string::npos);

  // An occupied preferred letter keeps the existing same-volume letter
  // instead of allocating an unrelated one.
  usage.used['G' - 'A'] = true;
  choice = xfs::ChooseDriveLetter("G:", usage, "F:");
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "F:");
  CHECK_STR_EQ(choice.value().release_drive, "F:");
  CHECK(choice.value().preferred_unavailable);

  // No existing same-volume letter: ordinary allocation, starting at D:.
  choice = xfs::ChooseDriveLetter("G:", usage, "");
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "D:");
  CHECK(choice.value().release_drive.empty());

  // A: / B: / C: are never taken over, and a syntactically valid but
  // non-takeover candidate is ignored rather than released.
  for (const char* letter : {"A:", "B:", "C:"}) {
    choice = xfs::ChooseDriveLetter("", usage, letter);
    CHECK(choice.ok());
    CHECK(choice.value().release_drive.empty());
    CHECK_STR_EQ(choice.value().drive, "D:");
  }
}

TEST(ux_drive_letter_reinsertion_remembers_the_letter) {
  DriveLetterUsage usage;

  // A takeover deletes the volume's Windows mount point, and Windows does not
  // recreate it on reinsertion.  The remembered letter wins over the first
  // free one so the device comes back where it was.
  auto choice = xfs::ChooseDriveLetter("", usage, "", "F:");
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "F:");
  CHECK(choice.value().release_drive.empty());
  CHECK(!choice.value().preferred_unavailable);

  // An explicitly configured free preference still wins.
  choice = xfs::ChooseDriveLetter("G:", usage, "", "F:");
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "G:");

  // If Windows did assign the volume a letter again, that letter is taken
  // over before the remembered one: no RAW drive may stay visible next to the
  // XFS mount.
  usage.used['F' - 'A'] = true;
  choice = xfs::ChooseDriveLetter("", usage, "F:", "D:");
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "F:");
  CHECK_STR_EQ(choice.value().release_drive, "F:");

  // An occupied remembered letter falls back to the ordinary D:..Z: order.
  usage.used['D' - 'A'] = true;
  choice = xfs::ChooseDriveLetter("", usage, "", "D:");
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "E:");

  // A:, B: and C: are never reused.
  choice = xfs::ChooseDriveLetter("", usage, "", "C:");
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "E:");

  // Reusing the remembered letter because the configured preference is
  // occupied is still a fallback: status must say the preference was not
  // honored, not silently treat the remembered letter as the choice.
  DriveLetterUsage occupied;
  occupied.used['G' - 'A'] = true;
  choice = xfs::ChooseDriveLetter("G:", occupied, "", "F:");
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "F:");
  CHECK(choice.value().preferred_unavailable);

  // With no configured preference, the remembered letter is the primary
  // choice and is not reported as a fallback.
  choice = xfs::ChooseDriveLetter("", occupied, "", "F:");
  CHECK(choice.ok());
  CHECK_STR_EQ(choice.value().drive, "F:");
  CHECK(!choice.value().preferred_unavailable);
}

TEST(ux_drive_letter_single_mount_point) {
  CHECK_STR_EQ(xfs::SingleDriveLetterMountPoint({"E:\\"}), "E:");
  CHECK_STR_EQ(xfs::SingleDriveLetterMountPoint({"e:\\", "D:\\dir"}), "E:");
  CHECK_STR_EQ(xfs::SingleDriveLetterMountPoint({"D:\\dir"}), "");
  // Two drive letters must never be silently reduced to one.
  CHECK_STR_EQ(xfs::SingleDriveLetterMountPoint({"E:\\", "F:\\"}), "");
  CHECK_STR_EQ(xfs::SingleDriveLetterMountPoint({}), "");
}

namespace {

constexpr const char* kConfigUuid = "31425364-7586-4798-a9ba-cbdcedfe0f10";

xfs::AutomountEntry WritableEntry() {
  xfs::AutomountEntry entry;
  CHECK(xfs::ParseUuid(kConfigUuid, &entry.fs_uuid));
  entry.enabled = true;
  entry.mode = xfs::AutomountMode::kReadWrite;
  entry.preferred_drive = "E:";
  entry.expected_serial = "AA00000000000489";
  entry.expected_stable_id = "{E3BDCF9A-D368-E874-81CB-EC5639EC18E6}";
  entry.expected_size_bytes = 64160400896ull;
  entry.device_qualification = true;
  entry.allow_removable_disk = true;
  return entry;
}

std::filesystem::path UniqueConfigPath(const char* tag) {
  static int counter = 0;
  return std::filesystem::temp_directory_path() /
         ("xfswin-ux-config-" + std::string(tag) + "-" +
          std::to_string(++counter));
}

}  // namespace

TEST(ux_uuid_text_roundtrip) {
  xfs::Uuid uuid{};
  CHECK(xfs::ParseUuid(kConfigUuid, &uuid));
  CHECK_EQ(xfs::FormatUuid(uuid), std::string(kConfigUuid));
  CHECK(!xfs::ParseUuid("31425364-7586-4798-a9ba-cbdcedfe0f1", &uuid));
  CHECK(!xfs::ParseUuid("31425364x7586-4798-a9ba-cbdcedfe0f10", &uuid));
  CHECK(!xfs::ParseUuid("31425364-7586-4798-a9ba-cbdcedfe0fgg", &uuid));
  std::string upper(kConfigUuid);
  for (char& c : upper) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 32);
  }
  CHECK(xfs::ParseUuid(upper, &uuid));
  CHECK_EQ(xfs::FormatUuid(uuid), std::string(kConfigUuid));
}

TEST(ux_automount_config_read_only_roundtrip) {
  const std::string text =
      "# comment\n"
      "version = 1\n"
      "debounce-ms = 900\n"
      "reconcile-seconds = 45\n"
      "\n"
      "[filesystem \"" + std::string(kConfigUuid) + "\"]\n"
      "enabled = true\n"
      "mode = read-only\n"
      "drive = e:\n"
      "expected-serial = SN-1\n";
  auto parsed = xfs::ParseAutomountConfig(text);
  CHECK(parsed.ok());
  const xfs::AutomountConfig& config = parsed.value().config;
  CHECK_EQ(config.version, 1u);
  CHECK_EQ(config.debounce_ms, 900u);
  CHECK_EQ(config.reconcile_seconds, 45u);
  CHECK_EQ(config.entries.size(), 1u);
  const xfs::AutomountEntry* entry = config.Find(WritableEntry().fs_uuid);
  CHECK(entry != nullptr);
  CHECK(entry->enabled);
  CHECK(entry->mode == xfs::AutomountMode::kReadOnly);
  CHECK_STR_EQ(entry->preferred_drive, "E:");
  CHECK_STR_EQ(entry->expected_serial, "SN-1");
  CHECK(!entry->device_qualification);
  CHECK(parsed.value().warnings.empty());

  auto again = xfs::ParseAutomountConfig(
      xfs::SerializeAutomountConfig(config));
  CHECK(again.ok());
  CHECK_EQ(again.value().config.entries.size(), 1u);
  CHECK(again.value().config.Find(entry->fs_uuid) != nullptr);
  CHECK_STR_EQ(again.value().config.Find(entry->fs_uuid)->preferred_drive,
               "E:");
}

TEST(ux_automount_config_defaults_to_read_only) {
  const std::string text =
      "[filesystem \"" + std::string(kConfigUuid) + "\"]\n"
      "enabled = true\n";
  auto parsed = xfs::ParseAutomountConfig(text);
  CHECK(parsed.ok());
  CHECK_EQ(parsed.value().config.entries.size(), 1u);
  const auto& entry = parsed.value().config.entries.front();
  CHECK(!entry.device_qualification);
  CHECK(entry.mode == xfs::AutomountMode::kReadOnly);
  CHECK(entry.preferred_drive.empty());
  CHECK(entry.enabled);
}

TEST(ux_automount_config_rw_requires_qualification_and_identity) {
  auto entry = WritableEntry();
  CHECK(xfs::ValidateAutomountEntry(entry).ok());

  xfs::AutomountEntry unqualified = entry;
  unqualified.device_qualification = false;
  auto refused = xfs::ValidateAutomountEntry(unqualified);
  CHECK(!refused.ok());
  CHECK(refused.error().code == xfs::XfsError::kPermissionDenied);

  xfs::AutomountEntry unpinned = entry;
  unpinned.expected_serial.clear();
  refused = xfs::ValidateAutomountEntry(unpinned);
  CHECK(!refused.ok());
  CHECK(refused.error().code == xfs::XfsError::kPermissionDenied);

  xfs::AutomountEntry no_size = entry;
  no_size.expected_size_bytes = 0;
  CHECK(!xfs::ValidateAutomountEntry(no_size).ok());

  xfs::AutomountEntry fixed = entry;
  fixed.allow_fixed_disk = true;
  refused = xfs::ValidateAutomountEntry(fixed);
  CHECK(!refused.ok());
  CHECK(refused.error().code == xfs::XfsError::kPermissionDenied);
  CHECK(refused.error().message.find("fixed physical disks") !=
        std::string::npos);

  const std::string text =
      "version = 1\n"
      "[filesystem \"" + std::string(kConfigUuid) + "\"]\n"
      "enabled = true\n"
      "mode = read-write\n"
      "drive = E:\n";
  auto parsed = xfs::ParseAutomountConfig(text);
  CHECK(!parsed.ok());
  CHECK(parsed.error().code == xfs::XfsError::kPermissionDenied);
}

TEST(ux_automount_config_writable_entry_roundtrip) {
  xfs::AutomountConfig config;
  config.Upsert(WritableEntry());
  const std::string serialized = xfs::SerializeAutomountConfig(config);
  CHECK(serialized.find("mode = read-write") != std::string::npos);
  CHECK(serialized.find("experimental-rw") == std::string::npos);
  CHECK(serialized.find("device-qualification = true") != std::string::npos);
  auto parsed = xfs::ParseAutomountConfig(serialized);
  CHECK(parsed.ok());
  const auto* entry = parsed.value().config.Find(WritableEntry().fs_uuid);
  CHECK(entry != nullptr);
  CHECK(entry->mode == xfs::AutomountMode::kReadWrite);
  CHECK_STR_EQ(entry->expected_stable_id,
               "{E3BDCF9A-D368-E874-81CB-EC5639EC18E6}");
  CHECK_EQ(entry->expected_size_bytes, 64160400896ull);
}

TEST(ux_automount_config_legacy_mode_migrates_on_save) {
  const std::string legacy =
      "version = 1\n"
      "[filesystem \"" + std::string(kConfigUuid) + "\"]\n"
      "enabled = true\n"
      "mode = experimental-rw\n"
      "drive = E:\n"
      "expected-serial = AA00000000000489\n"
      "expected-stable-id = {E3BDCF9A-D368-E874-81CB-EC5639EC18E6}\n"
      "expected-size = 64160400896\n"
      "expected-device-class = virtual-disk\n"
      "device-qualification = true\n";
  auto parsed = xfs::ParseAutomountConfig(legacy);
  CHECK(parsed.ok());
  const xfs::AutomountEntry* entry = parsed.value().config.Find(
      WritableEntry().fs_uuid);
  CHECK(entry != nullptr);
  if (!entry) return;
  CHECK(entry->mode == xfs::AutomountMode::kReadWrite);
  CHECK_STR_EQ(entry->expected_serial, "AA00000000000489");
  CHECK_STR_EQ(entry->expected_stable_id,
               "{E3BDCF9A-D368-E874-81CB-EC5639EC18E6}");
  CHECK_EQ(entry->expected_size_bytes, 64160400896ull);
  CHECK(entry->expected_device_class == xfs::DeviceClass::kVirtualDisk);
  CHECK(entry->device_qualification);

  const std::string serialized =
      xfs::SerializeAutomountConfig(parsed.value().config);
  CHECK(serialized.find("mode = read-write") != std::string::npos);
  CHECK(serialized.find("experimental-rw") == std::string::npos);
  auto reloaded = xfs::ParseAutomountConfig(serialized);
  CHECK(reloaded.ok());
  const xfs::AutomountEntry* again =
      reloaded.value().config.Find(WritableEntry().fs_uuid);
  CHECK(again != nullptr);
  if (!again) return;
  CHECK(again->mode == xfs::AutomountMode::kReadWrite);
  CHECK_STR_EQ(again->expected_serial, "AA00000000000489");
  CHECK_STR_EQ(again->expected_stable_id,
               "{E3BDCF9A-D368-E874-81CB-EC5639EC18E6}");
  CHECK_EQ(again->expected_size_bytes, 64160400896ull);
  CHECK(again->expected_device_class == xfs::DeviceClass::kVirtualDisk);
}

// Canonical and legacy mode spellings all resolve to the same internal mode.
TEST(ux_automount_mode_spellings_are_equivalent) {
  for (const char* spelling :
       {"read-write", "rw", "experimental-rw", "experimental"}) {
    xfs::AutomountMode mode = xfs::AutomountMode::kReadOnly;
    CHECK(xfs::ParseAutomountMode(spelling, &mode));
    CHECK(mode == xfs::AutomountMode::kReadWrite);
  }
  xfs::AutomountMode read_only = xfs::AutomountMode::kReadWrite;
  CHECK(xfs::ParseAutomountMode("read-only", &read_only));
  CHECK(read_only == xfs::AutomountMode::kReadOnly);
}

TEST(ux_automount_config_rejects_duplicates_and_newer_versions) {
  const std::string duplicate =
      "[filesystem \"" + std::string(kConfigUuid) + "\"]\n"
      "enabled = true\n"
      "[filesystem \"" + std::string(kConfigUuid) + "\"]\n"
      "enabled = false\n";
  auto parsed = xfs::ParseAutomountConfig(duplicate);
  CHECK(!parsed.ok());
  CHECK(parsed.error().code == xfs::XfsError::kAlreadyExists);

  auto newer = xfs::ParseAutomountConfig("version = 2\n");
  CHECK(!newer.ok());
  CHECK(newer.error().code == xfs::XfsError::kUnsupportedFeature);
}

TEST(ux_automount_config_warns_on_unknown_keys) {
  const std::string text =
      "version = 1\n"
      "future-key = yes\n"
      "[filesystem \"" + std::string(kConfigUuid) + "\"]\n"
      "enabled = true\n"
      "future-entry-key = yes\n";
  auto parsed = xfs::ParseAutomountConfig(text);
  CHECK(parsed.ok());
  CHECK_EQ(parsed.value().warnings.size(), 2u);
}

TEST(ux_automount_config_atomic_save_and_load) {
  const std::filesystem::path path = UniqueConfigPath("save");
  std::error_code ec;
  std::filesystem::remove_all(path, ec);

  auto missing = xfs::LoadAutomountConfig(path);
  CHECK(missing.ok());
  CHECK(missing.value().config.entries.empty());

  xfs::AutomountConfig config;
  config.debounce_ms = 1200;
  config.Upsert(WritableEntry());
  auto saved = xfs::SaveAutomountConfig(path, config);
  CHECK(saved.ok());
  CHECK(std::filesystem::exists(path));

  auto loaded = xfs::LoadAutomountConfig(path);
  CHECK(loaded.ok());
  CHECK_EQ(loaded.value().config.debounce_ms, 1200u);
  const auto* entry = loaded.value().config.Find(WritableEntry().fs_uuid);
  CHECK(entry != nullptr);
  CHECK(entry->mode == xfs::AutomountMode::kReadWrite);

  // The temporary file must not be left behind.
  for (const auto& child : std::filesystem::directory_iterator(
           path.parent_path(), ec)) {
    const std::string name = child.path().filename().string();
    CHECK(name.find("xfswin-ux-config-save") == std::string::npos ||
          name == path.filename().string());
  }
  std::filesystem::remove_all(path, ec);
}

// ---------------------------------------------------------------------------
// Automount engine: injected arrival/removal events, no real USB required.
// ---------------------------------------------------------------------------

namespace {

using xfs::AutomountClock;
using xfs::AutomountLogRecord;
using xfs::AutomountMountStart;
using xfs::AutomountTimePoint;

constexpr AutomountTimePoint kStart = AutomountClock::time_point{} +
                                      std::chrono::seconds(10000);

AutomountTimePoint At(int milliseconds) {
  return kStart + std::chrono::milliseconds(milliseconds);
}

xfs::Uuid TestUuid(uint8_t seed) {
  xfs::Uuid uuid{};
  for (size_t i = 0; i < uuid.size(); ++i) {
    uuid[i] = static_cast<uint8_t>(seed + i);
  }
  return uuid;
}

xfs::DiscoveredFilesystem TestFilesystem(
    const xfs::Uuid& uuid, const std::string& label, uint32_t disk_number,
    const std::string& serial,
    xfs::DeviceClass device_class = xfs::DeviceClass::kRemovableDisk,
    uint64_t disk_size = 64ull << 30) {
  xfs::DiscoveredFilesystem fs;
  fs.probe.state = xfs::XfsProbeState::kValid;
  fs.probe.magic_seen = true;
  fs.probe.superblock.uuid = uuid;
  fs.probe.label = label;
  fs.probe.filesystem_bytes = disk_size - (64ull << 20);
  fs.probe.free_bytes = fs.probe.filesystem_bytes / 2;
  fs.probe.log_state = xfs::LogState::kClean;
  fs.probe.writable_profile = true;
  fs.location.disk_number = disk_number;
  fs.location.partition_number = 0;
  fs.location.whole_disk = true;
  fs.location.disk_path = "\\\\.\\PhysicalDrive" + std::to_string(disk_number);
  fs.location.offset_bytes = 0;
  fs.location.length_bytes = disk_size;
  fs.location.disk_size_bytes = disk_size;
  fs.location.removable =
      device_class == xfs::DeviceClass::kRemovableDisk;
  fs.location.device_class = device_class;
  fs.location.serial_number = serial;
  fs.location.stable_id = "stable-" + serial;
  return fs;
}

xfs::AutomountEntry EngineEntry(const xfs::Uuid& uuid,
                                const std::string& preferred = "",
                                bool enabled = true,
                                xfs::AutomountMode mode =
                                    xfs::AutomountMode::kReadOnly) {
  xfs::AutomountEntry entry;
  entry.fs_uuid = uuid;
  entry.enabled = enabled;
  entry.mode = mode;
  entry.preferred_drive = preferred;
  return entry;
}

struct FakeSession {
  std::string id;
  std::string drive;
  xfs::DiscoveredFilesystem filesystem;
  xfs::AutomountEntry entry;
  xfs::AutomountMode mode = xfs::AutomountMode::kReadOnly;
};

class FakeAutomountPlatform final : public xfs::AutomountPlatform {
 public:
  struct Device {
    xfs::DiscoveredFilesystem filesystem;
    bool present = true;
  };

  std::vector<Device> devices;
  std::vector<xfs::DriveLetterInfo> occupied_letters;
  // Drive roots that currently resolve to a Windows volume GUID, keyed by
  // "F:".  Used by the restored-letter hint.
  std::map<std::string, std::string> drive_volumes;
  bool discovery_fails = false;
  bool discovery_incomplete = false;
  uint64_t retryable_failures = 0;
  std::vector<uint32_t> incomplete_disks;
  bool letters_fail = false;
  std::string letters_error = "drive-letter enumeration failed";
  int discover_calls = 0;
  int start_calls = 0;
  int stop_calls = 0;
  int stop_failures = 0;
  int retryable_mount_failures = 0;
  int mount_failures = 0;
  int takeover_calls = 0;
  int restore_calls = 0;
  int restore_failures = 0;
  bool takeover_fails = false;
  bool volume_present = true;
  std::vector<xfs::DriveLetterChoice> takeover_choices;
  std::map<std::string, FakeSession> sessions;
  std::vector<std::string> started_session_ids;
  std::vector<AutomountLogRecord> logs;
  int next_session = 1;

  xfs::Result<xfs::DiscoveryReport> Discover(
      const xfs::DiscoveryOptions&) override {
    ++discover_calls;
    if (discovery_fails) {
      return xfs::XFS_ERR(xfs::XfsError::kPermissionDenied, "discovery failed");
    }
    xfs::DiscoveryReport report;
    for (const auto& device : devices) {
      if (device.present) report.filesystems.push_back(device.filesystem);
    }
    report.incomplete = discovery_incomplete || !incomplete_disks.empty();
    report.retryable_failures = retryable_failures;
    report.incomplete_disks = incomplete_disks;
    return report;
  }

  std::vector<xfs::DriveLetterInfo> ListDriveLetters(std::string* error) override {
    if (letters_fail) {
      if (error) *error = letters_error;
      return {};
    }
    if (error) error->clear();
    std::vector<xfs::DriveLetterInfo> letters = occupied_letters;
    for (const auto& [id, session] : sessions) {
      (void)id;
      xfs::DriveLetterInfo info;
      info.letter = session.drive.empty() ? '?' : session.drive[0];
      info.kind = xfs::DriveLetterKind::kFixed;
      info.target = "xfswin";
      letters.push_back(info);
    }
    return letters;
  }

  bool DriveLetterResolvesToVolume(
      std::string_view drive, std::string_view volume_device_path) override {
    const auto it = drive_volumes.find(std::string(drive));
    return it != drive_volumes.end() &&
           it->second == volume_device_path;
  }

  xfs::TakeoverResult TakeOverDriveLetter(
      const xfs::DiscoveredFilesystem& filesystem,
      const xfs::DriveLetterChoice& choice) override {
    ++takeover_calls;
    takeover_choices.push_back(choice);
    xfs::TakeoverResult result;
    result.takeover.drive = choice.drive;
    if (choice.release_drive.empty()) {
      result.takeover.state = xfs::TakeoverState::kNone;
      return result;
    }
    if (takeover_fails) {
      result.status = xfs::XFS_ERR(xfs::XfsError::kIoError,
                                   "the drive-letter takeover failed");
      result.takeover.state = xfs::TakeoverState::kFailed;
      return result;
    }
    result.takeover.state = xfs::TakeoverState::kOriginalRemoved;
    result.takeover.mount_point = choice.release_drive + "\\";
    result.takeover.original_drive = choice.release_drive;
    result.takeover.volume_name = filesystem.volume_device_path + "\\";
    result.takeover.volume_device_path = filesystem.volume_device_path;
    result.takeover.fs_uuid = xfs::FormatUuid(
        filesystem.probe.superblock.uuid);
    result.takeover.disk_number = filesystem.location.disk_number;
    result.takeover.offset_bytes = filesystem.location.offset_bytes;
    result.takeover.length_bytes = filesystem.location.length_bytes;
    result.takeover.volume_present = true;
    return result;
  }

  xfs::Status RestoreDriveLetterTakeover(
      xfs::DriveLetterTakeover* takeover) override {
    ++restore_calls;
    if (!takeover) return xfs::OkStatus();
    if (restore_failures > 0) {
      --restore_failures;
      takeover->state = xfs::TakeoverState::kRestorePending;
      takeover->error = "injected restore failure";
      return xfs::XFS_ERR(xfs::XfsError::kIoError,
                          "injected restore failure");
    }
    takeover->volume_present = volume_present;
    takeover->state = xfs::TakeoverState::kRestored;
    takeover->error.clear();
    return xfs::OkStatus();
  }

  AutomountMountStart StartMount(const xfs::DiscoveredFilesystem& filesystem,
                                 const xfs::AutomountEntry& entry,
                                 xfs::AutomountMode mode,
                                 const std::string& drive) override {
    ++start_calls;
    if (mount_failures > 0) {
      --mount_failures;
      AutomountMountStart failed;
      failed.status = xfs::XFS_ERR(xfs::XfsError::kIoError,
                                   "injected WinFsp mount failure");
      return failed;
    }
    if (retryable_mount_failures > 0) {
      --retryable_mount_failures;
      AutomountMountStart failed;
      failed.status = xfs::XFS_ERR(xfs::XfsError::kIoError,
                                   "the device is not ready");
      failed.retryable = true;
      return failed;
    }
    FakeSession session;
    session.id = "session-" + std::to_string(next_session++);
    session.drive = drive;
    session.filesystem = filesystem;
    session.entry = entry;
    session.mode = mode;
    sessions[session.id] = session;
    started_session_ids.push_back(session.id);
    AutomountMountStart started;
    started.session_id = session.id;
    return started;
  }

  xfs::Status StopMount(const std::string& session_id) override {
    ++stop_calls;
    if (stop_failures > 0) {
      --stop_failures;
      return xfs::XFS_ERR(xfs::XfsError::kIoError,
                          "the mount session did not stop in time");
    }
    sessions.erase(session_id);
    return xfs::OkStatus();
  }

  bool IsSessionAlive(const std::string& session_id) override {
    return sessions.find(session_id) != sessions.end();
  }

  void EmitLog(const AutomountLogRecord& record) override {
    logs.push_back(record);
  }

  int CountLogs(const std::string& event, const std::string& result) const {
    int count = 0;
    for (const auto& log : logs) {
      if (log.event == event && log.result == result) ++count;
    }
    return count;
  }
};

xfs::AutomountConfig ConfigWith(const std::vector<xfs::AutomountEntry>& entries,
                                uint32_t debounce_ms = 0) {
  xfs::AutomountConfig config;
  config.debounce_ms = debounce_ms;
  config.reconcile_seconds = 60;
  for (const auto& entry : entries) config.Upsert(entry);
  return config;
}

xfs::AutomountSettings FastRetrySettings() {
  xfs::AutomountSettings settings;
  settings.initial_retry_ms = 100;
  settings.max_retry_ms = 1000;
  settings.max_ready_retries = 3;
  return settings;
}

}  // namespace

TEST(ux_automount_start_mounts_configured_filesystem) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x10);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);

  CHECK_EQ(platform.sessions.size(), 1u);
  const auto& session = platform.sessions.begin()->second;
  CHECK_STR_EQ(session.drive, "D:");
  CHECK(session.entry.enabled);
  CHECK(session.entry.mode == xfs::AutomountMode::kReadOnly);
  CHECK_EQ(engine.status().mounts.size(), 1u);
  CHECK_STR_EQ(engine.status().mounts.front().drive, "D:");
  CHECK_STR_EQ(engine.status().mounts.front().label, "USB1");
  CHECK_EQ(engine.status().entries.size(), 1u);
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kMounted);
  CHECK_EQ(platform.CountLogs("mount-result", "mounted"), 1);
  CHECK(engine.NextWake(kStart).has_value());
}

TEST(ux_automount_preferred_letter_fallback_is_reported) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x20);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), true});
  xfs::DriveLetterInfo occupied;
  occupied.letter = 'E';
  occupied.kind = xfs::DriveLetterKind::kNetwork;
  occupied.target = "\\\\server\\share";
  platform.occupied_letters.push_back(occupied);

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid, "E:")}));
  engine.Start(kStart);
  engine.Tick(kStart);

  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_STR_EQ(platform.sessions.begin()->second.drive, "D:");
  CHECK(engine.status().entries.front().detail.find("already in use") !=
        std::string::npos);
}

TEST(ux_automount_two_filesystems_same_label_are_independent) {
  FakeAutomountPlatform platform;
  const xfs::Uuid first = TestUuid(0x30);
  const xfs::Uuid second = TestUuid(0x50);
  platform.devices.push_back({TestFilesystem(first, "USB", 1, "SN1"), true});
  platform.devices.push_back({TestFilesystem(second, "USB", 2, "SN2"), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(first), EngineEntry(second)}));
  engine.Start(kStart);
  engine.Tick(kStart);

  CHECK_EQ(platform.sessions.size(), 2u);
  CHECK_EQ(engine.status().mounts.size(), 2u);
  std::string first_drive;
  std::string second_drive;
  for (const auto& [id, session] : platform.sessions) {
    (void)id;
    CHECK_STR_EQ(session.filesystem.probe.label, "USB");
    if (xfs::UuidEqual(session.filesystem.probe.superblock.uuid, first)) {
      first_drive = session.drive;
    } else {
      second_drive = session.drive;
    }
  }
  CHECK_STR_EQ(first_drive, "D:");
  CHECK_STR_EQ(second_drive, "E:");
}

TEST(ux_automount_reinsertion_reclaims_the_preferred_letter) {
  FakeAutomountPlatform platform;
  const xfs::Uuid first = TestUuid(0x32);
  const xfs::Uuid second = TestUuid(0x52);
  platform.devices.push_back({TestFilesystem(first, "USB", 1, "SN1"), true});
  platform.devices.push_back({TestFilesystem(second, "USB", 2, "SN2"), true});

  xfs::AutomountEngine engine(platform);
  // Both prefer D:.  The second device must not collide with the first, and
  // the first device must get D: back after a removal/reinsertion cycle.
  engine.SetConfig(ConfigWith(
      {EngineEntry(first, "D:"), EngineEntry(second, "D:")}));
  engine.Start(kStart);
  engine.Tick(kStart);

  auto drive_for = [&platform](const xfs::Uuid& uuid) {
    for (const auto& [id, session] : platform.sessions) {
      (void)id;
      if (xfs::UuidEqual(session.filesystem.probe.superblock.uuid, uuid)) {
        return session.drive;
      }
    }
    return std::string();
  };
  CHECK_STR_EQ(drive_for(first), "D:");
  CHECK_STR_EQ(drive_for(second), "E:");

  // Removing the D: device leaves the other mount alone.
  platform.devices[0].present = false;
  engine.NotifyDeviceChanged(At(1000));
  engine.Tick(At(1000));
  CHECK_STR_EQ(drive_for(first), "");
  CHECK_STR_EQ(drive_for(second), "E:");

  // Reinserting it reclaims the preferred letter without touching the other.
  platform.devices[0].present = true;
  engine.NotifyDeviceChanged(At(2000));
  engine.Tick(At(2000));
  CHECK_STR_EQ(drive_for(first), "D:");
  CHECK_STR_EQ(drive_for(second), "E:");
}

TEST(ux_automount_removal_then_reinsertion_uses_a_fresh_session) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x60);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid, "E:")}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.started_session_ids.size(), 1u);
  const std::string first_session = platform.started_session_ids.front();

  platform.devices.front().present = false;
  engine.NotifyDeviceChanged(At(1000));
  engine.Tick(At(1000));
  CHECK(platform.sessions.empty());
  CHECK_EQ(engine.status().mounts.size(), 0u);
  CHECK_EQ(platform.CountLogs("unmount", "unmounted"), 1);

  // Reinsertion re-enumerates the device: the disk number changes, which must
  // not let the old handle or session be reused.
  platform.devices.front().present = true;
  platform.devices.front().filesystem.location.disk_number = 3;
  platform.devices.front().filesystem.location.disk_path =
      "\\\\.\\PhysicalDrive3";
  engine.NotifyDeviceChanged(At(2000));
  engine.Tick(At(2000));

  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_EQ(platform.started_session_ids.size(), 2u);
  CHECK(platform.started_session_ids.back() != first_session);
  CHECK_STR_EQ(platform.sessions.begin()->second.drive, "E:");
  CHECK_EQ(engine.status().mounts.size(), 1u);
}

TEST(ux_automount_debounces_a_device_event_burst) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x70);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), true});

  xfs::AutomountEngine engine(platform, FastRetrySettings());
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}, 200));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.discover_calls, 1);

  for (int i = 0; i < 5; ++i) {
    engine.NotifyDeviceChanged(At(10 + i * 10));
  }
  engine.Tick(At(100));
  CHECK_EQ(platform.discover_calls, 1);
  engine.Tick(At(300));
  CHECK_EQ(platform.discover_calls, 2);
}

TEST(ux_automount_not_ready_retries_are_bounded) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x80);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), false});
  platform.discovery_incomplete = true;
  platform.retryable_failures = 1;

  xfs::AutomountEngine engine(platform, FastRetrySettings());
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kRetrying);
  CHECK_EQ(engine.status().entries.front().retry_attempt, 1u);
  auto wake = engine.NextWake(kStart);
  CHECK(wake.has_value());
  CHECK(*wake == At(100));

  engine.Tick(At(100));
  CHECK_EQ(engine.status().entries.front().retry_attempt, 2u);
  engine.Tick(At(300));
  CHECK_EQ(engine.status().entries.front().retry_attempt, 3u);
  engine.Tick(At(700));
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kFailed);
  CHECK_EQ(platform.discover_calls, 4);
  CHECK(platform.sessions.empty());

  // The fast retries stop; only the low-frequency reconciliation remains.
  wake = engine.NextWake(At(700));
  CHECK(wake.has_value());
  CHECK(*wake > At(10 * 1000));
}

TEST(ux_automount_mount_retry_only_for_not_ready) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x90);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), true});
  platform.retryable_mount_failures = 2;

  xfs::AutomountEngine engine(platform, FastRetrySettings());
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kRetrying);
  CHECK_EQ(platform.start_calls, 1);
  engine.Tick(At(100));
  CHECK_EQ(platform.start_calls, 2);
  engine.Tick(At(300));
  CHECK_EQ(platform.start_calls, 3);
  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kMounted);
}

TEST(ux_automount_filesystem_refusal_is_not_retried) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0xa0);
  xfs::DiscoveredFilesystem dirty =
      TestFilesystem(uuid, "USB1", 1, "SN1");
  dirty.probe.log_state = xfs::LogState::kDirty;
  platform.devices.push_back({dirty, true});

  xfs::AutomountEngine engine(platform, FastRetrySettings());
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.start_calls, 0);
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kRefused);
  CHECK(engine.status().entries.front().detail.find("journal") !=
        std::string::npos);
  CHECK_EQ(platform.CountLogs("entry-refused", "refused"), 1);

  // A low-frequency reconciliation re-evaluates the filesystem but does not
  // spin on a retry and does not repeat the same refusal log.
  engine.Tick(At(61000));
  CHECK_EQ(platform.start_calls, 0);
  CHECK_EQ(platform.CountLogs("entry-refused", "refused"), 1);
}

TEST(ux_automount_read_write_policy_preserves_device_class_boundaries) {
  FakeAutomountPlatform platform;
  const xfs::Uuid fixed_uuid = TestUuid(0xb0);
  platform.devices.push_back(
      {TestFilesystem(fixed_uuid, "FIXED", 0, "SNF",
                      xfs::DeviceClass::kFixedDisk),
       true});

  xfs::AutomountEngine engine(platform, FastRetrySettings());
  xfs::AutomountEntry fixed_entry =
      EngineEntry(fixed_uuid, "", true,
                  xfs::AutomountMode::kReadWrite);
  fixed_entry.device_qualification = true;
  fixed_entry.expected_serial = "SNF";
  fixed_entry.expected_stable_id = "stable-SNF";
  fixed_entry.expected_size_bytes = 64ull << 30;
  engine.SetConfig(ConfigWith({fixed_entry}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.start_calls, 0);
  CHECK(engine.status().entries.front().detail.find("fixed physical disks") !=
        std::string::npos);

  // Removable read-write needs the explicit class opt-in as well.
  FakeAutomountPlatform removable_platform;
  const xfs::Uuid removable_uuid = TestUuid(0xc0);
  removable_platform.devices.push_back(
      {TestFilesystem(removable_uuid, "USB", 1, "SNR"), true});
  xfs::AutomountEngine removable_engine(removable_platform,
                                        FastRetrySettings());
  xfs::AutomountEntry entry =
      EngineEntry(removable_uuid, "", true,
                  xfs::AutomountMode::kReadWrite);
  entry.device_qualification = true;
  entry.expected_serial = "SNR";
  entry.expected_stable_id = "stable-SNR";
  entry.expected_size_bytes = 64ull << 30;
  removable_engine.SetConfig(ConfigWith({entry}));
  removable_engine.Start(kStart);
  removable_engine.Tick(kStart);
  CHECK(removable_platform.sessions.empty());
  CHECK(removable_engine.status().entries.front().detail.find(
            "removable-disk") != std::string::npos);

  entry.allow_removable_disk = true;
  removable_engine.SetConfig(ConfigWith({entry}));
  removable_engine.NotifySettingsChanged(At(1000));
  removable_engine.Tick(At(1000));
  CHECK_EQ(removable_platform.sessions.size(), 1u);
  CHECK(removable_platform.sessions.begin()->second.entry.mode ==
        xfs::AutomountMode::kReadWrite);
}

TEST(ux_automount_identity_pin_mismatch_refuses_mount) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0xd0);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), true});

  xfs::AutomountEngine engine(platform);
  xfs::AutomountEntry entry = EngineEntry(uuid);
  entry.expected_serial = "OTHER";
  engine.SetConfig(ConfigWith({entry}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK(platform.sessions.empty());
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kRefused);
  CHECK(engine.status().entries.front().detail.find("serial") !=
        std::string::npos);
}

TEST(ux_automount_duplicate_uuid_is_ambiguous_without_identity_pins) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0xe0);
  platform.devices.push_back({TestFilesystem(uuid, "COPY", 1, "SN1"), true});
  platform.devices.push_back({TestFilesystem(uuid, "COPY", 2, "SN2"), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK(platform.sessions.empty());
  CHECK(engine.status().entries.front().detail.find("share this XFS UUID") !=
        std::string::npos);

  xfs::AutomountEntry pinned = EngineEntry(uuid);
  pinned.expected_serial = "SN2";
  engine.SetConfig(ConfigWith({pinned}));
  engine.NotifySettingsChanged(At(1000));
  engine.Tick(At(1000));
  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_STR_EQ(platform.sessions.begin()->second.filesystem.location
                   .serial_number,
               "SN2");
}

TEST(ux_automount_disable_unmounts_only_the_automount_session) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0xf0);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.sessions.size(), 1u);

  xfs::AutomountConfig disabled = engine.config();
  disabled.entries.front().enabled = false;
  engine.SetConfig(disabled);
  engine.NotifySettingsChanged(At(1000));
  engine.Tick(At(1000));
  CHECK(platform.sessions.empty());
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kDisabled);
  CHECK_EQ(platform.CountLogs("unmount", "unmounted"), 1);
}

TEST(ux_automount_external_mount_is_not_duplicated) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x11);
  xfs::DiscoveredFilesystem filesystem =
      TestFilesystem(uuid, "USB1", 1, "SN1");
  filesystem.xfswin_mounted = true;
  filesystem.xfswin_drive = "G:";
  platform.devices.push_back({filesystem, true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.start_calls, 0);
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kMountedElsewhere);
  CHECK_STR_EQ(engine.status().entries.front().drive, "G:");
}

TEST(ux_automount_drive_enumeration_failure_refuses_mount) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x22);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), true});
  platform.letters_fail = true;

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.start_calls, 0);
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kFailed);
  CHECK(engine.status().entries.front().detail.find("enumerated") !=
        std::string::npos);
}

TEST(ux_automount_terminal_session_failure_cleans_up) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x33);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.sessions.size(), 1u);
  const std::string session_id = platform.started_session_ids.front();

  platform.devices.front().present = false;
  engine.OnSessionTerminated(At(1000), session_id,
                             "The XFS device was removed.");
  CHECK(platform.sessions.empty());
  engine.Tick(At(1250));
  CHECK_EQ(engine.status().mounts.size(), 0u);
  CHECK(engine.status().entries.front().state !=
        xfs::AutomountEntryState::kMounted);

  // A reinsertion creates a fresh session, never the terminated one.
  platform.devices.front().present = true;
  platform.devices.front().filesystem.location.disk_number = 4;
  engine.NotifyDeviceChanged(At(2000));
  engine.Tick(At(2000));
  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK(platform.started_session_ids.size() == 2);
  CHECK(platform.sessions.begin()->first != session_id);
}

TEST(ux_automount_stop_failure_is_retried_before_remount) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x77);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.sessions.size(), 1u);

  platform.stop_failures = 1;
  platform.devices.front().present = false;
  engine.NotifyDeviceChanged(At(1000));
  engine.Tick(At(1000));
  // The session is being stopped but still owns its drive letter; the engine
  // must not start a replacement for the same UUID in the meantime.
  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_EQ(platform.start_calls, 1);
  CHECK_EQ(engine.status().mounts.size(), 0u);
  auto wake = engine.NextWake(At(1000));
  CHECK(wake.has_value());
  CHECK(*wake <= At(2000));

  engine.Tick(At(2000));
  CHECK(platform.sessions.empty());
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kWaitingForDevice);
}

TEST(ux_automount_one_failure_does_not_disturb_other_mounts) {
  FakeAutomountPlatform platform;
  const xfs::Uuid good = TestUuid(0x44);
  const xfs::Uuid bad = TestUuid(0x55);
  platform.devices.push_back({TestFilesystem(good, "GOOD", 1, "SNG"), true});
  xfs::DiscoveredFilesystem corrupt =
      TestFilesystem(bad, "BAD", 2, "SNB");
  corrupt.probe.state = xfs::XfsProbeState::kCorrupt;
  platform.devices.push_back({corrupt, true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(good), EngineEntry(bad)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_EQ(engine.status().mounts.size(), 1u);
  CHECK_STR_EQ(engine.status().mounts.front().label, "GOOD");
  CHECK(engine.status().entries.back().state ==
        xfs::AutomountEntryState::kWaitingForDevice);
}

TEST(ux_automount_config_loader_applies_changes) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x66);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), true});
  xfs::AutomountConfig current = ConfigWith({EngineEntry(uuid)}, 0);

  xfs::AutomountEngine engine(platform);
  engine.SetConfigLoader([&current]() {
    xfs::AutomountConfigLoad load;
    load.config = current;
    return load;
  });
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.sessions.size(), 1u);

  current.entries.front().enabled = false;
  engine.NotifySettingsChanged(At(1000));
  engine.Tick(At(1000));
  CHECK(platform.sessions.empty());
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kDisabled);
}

// ---------------------------------------------------------------------------
// Interactive selection: menu formatting and answer parsing without console IO.
// ---------------------------------------------------------------------------

TEST(ux_selection_menu_and_number_parsing) {
  xfs::DiscoveryReport report;
  const xfs::Uuid uuid = TestUuid(0x99);
  xfs::DiscoveredFilesystem filesystem =
      TestFilesystem(uuid, "USB1", 1, "SN1");
  report.filesystems.push_back(filesystem);

  const auto lines = xfs::FormatDeviceMenu(report);
  CHECK_EQ(lines.size(), 1u);
  CHECK(lines.front().find("USB1") != std::string::npos);
  CHECK(lines.front().find(xfs::FormatUuid(uuid)) != std::string::npos);
  CHECK(lines.front().find("removable") != std::string::npos);
  CHECK(lines.front().find("free") != std::string::npos);
  CHECK(lines.front().find("whole disk") != std::string::npos);
  CHECK(lines.front().find("journal") == std::string::npos);

  filesystem.probe.log_state = xfs::LogState::kDirty;
  report.filesystems.front() = filesystem;
  CHECK(xfs::FormatDeviceMenu(report).front().find("[journal dirty]") !=
        std::string::npos);
  filesystem.probe.log_state = xfs::LogState::kUnknown;
  report.filesystems.front() = filesystem;
  CHECK(xfs::FormatDeviceMenu(report).front().find("journal state unknown") !=
        std::string::npos);
  filesystem.probe.log_state = xfs::LogState::kClean;
  report.filesystems.front() = filesystem;

  auto selected = xfs::SelectDiscoveredFilesystem("1", report.filesystems);
  CHECK(selected.ok());
  CHECK_EQ(selected.value(), 0u);
  selected = xfs::SelectDiscoveredFilesystem(" #1 ", report.filesystems);
  CHECK(selected.ok());
  CHECK(!xfs::SelectDiscoveredFilesystem("0", report.filesystems).ok());
  CHECK(!xfs::SelectDiscoveredFilesystem("2", report.filesystems).ok());
  CHECK(!xfs::SelectDiscoveredFilesystem("", report.filesystems).ok());
  CHECK(!xfs::SelectDiscoveredFilesystem("3x", report.filesystems).ok());

  filesystem.xfswin_mounted = true;
  filesystem.xfswin_drive = "E:";
  CHECK(xfs::DescribeMountState(filesystem).find("mounted at E:") !=
        std::string::npos);
  report.filesystems.front() = filesystem;
  CHECK(xfs::FormatDeviceMenu(report).front().find("mounted at E:") !=
        std::string::npos);
}

TEST(ux_selection_accepts_uuid_and_unique_label) {
  std::vector<xfs::DiscoveredFilesystem> filesystems;
  const xfs::Uuid first = TestUuid(0xaa);
  const xfs::Uuid second = TestUuid(0xbb);
  filesystems.push_back(TestFilesystem(first, "alpha", 1, "SN1"));
  filesystems.push_back(TestFilesystem(second, "beta", 2, "SN2"));

  auto selected = xfs::SelectDiscoveredFilesystem("alpha", filesystems);
  CHECK(selected.ok());
  CHECK_EQ(selected.value(), 0u);
  selected = xfs::SelectDiscoveredFilesystem("BETA", filesystems);
  CHECK(selected.ok());
  CHECK_EQ(selected.value(), 1u);
  selected = xfs::SelectDiscoveredFilesystem(xfs::FormatUuid(second),
                                             filesystems);
  CHECK(selected.ok());
  CHECK_EQ(selected.value(), 1u);
  CHECK(!xfs::SelectDiscoveredFilesystem("gamma", filesystems).ok());

  filesystems.push_back(TestFilesystem(TestUuid(0xcc), "alpha", 3, "SN3"));
  auto ambiguous = xfs::SelectDiscoveredFilesystem("alpha", filesystems);
  CHECK(!ambiguous.ok());
  CHECK(ambiguous.error().message.find("more than one") != std::string::npos);
}

TEST(ux_selection_drive_answers) {
  auto selection = xfs::ParseDriveSelection("");
  CHECK(selection.ok());
  CHECK(selection.value().automatic);
  selection = xfs::ParseDriveSelection(" auto ");
  CHECK(selection.ok());
  CHECK(selection.value().automatic);

  selection = xfs::ParseDriveSelection("E");
  CHECK(selection.ok());
  CHECK(!selection.value().automatic);
  CHECK_STR_EQ(selection.value().drive, "E:");
  selection = xfs::ParseDriveSelection("f:");
  CHECK(selection.ok());
  CHECK_STR_EQ(selection.value().drive, "F:");
  selection = xfs::ParseDriveSelection("C:");
  CHECK(selection.ok());

  CHECK(!xfs::ParseDriveSelection("A:").ok());
  CHECK(!xfs::ParseDriveSelection("B:").ok());
  CHECK(!xfs::ParseDriveSelection("AA:").ok());
  CHECK(!xfs::ParseDriveSelection("1").ok());
}

TEST(ux_selection_lists_rejected_xfs_candidates) {
  xfs::DiscoveryReport report;
  xfs::RejectedFilesystem rejected;
  rejected.location.disk_number = 2;
  rejected.location.disk_path = "\\\\.\\PhysicalDrive2";
  rejected.location.partition_number = 1;
  rejected.probe.state = xfs::XfsProbeState::kCorrupt;
  rejected.probe.magic_seen = true;
  rejected.probe.label = "BROKEN";
  report.rejected.push_back(rejected);

  xfs::RejectedFilesystem plain;
  plain.probe.state = xfs::XfsProbeState::kNoXfs;
  plain.probe.magic_seen = false;
  report.rejected.push_back(plain);

  const auto lines = xfs::FormatRejectedDevices(report);
  CHECK_EQ(lines.size(), 1u);
  CHECK(lines.front().find("[!]") != std::string::npos);
  CHECK(lines.front().find("damaged") != std::string::npos);
  CHECK(lines.front().find("BROKEN") != std::string::npos);
  CHECK(lines.front().find("partition 1") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Drive-letter takeover transaction (portable state machine, fake Win32 ops).
// ---------------------------------------------------------------------------

namespace {

xfs::DiscoveredFilesystem TakeoverCandidate(const xfs::Uuid& uuid) {
  xfs::DiscoveredFilesystem fs = TestFilesystem(uuid, "TAKEOVER", 2, "SN-T");
  fs.windows_volume_found = true;
  fs.volume_identity_exact = true;
  fs.volume_unrecognized_raw = true;
  fs.volume_device_path = "\\\\?\\Volume{00000000-0000-0000-0000-000000000001}";
  fs.mount_points = {"F:\\"};
  return fs;
}

class FakeTakeoverOps final : public xfs::TakeoverOps {
 public:
  bool fail_capture = false;
  bool fail_remove = false;
  bool remove_verification_fails = false;
  bool volume_present = true;
  bool fail_restore = false;
  bool removed = false;
  bool restored = false;
  int capture_calls = 0;
  int remove_calls = 0;
  int restore_calls = 0;
  std::string last_release_volume_device_path;

  xfs::Status VerifyAndCapture(const xfs::DiscoveredFilesystem& filesystem,
                               std::string_view,
                               std::string_view release_drive,
                               std::string_view release_volume_device_path,
                               xfs::DriveLetterTakeover* takeover) override {
    ++capture_calls;
    last_release_volume_device_path =
        std::string(release_volume_device_path);
    if (fail_capture) {
      return xfs::XFS_ERR(xfs::XfsError::kIoError, "injected capture failure");
    }
    takeover->mount_point = std::string(release_drive) + "\\";
    takeover->original_drive = std::string(release_drive);
    takeover->volume_name = "\\\\?\\Volume{test}\\";
    takeover->volume_device_path = filesystem.volume_device_path;
    takeover->volume_present = true;
    takeover->other_mount_points = {"D:\\xfswin-mount"};
    return xfs::OkStatus();
  }

  xfs::Status RemoveMountPoint(xfs::DriveLetterTakeover*) override {
    ++remove_calls;
    if (fail_remove) {
      return xfs::XFS_ERR(xfs::XfsError::kIoError,
                          "injected mount-point removal failure");
    }
    removed = true;
    return xfs::OkStatus();
  }

  xfs::Status VerifyRemoved(const xfs::DriveLetterTakeover&) override {
    if (remove_verification_fails) {
      return xfs::XFS_ERR(xfs::XfsError::kAlreadyExists,
                          "the mount point is still attached");
    }
    return removed ? xfs::OkStatus()
                   : xfs::XFS_ERR(xfs::XfsError::kNotFound,
                                  "the mount point is not removed");
  }

  bool VolumePresent(const xfs::DriveLetterTakeover&) override {
    return volume_present;
  }

  xfs::Status RestoreMountPoint(xfs::DriveLetterTakeover*) override {
    ++restore_calls;
    if (fail_restore) {
      return xfs::XFS_ERR(xfs::XfsError::kIoError,
                          "injected mount-point restore failure");
    }
    restored = true;
    removed = false;
    return xfs::OkStatus();
  }

  xfs::Status VerifyRestored(const xfs::DriveLetterTakeover&) override {
    return restored ? xfs::OkStatus()
                    : xfs::XFS_ERR(xfs::XfsError::kNotFound,
                                   "the mount point is not restored");
  }
};

}  // namespace

TEST(ux_takeover_transaction_success_and_state_machine) {
  FakeTakeoverOps ops;
  const xfs::DiscoveredFilesystem filesystem =
      TakeoverCandidate(TestUuid(0x31));
  auto result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "F:");
  CHECK(result.status.ok());
  CHECK(result.takeover.state == xfs::TakeoverState::kOriginalRemoved);
  CHECK_STR_EQ(result.takeover.drive, "F:");
  CHECK_STR_EQ(result.takeover.mount_point, "F:\\");
  CHECK_STR_EQ(result.takeover.original_drive, "F:");
  CHECK_STR_EQ(result.takeover.volume_device_path,
               filesystem.volume_device_path);
  CHECK_EQ(result.takeover.other_mount_points.size(), 1u);
  CHECK(xfs::TakeoverNeedsRestore(result.takeover));
  CHECK_EQ(ops.capture_calls, 1);
  CHECK_EQ(ops.remove_calls, 1);

  xfs::MarkTakeoverMounting(&result.takeover);
  CHECK(result.takeover.state == xfs::TakeoverState::kMounting);
  xfs::MarkTakeoverOwned(&result.takeover);
  CHECK(result.takeover.state == xfs::TakeoverState::kOwned);
  CHECK_STR_EQ(xfs::ToString(result.takeover.state), "owned");

  // A different WinFsp letter than the released Windows letter is valid: the
  // original letter is still restored on removal.
  auto other = xfs::PrepareDriveLetterTakeover(ops, filesystem, "G:", "F:");
  CHECK(other.status.ok());
  CHECK_STR_EQ(other.takeover.drive, "G:");
  CHECK_STR_EQ(other.takeover.mount_point, "F:\\");
}

TEST(ux_takeover_noop_without_windows_letter) {
  FakeTakeoverOps ops;
  const xfs::DiscoveredFilesystem filesystem =
      TakeoverCandidate(TestUuid(0x32));
  auto result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "D:", "");
  CHECK(result.status.ok());
  CHECK(result.takeover.state == xfs::TakeoverState::kNone);
  CHECK(!xfs::TakeoverNeedsRestore(result.takeover));
  CHECK_EQ(ops.remove_calls, 0);
  xfs::MarkTakeoverMounting(&result.takeover);
  CHECK(result.takeover.state == xfs::TakeoverState::kNone);
}

TEST(ux_takeover_refuses_unsafe_candidates) {
  FakeTakeoverOps ops;
  xfs::DiscoveredFilesystem filesystem = TakeoverCandidate(TestUuid(0x33));

  // C: is the Windows system drive and is never taken over.
  auto result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "C:", "C:");
  CHECK(!result.status.ok());
  CHECK(result.takeover.state == xfs::TakeoverState::kFailed);
  CHECK_EQ(ops.remove_calls, 0);

  // A recognized filesystem is never detached.
  filesystem.volume_unrecognized_raw = false;
  result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "F:");
  CHECK(!result.status.ok());
  CHECK_EQ(ops.remove_calls, 0);
  filesystem.volume_unrecognized_raw = true;

  // A correlation that is not an exact extent match is display-only.
  filesystem.volume_identity_exact = false;
  result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "F:");
  CHECK(!result.status.ok());
  CHECK_EQ(ops.remove_calls, 0);
  filesystem.volume_identity_exact = true;

  // Multiple drive letters must never be silently reduced to one.
  filesystem.mount_points = {"E:\\", "F:\\"};
  result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "F:");
  CHECK(!result.status.ok());
  CHECK_EQ(ops.remove_calls, 0);
  filesystem.mount_points = {"F:\\"};

  // The requested release letter must be the volume's own letter.
  result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "E:");
  CHECK(!result.status.ok());
  CHECK_EQ(ops.remove_calls, 0);

  // The XFS verification must have passed.
  filesystem.probe.state = xfs::XfsProbeState::kCorrupt;
  result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "F:");
  CHECK(!result.status.ok());
  CHECK_EQ(ops.remove_calls, 0);
}

TEST(ux_takeover_accepts_a_live_verified_release_letter_after_a_restore) {
  FakeTakeoverOps ops;
  xfs::DiscoveredFilesystem filesystem = TakeoverCandidate(TestUuid(0x4d));
  const std::string volume_path = filesystem.volume_device_path;
  // The pass that follows a restore can run before the Mount Manager lists
  // the restored letter again; the caller verified it live.
  filesystem.windows_volume_found = false;
  filesystem.volume_identity_exact = false;
  filesystem.volume_unrecognized_raw = false;
  filesystem.mount_points.clear();

  // Without that live check the stale snapshot is refused before anything is
  // detached.
  auto result =
      xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "F:");
  CHECK(!result.status.ok());
  CHECK_EQ(ops.remove_calls, 0);
  CHECK_EQ(ops.capture_calls, 0);

  result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "F:",
                                           volume_path);
  CHECK(result.status.ok());
  CHECK(result.takeover.state == xfs::TakeoverState::kOriginalRemoved);
  CHECK_EQ(ops.capture_calls, 1);
  CHECK_STR_EQ(ops.last_release_volume_device_path, volume_path);
}

TEST(ux_takeover_capture_failure_has_no_side_effects) {
  FakeTakeoverOps ops;
  ops.fail_capture = true;
  const xfs::DiscoveredFilesystem filesystem =
      TakeoverCandidate(TestUuid(0x34));
  auto result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "F:");
  CHECK(!result.status.ok());
  CHECK(result.takeover.state == xfs::TakeoverState::kFailed);
  CHECK_EQ(ops.remove_calls, 0);
  CHECK(!xfs::TakeoverNeedsRestore(result.takeover));
}

TEST(ux_takeover_removal_failure_with_verified_removal_succeeds) {
  FakeTakeoverOps ops;
  ops.fail_remove = true;
  ops.removed = true;  // DeleteVolumeMountPointW failed but the point is gone
  const xfs::DiscoveredFilesystem filesystem =
      TakeoverCandidate(TestUuid(0x35));
  auto result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "F:");
  CHECK(result.status.ok());
  CHECK(result.takeover.state == xfs::TakeoverState::kOriginalRemoved);
}

TEST(ux_takeover_verification_failure_rolls_back) {
  FakeTakeoverOps ops;
  ops.remove_verification_fails = true;
  const xfs::DiscoveredFilesystem filesystem =
      TakeoverCandidate(TestUuid(0x36));
  auto result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "F:");
  CHECK(!result.status.ok());
  CHECK(result.takeover.state == xfs::TakeoverState::kRestored);
  CHECK_EQ(ops.restore_calls, 1);
  CHECK(!xfs::TakeoverNeedsRestore(result.takeover));
}

TEST(ux_takeover_restore_skips_a_removed_volume) {
  FakeTakeoverOps ops;
  const xfs::DiscoveredFilesystem filesystem =
      TakeoverCandidate(TestUuid(0x37));
  auto result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "F:");
  CHECK(result.status.ok());
  ops.volume_present = false;
  xfs::Status restored = xfs::RestoreDriveLetterTakeover(ops, &result.takeover);
  CHECK(restored.ok());
  CHECK(result.takeover.state == xfs::TakeoverState::kRestored);
  CHECK(!result.takeover.volume_present);
  // Never call SetVolumeMountPointW for a device that is no longer attached.
  CHECK_EQ(ops.restore_calls, 0);

  // A repeated restore is idempotent.
  CHECK(xfs::RestoreDriveLetterTakeover(ops, &result.takeover).ok());
  CHECK_EQ(ops.restore_calls, 0);
}

TEST(ux_takeover_restore_failure_is_pending_and_retryable) {
  FakeTakeoverOps ops;
  const xfs::DiscoveredFilesystem filesystem =
      TakeoverCandidate(TestUuid(0x38));
  auto result = xfs::PrepareDriveLetterTakeover(ops, filesystem, "F:", "F:");
  CHECK(result.status.ok());
  ops.fail_restore = true;
  xfs::Status failed = xfs::RestoreDriveLetterTakeover(ops, &result.takeover);
  CHECK(!failed.ok());
  CHECK(result.takeover.state == xfs::TakeoverState::kRestorePending);
  CHECK(xfs::TakeoverNeedsRestore(result.takeover));
  CHECK_EQ(ops.restore_calls, 1);

  ops.fail_restore = false;
  CHECK(xfs::RetryDriveLetterRestore(ops, &result.takeover).ok());
  CHECK(result.takeover.state == xfs::TakeoverState::kRestored);
  CHECK_EQ(ops.restore_calls, 2);
}

// ---------------------------------------------------------------------------
// Automount engine: native drive-letter takeover and read-write fallback.
// ---------------------------------------------------------------------------

namespace {

// A discovered XFS filesystem whose Windows volume is still RAW and has the
// single drive letter F:.
xfs::DiscoveredFilesystem MountedTakeoverFilesystem(const xfs::Uuid& uuid) {
  xfs::DiscoveredFilesystem fs = TakeoverCandidate(uuid);
  fs.location.disk_size_bytes = 64ull << 30;
  fs.location.length_bytes = 64ull << 30;
  fs.probe.filesystem_bytes = (64ull << 30) - (64ull << 20);
  return fs;
}

xfs::AutomountEntry RwEngineEntry(const xfs::Uuid& uuid,
                                  uint64_t disk_size = 64ull << 30) {
  xfs::AutomountEntry entry =
      EngineEntry(uuid, "", true, xfs::AutomountMode::kReadWrite);
  entry.expected_serial = "SN-T";
  entry.expected_stable_id = "stable-SN-T";
  entry.expected_size_bytes = disk_size;
  entry.expected_device_class = xfs::DeviceClass::kRemovableDisk;
  entry.device_qualification = true;
  entry.allow_removable_disk = true;
  return entry;
}

}  // namespace

TEST(ux_automount_takes_over_the_raw_volume_letter) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x41);
  platform.devices.push_back({MountedTakeoverFilesystem(uuid), true});
  xfs::DriveLetterInfo raw_letter;
  raw_letter.letter = 'F';
  raw_letter.kind = xfs::DriveLetterKind::kRemovable;
  platform.occupied_letters.push_back(raw_letter);

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);

  CHECK_EQ(platform.takeover_calls, 1);
  CHECK_EQ(platform.takeover_choices.size(), 1u);
  CHECK_STR_EQ(platform.takeover_choices.front().drive, "F:");
  CHECK_STR_EQ(platform.takeover_choices.front().release_drive, "F:");
  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_STR_EQ(platform.sessions.begin()->second.drive, "F:");
  CHECK(platform.sessions.begin()->second.mode ==
        xfs::AutomountMode::kReadOnly);

  const auto& mount = engine.status().mounts.front();
  CHECK(mount.drive_taken_over);
  CHECK_STR_EQ(mount.original_windows_drive, "F:");
  CHECK(!mount.read_write);
  CHECK(engine.status().entries.front().drive_taken_over);
  CHECK(platform.CountLogs("drive-letter-takeover", "taken-over") == 1);
}

TEST(ux_automount_takeover_failure_does_not_mount) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x42);
  platform.devices.push_back({MountedTakeoverFilesystem(uuid), true});
  platform.takeover_fails = true;

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);

  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.start_calls, 0);
  CHECK(platform.CountLogs("entry-refused", "failed") >= 1);
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kFailed);
}

TEST(ux_automount_winfsp_failure_restores_the_windows_letter) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x43);
  platform.devices.push_back({MountedTakeoverFilesystem(uuid), true});
  platform.mount_failures = 1;

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);

  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.takeover_calls, 1);
  CHECK_EQ(platform.restore_calls, 1);
  CHECK(platform.CountLogs("drive-letter-restore", "restored") == 1);
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kFailed);
}

TEST(ux_automount_retry_after_a_failed_mount_returns_to_the_restored_letter) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x4b);
  const xfs::DiscoveredFilesystem filesystem =
      MountedTakeoverFilesystem(uuid);
  platform.devices.push_back({filesystem, true});
  xfs::DriveLetterInfo raw_letter;
  raw_letter.letter = 'F';
  raw_letter.kind = xfs::DriveLetterKind::kRemovable;
  platform.occupied_letters.push_back(raw_letter);
  platform.drive_volumes["F:"] = filesystem.volume_device_path;
  platform.mount_failures = 1;

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);

  // The failed mount restored the RAW letter; no other letter was allocated.
  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.takeover_calls, 1);
  CHECK_EQ(platform.restore_calls, 1);
  CHECK(platform.CountLogs("drive-letter-restore", "restored") == 1);

  // A notification runs the next pass before the Mount Manager has refreshed
  // the volume's drive-letter list, so discovery no longer correlates the RAW
  // letter with the XFS candidate.  The restored hint is validated live and
  // the takeover releases F: again instead of allocating D: next to it.
  xfs::DiscoveredFilesystem stale = filesystem;
  stale.windows_volume_found = false;
  stale.volume_identity_exact = false;
  stale.volume_unrecognized_raw = false;
  stale.mount_points.clear();
  platform.devices.front().filesystem = stale;
  engine.NotifyDeviceChanged(At(1000));
  engine.Tick(At(1000));

  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_STR_EQ(platform.sessions.begin()->second.drive, "F:");
  CHECK_EQ(platform.takeover_calls, 2);
  CHECK_EQ(platform.takeover_choices.size(), 2u);
  CHECK_STR_EQ(platform.takeover_choices.back().release_drive, "F:");
  CHECK_STR_EQ(platform.takeover_choices.back().release_volume_device_path,
               filesystem.volume_device_path);
  CHECK(engine.status().mounts.front().drive_taken_over);
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kMounted);
}

TEST(ux_automount_restored_letter_hint_is_ignored_when_the_letter_moved) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x4c);
  const xfs::DiscoveredFilesystem filesystem =
      MountedTakeoverFilesystem(uuid);
  platform.devices.push_back({filesystem, true});
  xfs::DriveLetterInfo raw_letter;
  raw_letter.letter = 'F';
  raw_letter.kind = xfs::DriveLetterKind::kRemovable;
  platform.occupied_letters.push_back(raw_letter);
  platform.drive_volumes["F:"] = filesystem.volume_device_path;
  platform.mount_failures = 1;

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.takeover_calls, 1);

  // The letter was reassigned to a different volume between the restore and
  // the next pass.  The hint must not authorize a takeover; F: belongs to
  // someone else, so a normal free-letter allocation is correct.
  xfs::DiscoveredFilesystem stale = filesystem;
  stale.windows_volume_found = false;
  stale.volume_identity_exact = false;
  stale.volume_unrecognized_raw = false;
  stale.mount_points.clear();
  platform.devices.front().filesystem = stale;
  platform.drive_volumes["F:"] = "\\\\?\\Volume{other-volume}";
  engine.NotifyDeviceChanged(At(1000));
  engine.Tick(At(1000));

  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_STR_EQ(platform.sessions.begin()->second.drive, "D:");
  // The second pass still calls the platform, but as a plain allocation: no
  // letter of the other volume may be released.
  CHECK_EQ(platform.takeover_calls, 2);
  CHECK(platform.takeover_choices.back().release_drive.empty());
  CHECK(!engine.status().mounts.front().drive_taken_over);
}

TEST(ux_automount_removal_does_not_restore_to_a_dead_volume) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x44);
  platform.devices.push_back({MountedTakeoverFilesystem(uuid), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_EQ(platform.restore_calls, 0);

  platform.devices.front().present = false;
  platform.volume_present = false;
  engine.NotifyDeviceChanged(At(1000));
  engine.Tick(At(1000));

  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.restore_calls, 1);
  CHECK(platform.CountLogs("drive-letter-restore", "skipped-removed") == 1);
}

TEST(ux_automount_reinsertion_reuses_the_remembered_letter) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x45);
  platform.devices.push_back({MountedTakeoverFilesystem(uuid), true});
  xfs::DriveLetterInfo raw_letter;
  raw_letter.letter = 'F';
  raw_letter.kind = xfs::DriveLetterKind::kRemovable;
  platform.occupied_letters.push_back(raw_letter);

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_STR_EQ(platform.sessions.begin()->second.drive, "F:");
  CHECK(engine.status().mounts.front().drive_taken_over);

  // Remove the device while the takeover owns F:.  The letter is released and
  // no mount point is restored; the takeover deleted the only Windows one, so
  // a discovery after reinsertion sees no letter at all.
  platform.devices.front().present = false;
  platform.volume_present = false;
  platform.occupied_letters.clear();
  engine.NotifyDeviceChanged(At(1000));
  engine.Tick(At(1000));
  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.restore_calls, 1);
  CHECK(platform.CountLogs("drive-letter-restore", "skipped-removed") == 1);

  // The same filesystem comes back without a Windows letter.  The ordinary
  // allocator would pick D:; the remembered F: must win so the device returns
  // to where it was.
  xfs::DiscoveredFilesystem reinserted = MountedTakeoverFilesystem(uuid);
  reinserted.mount_points.clear();
  platform.devices.front().filesystem = reinserted;
  platform.devices.front().present = true;
  platform.volume_present = true;
  engine.NotifyDeviceChanged(At(2000));
  engine.Tick(At(2000));

  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_STR_EQ(platform.sessions.begin()->second.drive, "F:");
  CHECK_STR_EQ(platform.takeover_choices.back().drive, "F:");
  CHECK(platform.takeover_choices.back().release_drive.empty());
  CHECK(!engine.status().mounts.front().drive_taken_over);
}

TEST(ux_automount_unreadable_disk_does_not_drop_a_live_session) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x47);
  platform.devices.push_back({TestFilesystem(uuid, "LIVE", 2, "SN-LIVE"), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.sessions.size(), 1u);
  const uint32_t disk =
      platform.devices.front().filesystem.location.disk_number;

  // A read-write session holds the raw disk exclusively: discovery cannot open
  // it, so the UUID is missing from the report while the disk is reported as
  // incomplete.  That must not be treated as a removal.
  platform.devices.front().present = false;
  platform.incomplete_disks.push_back(disk);
  engine.NotifyDeviceChanged(At(1000));
  engine.Tick(At(1000));
  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_EQ(engine.status().mounts.size(), 1u);
  bool warned = false;
  for (const auto& warning : engine.status().warnings) {
    if (warning.find("could not be read while a mount is active") !=
        std::string::npos) {
      warned = true;
    }
  }
  CHECK(warned);

  // The disk is genuinely gone (not merely unreadable): the session stops.
  platform.incomplete_disks.clear();
  engine.NotifyDeviceChanged(At(2000));
  engine.Tick(At(2000));
  CHECK(platform.sessions.empty());
  CHECK(engine.status().mounts.empty());
}

TEST(ux_automount_service_stop_restores_the_windows_letter) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x45);
  platform.devices.push_back({MountedTakeoverFilesystem(uuid), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.sessions.size(), 1u);

  const auto report = engine.Stop(At(1000));
  CHECK(report.clean());
  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.restore_calls, 1);
  CHECK(platform.CountLogs("drive-letter-restore", "restored") == 1);
  CHECK(!engine.running());
}

TEST(ux_automount_service_stop_reports_a_live_session_failure) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x45);
  platform.devices.push_back({MountedTakeoverFilesystem(uuid), true});
  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.sessions.size(), 1u);
  platform.stop_failures = 2;

  const auto report = engine.Stop(At(1000));
  CHECK(!report.clean());
  CHECK(!report.failures.empty());
  CHECK(report.failures.front().find("did not stop in time") != std::string::npos);
  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_EQ(platform.restore_calls, 0);
  CHECK(!engine.running());
}

TEST(ux_automount_service_stop_reports_windows_letter_restore_failure) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x45);
  platform.devices.push_back({MountedTakeoverFilesystem(uuid), true});
  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.sessions.size(), 1u);
  platform.restore_failures = 2;

  const auto report = engine.Stop(At(1000));
  CHECK(!report.clean());
  CHECK(!report.failures.empty());
  CHECK(report.failures.back().find("injected restore failure") != std::string::npos);
  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.restore_calls, 2);
  CHECK(!engine.running());
}

TEST(ux_automount_rw_identity_mismatch_falls_back_to_read_only) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x46);
  platform.devices.push_back({MountedTakeoverFilesystem(uuid), true});

  xfs::AutomountEntry entry = RwEngineEntry(uuid);
  entry.expected_serial = "A-DIFFERENT-DEVICE";
  entry.expected_stable_id = "stable-A-DIFFERENT-DEVICE";

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({entry}));
  engine.Start(kStart);
  engine.Tick(kStart);

  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK(platform.sessions.begin()->second.mode ==
        xfs::AutomountMode::kReadOnly);
  const auto& mount = engine.status().mounts.front();
  CHECK(!mount.read_write);
  CHECK(mount.mode == xfs::AutomountMode::kReadOnly);
  CHECK(mount.configured_mode == xfs::AutomountMode::kReadWrite);
  CHECK(mount.mode_note.find("read-only") != std::string::npos);
  CHECK(engine.status().entries.front().effective_mode ==
        xfs::AutomountMode::kReadOnly);

  // The effective mode is stable across the periodic reconciliation: the
  // requested mode did not change, so the session must not be remounted.
  engine.Tick(At(61000));
  CHECK_EQ(platform.start_calls, 1);
  CHECK_EQ(platform.stop_calls, 0);
  CHECK_EQ(platform.sessions.size(), 1u);
}

TEST(ux_automount_rw_class_mismatch_falls_back_to_read_only) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x47);
  platform.devices.push_back({MountedTakeoverFilesystem(uuid), true});

  xfs::AutomountEntry entry = RwEngineEntry(uuid);
  entry.expected_device_class = xfs::DeviceClass::kVirtualDisk;

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({entry}));
  engine.Start(kStart);
  engine.Tick(kStart);

  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK(platform.sessions.begin()->second.mode ==
        xfs::AutomountMode::kReadOnly);
  CHECK(engine.status().mounts.front().mode_note.find("class") !=
        std::string::npos);
}

TEST(ux_automount_rw_matching_identity_mounts_read_write) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x48);
  platform.devices.push_back({MountedTakeoverFilesystem(uuid), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({RwEngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);

  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK(platform.sessions.begin()->second.mode ==
        xfs::AutomountMode::kReadWrite);
  CHECK(engine.status().mounts.front().read_write);
  CHECK(engine.status().mounts.front().drive_taken_over);
}

TEST(ux_automount_rw_refuses_a_disk_with_another_filesystem) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x49);
  xfs::DiscoveredFilesystem filesystem = MountedTakeoverFilesystem(uuid);
  filesystem.disk_has_other_filesystems = true;
  platform.devices.push_back({filesystem, true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({RwEngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);

  CHECK(platform.sessions.empty());
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kRefused);
  CHECK(engine.status().entries.front().detail.find("same physical disk") !=
        std::string::npos);
}

TEST(ux_automount_restore_pending_blocks_the_next_mount_attempt) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0x4a);
  platform.devices.push_back({MountedTakeoverFilesystem(uuid), true});
  platform.mount_failures = 1;
  platform.restore_failures = 2;

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);

  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.restore_calls, 1);
  CHECK_EQ(platform.start_calls, 1);
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kFailed);
  CHECK(engine.status().entries.front().detail.find("restored") !=
        std::string::npos);

  // The restore is retried; while it keeps failing, no new mount is started.
  engine.Tick(At(61000));
  CHECK_EQ(platform.restore_calls, 2);
  CHECK_EQ(platform.start_calls, 1);
  CHECK(platform.sessions.empty());

  // Once the restore succeeds, the next reconciliation may mount again.
  engine.Tick(At(121000));
  CHECK_EQ(platform.restore_calls, 3);
  CHECK_EQ(platform.start_calls, 2);
  CHECK_EQ(platform.sessions.size(), 1u);
}

// ---------------------------------------------------------------------------
// RW enrollment: device-class pin persistence.
// ---------------------------------------------------------------------------

TEST(ux_automount_config_device_class_roundtrip) {
  const std::string text =
      "version = 1\n"
      "[filesystem \"" + std::string(kConfigUuid) + "\"]\n"
      "enabled = true\n"
      "mode = read-write\n"
      "drive = F:\n"
      "expected-serial = SN1\n"
      "expected-stable-id = stable-SN1\n"
      "expected-size = 64160400896\n"
      "expected-device-class = removable-disk\n"
      "device-qualification = true\n"
      "allow-removable-disk = true\n";
  auto parsed = xfs::ParseAutomountConfig(text);
  CHECK(parsed.ok());
  const xfs::AutomountEntry* entry = parsed.value().config.entries.empty()
                                         ? nullptr
                                         : &parsed.value().config.entries[0];
  CHECK(entry != nullptr);
  CHECK(entry->expected_device_class == xfs::DeviceClass::kRemovableDisk);
  const std::string serialized =
      xfs::SerializeAutomountConfig(parsed.value().config);
  CHECK(serialized.find("expected-device-class = removable-disk") !=
        std::string::npos);

  xfs::AutomountConfig catalog;
  catalog.entries.push_back(*entry);
  const std::string normalized = xfs::SerializeAutomountConfig(catalog);
  auto reparsed = xfs::ParseAutomountConfig(normalized);
  CHECK(reparsed.ok());
  CHECK(reparsed.value().config.entries.front().expected_device_class ==
        xfs::DeviceClass::kRemovableDisk);
}

TEST(ux_automount_config_refuses_unsafe_device_class_pins) {
  auto parse_entry = [](const std::string& class_line, bool allow_removable,
                        bool qualification) {
    std::string text =
        "version = 1\n"
        "[filesystem \"" + std::string(kConfigUuid) + "\"]\n"
        "enabled = true\n"
        "mode = read-write\n"
        "expected-serial = SN1\n"
        "expected-stable-id = stable-SN1\n"
        "expected-size = 64160400896\n" +
        class_line + "device-qualification = " +
        (qualification ? "true" : "false") + "\n"
        "allow-removable-disk = " + (allow_removable ? "true" : "false") +
        "\n";
    return xfs::ParseAutomountConfig(text);
  };

  CHECK(!parse_entry("expected-device-class = fixed-disk\n", true, true).ok());
  CHECK(!parse_entry("expected-device-class = removable-disk\n", false, true)
             .ok());
  CHECK(!parse_entry("expected-device-class = made-up\n", true, true).ok());
  CHECK(parse_entry("expected-device-class = removable-disk\n", true, true)
            .ok());
  CHECK(parse_entry("expected-device-class = virtual-disk\n", false, true)
            .ok());
}

TEST(ux_automount_config_virtual_disk_needs_no_serial) {
  auto parse = [](const std::string& serial_line,
                  const std::string& class_line) {
    const std::string text =
        "version = 1\n"
        "[filesystem \"" + std::string(kConfigUuid) + "\"]\n"
        "enabled = true\n"
        "mode = read-write\n" +
        serial_line + "expected-stable-id = stable-SN1\n"
        "expected-size = 64160400896\n" +
        class_line + "device-qualification = true\n"
        "allow-removable-disk = true\n";
    return xfs::ParseAutomountConfig(text);
  };

  // Windows exposes no serial number for VHD/VHDX; the stable id, size and
  // class are the identity pins there.
  auto virtual_disk =
      parse("", "expected-device-class = virtual-disk\n");
  CHECK(virtual_disk.ok());
  CHECK(virtual_disk.value().config.entries.front().expected_serial.empty());

  // Every physical device class still has to pin a serial.
  CHECK(!parse("", "expected-device-class = removable-disk\n").ok());
  CHECK(parse("expected-serial = SN1\n",
              "expected-device-class = removable-disk\n")
            .ok());
}
TEST(ux_automount_eject_suspends_until_device_reinsertion) {
  FakeAutomountPlatform platform;
  const xfs::Uuid uuid = TestUuid(0xf1);
  platform.devices.push_back({TestFilesystem(uuid, "USB1", 1, "SN1"), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(uuid)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_EQ(platform.start_calls, 1);

  std::string detail;
  CHECK(engine.Eject(uuid, At(1000), &detail));
  CHECK(detail.find("safe removal") != std::string::npos);
  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.stop_calls, 1);
  CHECK_EQ(platform.CountLogs("eject", "released"), 1);

  // While Windows still reports the device, even a periodic reconciliation
  // must not silently take the disk back after the user released it.
  engine.Tick(At(61000));
  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.start_calls, 1);
  CHECK(engine.status().entries.front().detail.find("safe removal") !=
        std::string::npos);

  // Safe removal: discovery no longer sees the filesystem.  The suspension is
  // lifted, but there is still nothing attached to mount.
  platform.devices[0].present = false;
  engine.NotifyDeviceChanged(At(62000));
  engine.Tick(At(62000));
  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.CountLogs("eject", "device-removed"), 1);

  // Reinsertion starts a fresh mount cycle.
  platform.devices[0].present = true;
  engine.NotifyDeviceChanged(At(63000));
  engine.Tick(At(63000));
  CHECK_EQ(platform.sessions.size(), 1u);
  CHECK_EQ(platform.start_calls, 2);
  CHECK(engine.status().entries.front().state ==
        xfs::AutomountEntryState::kMounted);
}

TEST(ux_automount_eject_refuses_unenrolled_and_survives_a_failed_stop) {
  FakeAutomountPlatform platform;
  const xfs::Uuid enrolled = TestUuid(0xf2);
  const xfs::Uuid other = TestUuid(0xf3);
  platform.devices.push_back(
      {TestFilesystem(enrolled, "USB1", 1, "SN1"), true});

  xfs::AutomountEngine engine(platform);
  engine.SetConfig(ConfigWith({EngineEntry(enrolled)}));
  engine.Start(kStart);
  engine.Tick(kStart);
  std::string detail;
  CHECK(!engine.Eject(other, At(1000), &detail));
  CHECK(detail.find("not enrolled") != std::string::npos);

  platform.stop_failures = 1;
  CHECK(!engine.Eject(enrolled, At(2000), &detail));
  CHECK(detail.find("did not stop cleanly") != std::string::npos);
  CHECK_EQ(platform.CountLogs("eject", "failed"), 1);

  // The failed stop is retried by reconciliation.  The release intent is
  // already recorded, so the retry must not restart the mount the user asked
  // to release.
  engine.Tick(At(3000));
  CHECK(platform.sessions.empty());
  CHECK_EQ(platform.start_calls, 1);

  // A repeated request reports the release instead of touching the device
  // again, and the periodic reconciliation keeps it unmounted while the
  // device stays attached.
  CHECK(engine.Eject(enrolled, At(5000), &detail));
  CHECK(detail.find("already released") != std::string::npos);
  engine.Tick(At(66000));
  CHECK_EQ(platform.start_calls, 1);
  CHECK(engine.status().entries.front().detail.find("safe removal") !=
        std::string::npos);
}
