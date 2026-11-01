// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - block-device identity, geometry and write policy.
#include "xfs/device_policy.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <vector>

namespace xfs {
namespace {

constexpr uint32_t kMinSectorSize = 512;
constexpr uint32_t kMaxSectorSize = 64 * 1024;

bool IsPowerOfTwo(uint32_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

std::string LowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string Trim(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

uint32_t ParseCount(std::string_view text, bool* ok) {
  std::string value = Trim(text);
  if (value.empty()) {
    *ok = false;
    return 0;
  }
  char* end = nullptr;
  unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || parsed == 0 ||
      parsed > std::numeric_limits<uint32_t>::max()) {
    *ok = false;
    return 0;
  }
  *ok = true;
  return static_cast<uint32_t>(parsed);
}

const char* ClassName(DeviceClass value) {
  switch (value) {
    case DeviceClass::kUnknown: return "unknown";
    case DeviceClass::kImageFile: return "image-file";
    case DeviceClass::kVirtualDisk: return "virtual-disk";
    case DeviceClass::kRemovableDisk: return "removable-disk";
    case DeviceClass::kFixedDisk: return "fixed-disk";
    case DeviceClass::kVolume: return "volume";
    case DeviceClass::kMemory: return "memory";
  }
  return "unknown";
}

}  // namespace

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
bool IsValidDeviceGeometry(const DeviceGeometry& geometry, std::string* reason) {
  auto fail = [&](std::string message) {
    if (reason) *reason = std::move(message);
    return false;
  };

  if (geometry.size_bytes == 0) {
    return fail("device size is zero");
  }
  if (!IsPowerOfTwo(geometry.logical_sector_size)) {
    return fail("logical sector size " +
                std::to_string(geometry.logical_sector_size) +
                " is not a power of two");
  }
  if (geometry.logical_sector_size < kMinSectorSize ||
      geometry.logical_sector_size > kMaxSectorSize) {
    return fail("logical sector size " +
                std::to_string(geometry.logical_sector_size) +
                " is outside the supported 512..65536 range");
  }
  if (!IsPowerOfTwo(geometry.physical_sector_size)) {
    return fail("physical sector size " +
                std::to_string(geometry.physical_sector_size) +
                " is not a power of two");
  }
  if (geometry.physical_sector_size < geometry.logical_sector_size) {
    return fail("physical sector size " +
                std::to_string(geometry.physical_sector_size) +
                " is smaller than the logical sector size " +
                std::to_string(geometry.logical_sector_size));
  }
  if (geometry.physical_sector_size > kMaxSectorSize) {
    return fail("physical sector size " +
                std::to_string(geometry.physical_sector_size) +
                " is outside the supported range");
  }
  if (geometry.physical_sector_size % geometry.logical_sector_size != 0) {
    return fail("physical sector size is not a multiple of the logical "
                "sector size");
  }
  if (geometry.alignment_offset >= geometry.physical_sector_size) {
    return fail("alignment offset " +
                std::to_string(geometry.alignment_offset) +
                " is outside the physical sector");
  }
  if (geometry.alignment_offset % geometry.logical_sector_size != 0) {
    return fail("alignment offset " +
                std::to_string(geometry.alignment_offset) +
                " is not a multiple of the logical sector size " +
                std::to_string(geometry.logical_sector_size));
  }
  if (geometry.size_bytes % geometry.logical_sector_size != 0) {
    return fail("device size " + std::to_string(geometry.size_bytes) +
                " is not a whole number of logical sectors");
  }
  if (reason) reason->clear();
  return true;
}

Status ValidateDeviceGeometry(const DeviceGeometry& geometry) {
  std::string reason;
  if (!IsValidDeviceGeometry(geometry, &reason)) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "unusable device geometry: " + reason)
        .Metadata("device");
  }
  return OkStatus();
}

bool ValidateDeviceRequest(uint64_t device_size, uint64_t offset, uint64_t size,
                           std::string* reason) {
  auto fail = [&](std::string message) {
    if (reason) *reason = std::move(message);
    return false;
  };
  if (size == 0) {
    if (reason) reason->clear();
    return true;
  }
  if (device_size == 0) return fail("device size is zero");
  if (offset > device_size) {
    return fail("offset " + std::to_string(offset) +
                " is beyond the device size " +
                std::to_string(device_size));
  }
  if (size > device_size - offset) {
    return fail("request [" + std::to_string(offset) + ", " +
                std::to_string(offset + size) + ") exceeds the device size " +
                std::to_string(device_size));
  }
  if (reason) reason->clear();
  return true;
}

bool IsDirectDeviceRequest(uint64_t offset, uint64_t size, const void* buffer,
                           uint32_t sector_size) {
  if (!IsPowerOfTwo(sector_size)) return false;
  if ((offset % sector_size) != 0) return false;
  if ((size % sector_size) != 0) return false;
  if (size != 0 && buffer == nullptr) return false;
  return (reinterpret_cast<uintptr_t>(buffer) % sector_size) == 0;
}

bool AlignUpDeviceChecked(uint64_t value, uint32_t alignment, uint64_t* out) {
  if (!IsPowerOfTwo(alignment) || out == nullptr) return false;
  const uint64_t mask = static_cast<uint64_t>(alignment) - 1;
  if (value > std::numeric_limits<uint64_t>::max() - mask) return false;
  *out = (value + mask) & ~mask;
  return true;
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------
DeviceClass ClassifyDevicePath(std::string_view path) {
  const std::string lower = LowerAscii(path);
  if (lower.empty()) return DeviceClass::kUnknown;

  const bool device_namespace =
      lower.rfind("\\\\", 0) == 0 || lower.rfind("//", 0) == 0;
  if (!device_namespace) return DeviceClass::kImageFile;

  if (Contains(lower, "physicaldrive")) {
    // The path only says "disk"; the opened handle refines this to virtual,
    // removable or fixed.  A path-only caller must treat it as fixed.
    return DeviceClass::kFixedDisk;
  }
  if (Contains(lower, "harddiskvolume") || Contains(lower, "volume{") ||
      lower.rfind("\\\\.\\volume", 0) == 0 ||
      lower.rfind("//./volume", 0) == 0) {
    return DeviceClass::kVolume;
  }
  if (Contains(lower, "harddisk") || Contains(lower, "cdrom") ||
      Contains(lower, "tape")) {
    return DeviceClass::kUnknown;
  }
  return DeviceClass::kUnknown;
}

// ---------------------------------------------------------------------------
// Write policy
// ---------------------------------------------------------------------------
WriteEligibility EvaluateDeviceWriteClass(
    DeviceClass device_class, const DeviceGeometry& geometry,
    bool read_only_media, bool offline, bool is_system_disk, bool has_pagefile,
    const DeviceOpenPolicy& policy, std::string* reason) {
  auto refuse = [&](WriteEligibility value, std::string message) {
    if (reason) *reason = std::move(message);
    return value;
  };

  std::string geometry_reason;
  if (!IsValidDeviceGeometry(geometry, &geometry_reason)) {
    return refuse(WriteEligibility::kRefusedUnsupportedGeometry,
                  "unusable geometry: " + geometry_reason);
  }
  // Whole-disk unbuffered I/O starts at byte 0.  A non-zero alignment offset
  // means offset 0 is not sector-aligned for this device; do not guess.
  if (device_class != DeviceClass::kImageFile &&
      device_class != DeviceClass::kMemory &&
      geometry.alignment_offset != 0) {
    return refuse(WriteEligibility::kRefusedUnsupportedGeometry,
                  "device reports a non-zero alignment offset (" +
                      std::to_string(geometry.alignment_offset) +
                      "); writable whole-disk I/O requires offset 0");
  }
  if (read_only_media && !policy.allow_read_only_media) {
    return refuse(WriteEligibility::kRefusedReadOnlyMedia,
                  "device reports read-only media");
  }
  if (offline && !policy.allow_offline) {
    return refuse(WriteEligibility::kRefusedOffline,
                  "device is offline");
  }
  if (is_system_disk && !policy.allow_system_disk) {
    return refuse(WriteEligibility::kRefusedSystemDisk,
                  "device is the Windows system disk");
  }
  if (has_pagefile && !policy.allow_pagefile_disk) {
    return refuse(WriteEligibility::kRefusedPagefileDisk,
                  "device holds an active pagefile");
  }

  switch (device_class) {
    case DeviceClass::kImageFile:
    case DeviceClass::kMemory:
      if (reason) reason->clear();
      return WriteEligibility::kEligible;
    case DeviceClass::kVirtualDisk:
      if (!policy.allow_virtual_disk) {
        return refuse(WriteEligibility::kRefusedVirtualDisk,
                      "writable virtual disks are disabled by policy");
      }
      if (reason) reason->clear();
      return WriteEligibility::kEligible;
    case DeviceClass::kRemovableDisk:
      if (!policy.allow_removable_disk) {
        return refuse(WriteEligibility::kRefusedRemovableDisk,
                      "writable removable disks require an explicit opt-in");
      }
      if (reason) reason->clear();
      return WriteEligibility::kEligible;
    case DeviceClass::kFixedDisk:
      if (!policy.allow_fixed_disk) {
        return refuse(WriteEligibility::kRefusedFixedDisk,
                      "writable fixed disks are refused by default");
      }
      if (reason) reason->clear();
      return WriteEligibility::kEligible;
    case DeviceClass::kVolume:
      if (!policy.allow_volume) {
        return refuse(WriteEligibility::kRefusedVolume,
                      "writing an individual volume is not supported");
      }
      if (reason) reason->clear();
      return WriteEligibility::kEligible;
    case DeviceClass::kUnknown:
      if (!policy.qualification_override) {
        return refuse(WriteEligibility::kRefusedUnknownClass,
                      "device class is unknown; refusing to write without an "
                      "explicit qualification override");
      }
      if (reason) reason->clear();
      return WriteEligibility::kEligible;
  }
  return refuse(WriteEligibility::kRefusedUnknownClass,
                "unrecognized device class");
}

WriteEligibility EvaluateDeviceWriteEligibility(
    DeviceClass device_class, const DeviceGeometry& geometry,
    bool read_only_media, bool offline, bool is_system_disk, bool has_pagefile,
    bool ownership_confirmed, const DeviceOpenPolicy& policy,
    std::string* reason) {
  WriteEligibility eligibility = EvaluateDeviceWriteClass(
      device_class, geometry, read_only_media, offline, is_system_disk,
      has_pagefile, policy, reason);
  if (eligibility != WriteEligibility::kEligible) return eligibility;

  const bool needs_ownership =
      device_class == DeviceClass::kVirtualDisk ||
      device_class == DeviceClass::kRemovableDisk ||
      device_class == DeviceClass::kFixedDisk;
  if (needs_ownership && policy.require_volume_ownership &&
      !ownership_confirmed) {
    if (reason) {
      *reason =
          "volume ownership was not confirmed (lock/dismount every volume on "
          "the disk)";
    }
    return WriteEligibility::kRefusedNoVolumeOwnership;
  }
  if (reason) reason->clear();
  return WriteEligibility::kEligible;
}

// ---------------------------------------------------------------------------
// Error classification
// ---------------------------------------------------------------------------
namespace {
// Windows error codes kept numeric so this module builds on Linux.
constexpr uint32_t kErrorAccessDenied = 5;
constexpr uint32_t kErrorInvalidHandle = 6;
constexpr uint32_t kErrorWriteProtect = 19;
constexpr uint32_t kErrorNotReady = 21;
constexpr uint32_t kErrorBadCommand = 22;
constexpr uint32_t kErrorCrc = 23;
constexpr uint32_t kErrorDevNotExist = 55;
constexpr uint32_t kErrorNoSuchDevice = 433;
constexpr uint32_t kErrorDeviceHardwareError = 483;
constexpr uint32_t kErrorOperationAborted = 995;
constexpr uint32_t kErrorMediaChanged = 1110;
constexpr uint32_t kErrorNoMediaInDrive = 1112;
constexpr uint32_t kErrorIoDevice = 1117;
constexpr uint32_t kErrorDeviceNotConnected = 1167;
constexpr uint32_t kErrorUnrecognizedMedia = 1780;
}  // namespace

DeviceErrorKind ClassifyDeviceError(uint32_t win32_error) {
  switch (win32_error) {
    case kErrorDeviceNotConnected:
    case kErrorNoSuchDevice:
    case kErrorDevNotExist:
    case kErrorOperationAborted:
      return DeviceErrorKind::kDeviceRemoved;
    case kErrorMediaChanged:
      return DeviceErrorKind::kMediaChanged;
    case kErrorInvalidHandle:
      return DeviceErrorKind::kInvalidHandle;
    case kErrorWriteProtect:
      return DeviceErrorKind::kWriteProtected;
    case kErrorAccessDenied:
      return DeviceErrorKind::kAccessDenied;
    case kErrorNotReady:
    case kErrorNoMediaInDrive:
      return DeviceErrorKind::kNotReady;
    case kErrorCrc:
    case kErrorDeviceHardwareError:
    case kErrorIoDevice:
    case kErrorUnrecognizedMedia:
      return DeviceErrorKind::kMediaError;
    case kErrorBadCommand:
    default:
      return DeviceErrorKind::kOther;
  }
}

bool IsTerminalDeviceError(uint32_t win32_error) {
  const DeviceErrorKind kind = ClassifyDeviceError(win32_error);
  return kind == DeviceErrorKind::kDeviceRemoved ||
         kind == DeviceErrorKind::kMediaChanged ||
         kind == DeviceErrorKind::kInvalidHandle;
}

// ---------------------------------------------------------------------------
// Cache-mode parsing
// ---------------------------------------------------------------------------
bool ParseDeviceCacheMode(std::string_view text, DeviceCacheMode* out) {
  if (out == nullptr) return false;
  const std::string lower = LowerAscii(Trim(text));
  if (lower == "buffered" || lower == "buffered-default") {
    *out = DeviceCacheMode::kBuffered;
    return true;
  }
  if (lower == "write-through" || lower == "writethrough") {
    *out = DeviceCacheMode::kWriteThrough;
    return true;
  }
  if (lower == "no-buffering" || lower == "nobuffering") {
    *out = DeviceCacheMode::kNoBuffering;
    return true;
  }
  if (lower == "no-buffering-write-through" ||
      lower == "nobufferingwritethrough" || lower == "both") {
    *out = DeviceCacheMode::kNoBufferingWriteThrough;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Fault plan
// ---------------------------------------------------------------------------
DeviceFaultPlan DeviceFaultPlan::Parse(std::string_view text) {
  DeviceFaultPlan plan;
  std::string input(text);
  size_t pos = 0;
  while (pos <= input.size()) {
    size_t comma = input.find(',', pos);
    if (comma == std::string::npos) comma = input.size();
    const std::string item = Trim(
        std::string_view(input).substr(pos, comma - pos));
    pos = comma + 1;
    if (item.empty()) continue;

    std::string name = item;
    uint32_t occurrence = 1;
    const size_t colon = item.find(':');
    if (colon != std::string::npos) {
      name = Trim(std::string_view(item).substr(0, colon));
      bool ok = false;
      occurrence = ParseCount(std::string_view(item).substr(colon + 1), &ok);
      if (!ok) continue;
    }
    const std::string lower = LowerAscii(name);
    if (lower == "read_fail") {
      plan.read_fail_at = occurrence;
    } else if (lower == "write_fail") {
      plan.write_fail_at = occurrence;
    } else if (lower == "read_short") {
      plan.read_short_at = occurrence;
    } else if (lower == "write_short") {
      plan.write_short_at = occurrence;
    } else if (lower == "flush_fail") {
      plan.flush_fail_at = occurrence;
    } else if (lower == "invalidate_after") {
      plan.invalidate_after = occurrence;
    }
  }
  return plan;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
const char* ToString(DeviceClass value) { return ClassName(value); }

const char* ToString(DeviceBusType value) {
  switch (value) {
    case DeviceBusType::kUnknown: return "unknown";
    case DeviceBusType::kScsi: return "scsi";
    case DeviceBusType::kAtapi: return "atapi";
    case DeviceBusType::kAta: return "ata";
    case DeviceBusType::k1394: return "1394";
    case DeviceBusType::kSsa: return "ssa";
    case DeviceBusType::kFibre: return "fibre";
    case DeviceBusType::kUsb: return "usb";
    case DeviceBusType::kRaid: return "raid";
    case DeviceBusType::kIscsi: return "iscsi";
    case DeviceBusType::kSas: return "sas";
    case DeviceBusType::kSata: return "sata";
    case DeviceBusType::kSd: return "sd";
    case DeviceBusType::kMmc: return "mmc";
    case DeviceBusType::kVirtual: return "virtual";
    case DeviceBusType::kFileBackedVirtual: return "file-backed-virtual";
    case DeviceBusType::kSpaces: return "spaces";
    case DeviceBusType::kNvme: return "nvme";
    case DeviceBusType::kScm: return "scm";
    case DeviceBusType::kUfs: return "ufs";
    case DeviceBusType::kOther: return "other";
  }
  return "unknown";
}

const char* ToString(DeviceCacheMode value) {
  switch (value) {
    case DeviceCacheMode::kBuffered: return "buffered";
    case DeviceCacheMode::kWriteThrough: return "write-through";
    case DeviceCacheMode::kNoBuffering: return "no-buffering";
    case DeviceCacheMode::kNoBufferingWriteThrough:
      return "no-buffering-write-through";
  }
  return "buffered";
}

const char* ToString(DeviceState value) {
  switch (value) {
    case DeviceState::kClosed: return "closed";
    case DeviceState::kOpening: return "opening";
    case DeviceState::kOpen: return "open";
    case DeviceState::kFlushing: return "flushing";
    case DeviceState::kClosing: return "closing";
    case DeviceState::kInvalid: return "invalid";
  }
  return "unknown";
}

const char* ToString(WriteEligibility value) {
  switch (value) {
    case WriteEligibility::kEligible: return "eligible";
    case WriteEligibility::kRefusedUnsupportedGeometry:
      return "refused-unsupported-geometry";
    case WriteEligibility::kRefusedUnknownClass:
      return "refused-unknown-class";
    case WriteEligibility::kRefusedVolume: return "refused-volume";
    case WriteEligibility::kRefusedFixedDisk: return "refused-fixed-disk";
    case WriteEligibility::kRefusedRemovableDisk:
      return "refused-removable-disk";
    case WriteEligibility::kRefusedVirtualDisk: return "refused-virtual-disk";
    case WriteEligibility::kRefusedSystemDisk: return "refused-system-disk";
    case WriteEligibility::kRefusedPagefileDisk:
      return "refused-pagefile-disk";
    case WriteEligibility::kRefusedOffline: return "refused-offline";
    case WriteEligibility::kRefusedReadOnlyMedia:
      return "refused-read-only-media";
    case WriteEligibility::kRefusedNoVolumeOwnership:
      return "refused-no-volume-ownership";
  }
  return "unknown";
}

const char* ToString(DeviceErrorKind value) {
  switch (value) {
    case DeviceErrorKind::kNone: return "none";
    case DeviceErrorKind::kDeviceRemoved: return "device-removed";
    case DeviceErrorKind::kMediaChanged: return "media-changed";
    case DeviceErrorKind::kMediaError: return "media-error";
    case DeviceErrorKind::kWriteProtected: return "write-protected";
    case DeviceErrorKind::kAccessDenied: return "access-denied";
    case DeviceErrorKind::kInvalidHandle: return "invalid-handle";
    case DeviceErrorKind::kNotReady: return "not-ready";
    case DeviceErrorKind::kOther: return "other";
  }
  return "unknown";
}

}  // namespace xfs
