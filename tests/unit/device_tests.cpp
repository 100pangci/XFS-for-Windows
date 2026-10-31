// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// DEV-1: portable device policy, geometry, lifecycle and 64-bit I/O tests.
//
// These tests run on every platform.  They exercise the decision layer that
// the Windows raw-device backend uses, plus the image backend's lifecycle and
// >2 TiB offsets.  Real PhysicalDrive/VHD behavior is qualified separately on
// Windows (see tests/scripts/verify_device_windows.ps1).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "test_util.h"
#include "xfs/device_policy.h"
#include "xfs/image_device.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

uint32_t ProcessId() {
#ifdef _WIN32
  return static_cast<uint32_t>(_getpid());
#else
  return static_cast<uint32_t>(::getpid());
#endif
}

struct TempImage {
  std::filesystem::path path;
  TempImage() {
    path = std::filesystem::temp_directory_path() /
           ("xfswin-device-" + std::to_string(ProcessId()) + ".img");
  }
  ~TempImage() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

xfs::DeviceGeometry Geometry(uint64_t size, uint32_t logical,
                             uint32_t physical, uint32_t offset = 0) {
  xfs::DeviceGeometry geometry;
  geometry.size_bytes = size;
  geometry.logical_sector_size = logical;
  geometry.physical_sector_size = physical;
  geometry.alignment_offset = offset;
  return geometry;
}

xfs::DeviceOpenPolicy VirtualPolicy() {
  return xfs::DeviceOpenPolicy::VirtualDiskQualification();
}

}  // namespace

TEST(device_geometry_accepts_real_sector_combinations) {
  std::string reason;
  CHECK(xfs::IsValidDeviceGeometry(Geometry(1 << 20, 512, 512), &reason));
  CHECK(xfs::IsValidDeviceGeometry(Geometry(1 << 20, 512, 4096), &reason));
  CHECK(xfs::IsValidDeviceGeometry(Geometry(1 << 20, 4096, 4096), &reason));
  CHECK(xfs::IsValidDeviceGeometry(Geometry(1 << 20, 4096, 8192), &reason));
}

TEST(device_geometry_rejects_nonsense) {
  std::string reason;
  CHECK(!xfs::IsValidDeviceGeometry(Geometry(0, 512, 512), &reason));
  CHECK(!reason.empty());
  CHECK(!xfs::IsValidDeviceGeometry(Geometry(1 << 20, 0, 512), &reason));
  CHECK(!xfs::IsValidDeviceGeometry(Geometry(1 << 20, 1000, 1000), &reason));
  CHECK(!xfs::IsValidDeviceGeometry(Geometry(1 << 20, 4096, 512), &reason));
  CHECK(!xfs::IsValidDeviceGeometry(Geometry(1 << 20, 512, 3000), &reason));
  CHECK(!xfs::IsValidDeviceGeometry(Geometry((1 << 20) + 1, 512, 512),
                                    &reason));
  CHECK(!xfs::IsValidDeviceGeometry(Geometry(1 << 20, 512, 4096, 4096),
                                    &reason));
  CHECK(!xfs::IsValidDeviceGeometry(Geometry(1 << 20, 512, 512, 512),
                                    &reason));
  CHECK(!xfs::IsValidDeviceGeometry(Geometry(1 << 20, 1024, 4096, 512),
                                    &reason));
}

TEST(device_request_validation_is_overflow_safe) {
  std::string reason;
  CHECK(xfs::ValidateDeviceRequest(1 << 20, 0, 0, &reason));
  CHECK(xfs::ValidateDeviceRequest(1 << 20, 1 << 20, 0, &reason));
  CHECK(xfs::ValidateDeviceRequest(1 << 20, 0, 1 << 20, &reason));
  CHECK(!xfs::ValidateDeviceRequest(1 << 20, 1, 1 << 20, &reason));
  CHECK(!xfs::ValidateDeviceRequest(1 << 20, 1 << 20, 1, &reason));
  CHECK(!xfs::ValidateDeviceRequest(1 << 20, ~0ull, 2, &reason));
  CHECK(!xfs::ValidateDeviceRequest(0, 0, 1, &reason));
}

TEST(device_alignment_math_checks_overflow) {
  CHECK_EQ(xfs::AlignDownDevice(4097, 4096), 4096u);
  CHECK_EQ(xfs::AlignDownDevice(4096, 4096), 4096u);
  uint64_t out = 0;
  CHECK(xfs::AlignUpDeviceChecked(4096, 4096, &out));
  CHECK_EQ(out, 4096u);
  CHECK(xfs::AlignUpDeviceChecked(4097, 4096, &out));
  CHECK_EQ(out, 8192u);
  CHECK(!xfs::AlignUpDeviceChecked(~0ull - 10, 4096, &out));
}

TEST(device_direct_request_requires_aligned_buffer) {
  alignas(4096) uint8_t aligned[4096];
  CHECK(xfs::IsDirectDeviceRequest(4096, 4096, aligned, 4096));
  CHECK(!xfs::IsDirectDeviceRequest(1, 4096, aligned, 4096));
  CHECK(!xfs::IsDirectDeviceRequest(4096, 100, aligned, 4096));
  CHECK(!xfs::IsDirectDeviceRequest(4096, 4096, aligned + 1, 4096));
  CHECK(xfs::IsDirectDeviceRequest(512, 512, aligned, 512));
}

TEST(device_write_policy_matrix) {
  std::string reason;
  const auto geometry = Geometry(1 << 20, 512, 4096);
  xfs::DeviceOpenPolicy image = xfs::DeviceOpenPolicy::ImageFileWritable();

  CHECK_EQ(xfs::EvaluateDeviceWriteClass(
               xfs::DeviceClass::kImageFile, geometry, false, false, false,
               false, image, &reason),
           xfs::WriteEligibility::kEligible);

  xfs::DeviceOpenPolicy virtual_policy = VirtualPolicy();
  CHECK_EQ(xfs::EvaluateDeviceWriteClass(
               xfs::DeviceClass::kVirtualDisk, geometry, false, false, false,
               false, virtual_policy, &reason),
           xfs::WriteEligibility::kEligible);
  CHECK_EQ(xfs::EvaluateDeviceWriteEligibility(
               xfs::DeviceClass::kVirtualDisk, geometry, false, false, false,
               false, /*ownership_confirmed=*/false, virtual_policy, &reason),
           xfs::WriteEligibility::kRefusedNoVolumeOwnership);
  CHECK_EQ(xfs::EvaluateDeviceWriteEligibility(
               xfs::DeviceClass::kVirtualDisk, geometry, false, false, false,
               false, /*ownership_confirmed=*/true, virtual_policy, &reason),
           xfs::WriteEligibility::kEligible);

  xfs::DeviceOpenPolicy no_virtual = VirtualPolicy();
  no_virtual.allow_virtual_disk = false;
  CHECK_EQ(xfs::EvaluateDeviceWriteClass(
               xfs::DeviceClass::kVirtualDisk, geometry, false, false, false,
               false, no_virtual, &reason),
           xfs::WriteEligibility::kRefusedVirtualDisk);

  // Removable: refused by default, allowed with the explicit opt-in.
  CHECK_EQ(xfs::EvaluateDeviceWriteClass(
               xfs::DeviceClass::kRemovableDisk, geometry, false, false, false,
               false, virtual_policy, &reason),
           xfs::WriteEligibility::kRefusedRemovableDisk);
  xfs::DeviceOpenPolicy removable = VirtualPolicy();
  removable.allow_removable_disk = true;
  CHECK_EQ(xfs::EvaluateDeviceWriteEligibility(
               xfs::DeviceClass::kRemovableDisk, geometry, false, false, false,
               false, true, removable, &reason),
           xfs::WriteEligibility::kEligible);

  // Fixed: refused by default, allowed only with the qualification opt-in.
  CHECK_EQ(xfs::EvaluateDeviceWriteClass(
               xfs::DeviceClass::kFixedDisk, geometry, false, false, false,
               false, virtual_policy, &reason),
           xfs::WriteEligibility::kRefusedFixedDisk);
  xfs::DeviceOpenPolicy fixed = VirtualPolicy();
  fixed.allow_fixed_disk = true;
  CHECK_EQ(xfs::EvaluateDeviceWriteEligibility(
               xfs::DeviceClass::kFixedDisk, geometry, false, false, false,
               false, true, fixed, &reason),
           xfs::WriteEligibility::kEligible);

  // System/pagefile/offline/read-only media stay refused even when the class
  // is otherwise allowed.
  CHECK_EQ(xfs::EvaluateDeviceWriteClass(
               xfs::DeviceClass::kFixedDisk, geometry, false, false,
               /*is_system_disk=*/true, false, fixed, &reason),
           xfs::WriteEligibility::kRefusedSystemDisk);
  CHECK_EQ(xfs::EvaluateDeviceWriteClass(
               xfs::DeviceClass::kFixedDisk, geometry, false, false, false,
               /*has_pagefile=*/true, fixed, &reason),
           xfs::WriteEligibility::kRefusedPagefileDisk);
  CHECK_EQ(xfs::EvaluateDeviceWriteClass(
               xfs::DeviceClass::kVirtualDisk, geometry, false,
               /*offline=*/true, false, false, virtual_policy, &reason),
           xfs::WriteEligibility::kRefusedOffline);
  CHECK_EQ(xfs::EvaluateDeviceWriteClass(
               xfs::DeviceClass::kVirtualDisk, geometry,
               /*read_only_media=*/true, false, false, false, virtual_policy,
               &reason),
           xfs::WriteEligibility::kRefusedReadOnlyMedia);

  // Volumes are never whole-disk targets.
  CHECK_EQ(xfs::EvaluateDeviceWriteClass(
               xfs::DeviceClass::kVolume, geometry, false, false, false, false,
               virtual_policy, &reason),
           xfs::WriteEligibility::kRefusedVolume);

  // Unknown classes need the explicit qualification override.
  CHECK_EQ(xfs::EvaluateDeviceWriteClass(
               xfs::DeviceClass::kUnknown, geometry, false, false, false, false,
               virtual_policy, &reason),
           xfs::WriteEligibility::kRefusedUnknownClass);
  xfs::DeviceOpenPolicy override_policy = VirtualPolicy();
  override_policy.qualification_override = true;
  CHECK_EQ(xfs::EvaluateDeviceWriteEligibility(
               xfs::DeviceClass::kUnknown, geometry, false, false, false, false,
               true, override_policy, &reason),
           xfs::WriteEligibility::kEligible);

  // Bad geometry wins over everything else.
  CHECK_EQ(xfs::EvaluateDeviceWriteClass(
               xfs::DeviceClass::kVirtualDisk, Geometry(1 << 20, 1000, 1000),
               false, false, false, false, virtual_policy, &reason),
           xfs::WriteEligibility::kRefusedUnsupportedGeometry);
}

TEST(device_error_classification) {
  CHECK_EQ(xfs::ClassifyDeviceError(1167),
           xfs::DeviceErrorKind::kDeviceRemoved);
  CHECK_EQ(xfs::ClassifyDeviceError(433),
           xfs::DeviceErrorKind::kDeviceRemoved);
  CHECK_EQ(xfs::ClassifyDeviceError(55),
           xfs::DeviceErrorKind::kDeviceRemoved);
  CHECK_EQ(xfs::ClassifyDeviceError(995),
           xfs::DeviceErrorKind::kDeviceRemoved);
  CHECK(xfs::IsTerminalDeviceError(1167));
  CHECK(xfs::IsTerminalDeviceError(433));
  CHECK(xfs::IsTerminalDeviceError(55));
  CHECK(xfs::IsTerminalDeviceError(995));
  CHECK_EQ(xfs::ClassifyDeviceError(1110),
           xfs::DeviceErrorKind::kMediaChanged);
  CHECK(xfs::IsTerminalDeviceError(1110));
  CHECK_EQ(xfs::ClassifyDeviceError(1112),
           xfs::DeviceErrorKind::kNotReady);
  CHECK(!xfs::IsTerminalDeviceError(1112));
  CHECK_EQ(xfs::ClassifyDeviceError(19),
           xfs::DeviceErrorKind::kWriteProtected);
  CHECK(!xfs::IsTerminalDeviceError(19));
  CHECK_EQ(xfs::ClassifyDeviceError(5),
           xfs::DeviceErrorKind::kAccessDenied);
  CHECK(!xfs::IsTerminalDeviceError(5));
  CHECK_EQ(xfs::ClassifyDeviceError(6),
           xfs::DeviceErrorKind::kInvalidHandle);
  CHECK(xfs::IsTerminalDeviceError(6));
  CHECK_EQ(xfs::ClassifyDeviceError(21), xfs::DeviceErrorKind::kNotReady);
  CHECK_EQ(xfs::ClassifyDeviceError(1117),
           xfs::DeviceErrorKind::kMediaError);
  CHECK_EQ(xfs::ClassifyDeviceError(12345), xfs::DeviceErrorKind::kOther);
}

TEST(device_cache_mode_parsing) {
  xfs::DeviceCacheMode mode = xfs::DeviceCacheMode::kBuffered;
  CHECK(xfs::ParseDeviceCacheMode("buffered", &mode));
  CHECK_EQ(mode, xfs::DeviceCacheMode::kBuffered);
  CHECK(xfs::ParseDeviceCacheMode("write-through", &mode));
  CHECK_EQ(mode, xfs::DeviceCacheMode::kWriteThrough);
  CHECK(xfs::ParseDeviceCacheMode("no-buffering", &mode));
  CHECK_EQ(mode, xfs::DeviceCacheMode::kNoBuffering);
  CHECK(xfs::ParseDeviceCacheMode("no-buffering-write-through", &mode));
  CHECK_EQ(mode, xfs::DeviceCacheMode::kNoBufferingWriteThrough);
  CHECK(xfs::ParseDeviceCacheMode("both", &mode));
  CHECK_EQ(mode, xfs::DeviceCacheMode::kNoBufferingWriteThrough);
  CHECK(!xfs::ParseDeviceCacheMode("unsafe", &mode));
  CHECK(!xfs::ParseDeviceCacheMode("", &mode));
}

TEST(device_fault_plan_parsing) {
  auto plan = xfs::DeviceFaultPlan::Parse(
      "read_fail:3,write_fail:2,read_short:1,write_short:4,flush_fail:1,"
      "invalidate_after:7");
  CHECK(plan.enabled());
  CHECK_EQ(plan.read_fail_at, 3u);
  CHECK_EQ(plan.write_fail_at, 2u);
  CHECK_EQ(plan.read_short_at, 1u);
  CHECK_EQ(plan.write_short_at, 4u);
  CHECK_EQ(plan.flush_fail_at, 1u);
  CHECK_EQ(plan.invalidate_after, 7u);
  auto defaults = xfs::DeviceFaultPlan::Parse("read_fail");
  CHECK_EQ(defaults.read_fail_at, 1u);
  auto junk = xfs::DeviceFaultPlan::Parse("nonsense:2,read_fail:0,");
  CHECK(!junk.enabled());
}

TEST(image_device_lifecycle_and_io) {
  TempImage temp;
  std::string error;
  auto dev = xfs::ImageBlockDevice::Create(temp.path, 1 << 20, 4096, &error);
  CHECK_MESSAGE(dev != nullptr, error);
  CHECK_EQ(dev->Info().device_class, xfs::DeviceClass::kImageFile);
  CHECK_EQ(dev->Info().cache_mode, xfs::DeviceCacheMode::kBuffered);
  CHECK_EQ(dev->State(), xfs::DeviceState::kOpen);
  CHECK_EQ(dev->SectorSize(), 4096u);

  std::vector<uint8_t> pattern(4096);
  for (size_t i = 0; i < pattern.size(); ++i) {
    pattern[i] = static_cast<uint8_t>(i * 7 + 3);
  }
  CHECK(dev->Write(4096, pattern.data(), pattern.size()));
  CHECK(dev->Flush());
  std::vector<uint8_t> readback(4096, 0);
  CHECK(dev->Read(4096, readback.data(), readback.size()));
  CHECK(std::memcmp(pattern.data(), readback.data(), pattern.size()) == 0);

  // Unaligned request on a buffered image.
  const uint8_t marker[3] = {1, 2, 3};
  uint8_t marker_read[3] = {0, 0, 0};
  CHECK(dev->Write(123, marker, sizeof(marker)));
  CHECK(dev->Read(123, marker_read, sizeof(marker_read)));
  CHECK(std::memcmp(marker, marker_read, sizeof(marker)) == 0);

  dev->Close();
  CHECK_EQ(dev->State(), xfs::DeviceState::kClosed);
  CHECK(!dev->Write(0, pattern.data(), pattern.size()));
  dev->Close();  // repeated Close is safe
  CHECK_EQ(dev->State(), xfs::DeviceState::kClosed);
}

TEST(image_device_64_bit_offsets) {
  TempImage temp;
  std::string error;
  constexpr uint64_t kSize = (3ull << 40) + (1ull << 20);  // 3 TiB + 1 MiB
  auto dev = xfs::ImageBlockDevice::Create(temp.path, kSize, 4096, &error);
  CHECK_MESSAGE(dev != nullptr, error);
  CHECK_EQ(dev->Size(), kSize);

  const uint64_t offset = (3ull << 40);  // beyond 2 TiB and beyond 32 bits
  std::vector<uint8_t> pattern(4096);
  for (size_t i = 0; i < pattern.size(); ++i) {
    pattern[i] = static_cast<uint8_t>((i * 13 + 0x5C) & 0xFF);
  }
  CHECK(dev->Write(offset, pattern.data(), pattern.size()));
  CHECK(dev->Flush());
  std::vector<uint8_t> readback(4096, 0);
  CHECK(dev->Read(offset, readback.data(), readback.size()));
  CHECK(std::memcmp(pattern.data(), readback.data(), pattern.size()) == 0);

  // A request straddling the 2 TiB boundary must not truncate to 32 bits.
  const uint64_t straddle = (2ull << 40) - 2048;
  CHECK(dev->Write(straddle, pattern.data(), pattern.size()));
  CHECK(dev->Read(straddle, readback.data(), readback.size()));
  CHECK(std::memcmp(pattern.data(), readback.data(), pattern.size()) == 0);
  dev->Close();
}
