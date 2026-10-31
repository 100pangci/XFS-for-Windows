// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - block-device identity, geometry and write policy.
//
// DEV-1 separates three questions that the old backend answered with one
// `is_writable` bool:
//
//   1. What kind of device is this? (image file, VHD/VHDX virtual disk,
//      removable disk, fixed disk, volume, ...)
//   2. Is its geometry sane enough to issue sector-aligned I/O?
//   3. Under which explicit policy may this caller write to it?
//
// Everything in this header is platform independent so the same rules are
// unit-tested on Linux and applied by the Windows raw-device backend.  The
// backend still owns the actual DeviceIoControl probing; this module owns the
// decision, the arithmetic and the error classification.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "xfs/error.h"

namespace xfs {

// ---------------------------------------------------------------------------
// Device classes
// ---------------------------------------------------------------------------
enum class DeviceClass : uint8_t {
  kUnknown = 0,
  kImageFile,      // *.img / *.raw on a host filesystem
  kVirtualDisk,    // attached VHD/VHDX (BusTypeVirtual / FileBackedVirtual)
  kRemovableDisk,  // USB stick, SD card, removable media
  kFixedDisk,      // internal fixed disk
  kVolume,         // \\.\HarddiskVolumeX / \\.\Volume{...}
  kMemory,         // test-only MemoryBlockDevice
};

enum class DeviceBusType : uint8_t {
  kUnknown = 0,
  kScsi,
  kAtapi,
  kAta,
  k1394,
  kSsa,
  kFibre,
  kUsb,
  kRaid,
  kIscsi,
  kSas,
  kSata,
  kSd,
  kMmc,
  kVirtual,
  kFileBackedVirtual,
  kSpaces,
  kNvme,
  kScm,
  kUfs,
  kOther,
};

// How the Windows handle is opened.  The default for raw devices is
// kNoBuffering: the backend, not the host cache, must see every sector-aligned
// request, and durability is established by an explicit FlushFileBuffers
// barrier.  kBuffered / kWriteThrough exist for the qualification matrix and
// for image files.
enum class DeviceCacheMode : uint8_t {
  kBuffered = 0,
  kWriteThrough,
  kNoBuffering,
  kNoBufferingWriteThrough,
};

// Explicit lifecycle.  A handle is not "open" merely because it is not
// INVALID_HANDLE_VALUE: Close drains in-flight I/O and a removed device is a
// terminal state that never reconnects by name.
enum class DeviceState : uint8_t {
  kClosed = 0,
  kOpening,
  kOpen,
  kFlushing,
  kClosing,
  kInvalid,  // removed, media changed, or a fatal handle error
};

// Why a writable open was allowed or refused.
enum class WriteEligibility : uint8_t {
  kEligible = 0,
  kRefusedUnsupportedGeometry,
  kRefusedUnknownClass,
  kRefusedVolume,
  kRefusedFixedDisk,
  kRefusedRemovableDisk,
  kRefusedVirtualDisk,
  kRefusedSystemDisk,
  kRefusedPagefileDisk,
  kRefusedOffline,
  kRefusedReadOnlyMedia,
  kRefusedNoVolumeOwnership,
};

// ---------------------------------------------------------------------------
// Probed device description
// ---------------------------------------------------------------------------
struct DeviceGeometry {
  uint64_t size_bytes = 0;
  uint32_t logical_sector_size = 0;
  uint32_t physical_sector_size = 0;
  // Offset of the first logical-sector-aligned byte from the start of the
  // device.  Whole-disk handles report 0; volume handles can report a
  // non-zero value.  Writable whole-disk qualification requires 0.
  uint32_t alignment_offset = 0;
};

struct DeviceIdentity {
  std::string path;            // path used to open the handle
  std::string stable_id;       // DeviceGuid, else serial/model/size fallback
  std::string serial_number;
  std::string model;
  uint32_t disk_number = 0;
  uint32_t partition_number = 0;
};

// The explicit write policy.  The defaults are deliberately conservative:
// image files are writable, attached VHD/VHDX disks are writable once volume
// ownership is confirmed, removable disks need an explicit opt-in, and fixed
// disks are refused unless a qualification run opts in.
struct DeviceOpenPolicy {
  bool writable = false;
  bool exclusive = false;
  // Raw writable opens must lock and dismount every volume on the disk.
  bool require_volume_ownership = true;

  bool allow_virtual_disk = true;
  bool allow_removable_disk = false;
  bool allow_fixed_disk = false;
  bool allow_volume = false;
  bool allow_system_disk = false;
  bool allow_pagefile_disk = false;
  bool allow_offline = false;
  bool allow_read_only_media = false;

  // Qualification escape hatch for device classes the policy does not know
  // yet (e.g. a new virtual bus type).  It never overrides system, pagefile,
  // volume, offline or read-only-media refusals.
  bool qualification_override = false;

  DeviceCacheMode cache_mode = DeviceCacheMode::kNoBuffering;

  // Conservative policy for an ordinary image-file read-write mount.
  static DeviceOpenPolicy ImageFileWritable() {
    DeviceOpenPolicy policy;
    policy.writable = true;
    policy.exclusive = true;
    policy.cache_mode = DeviceCacheMode::kBuffered;
    return policy;
  }

  // The only policy under which a real disk may be mounted read-write during
  // DEV-1 qualification.  Fixed disks remain refused.
  static DeviceOpenPolicy VirtualDiskQualification() {
    DeviceOpenPolicy policy;
    policy.writable = true;
    policy.exclusive = true;
    policy.allow_virtual_disk = true;
    policy.allow_removable_disk = false;
    policy.allow_fixed_disk = false;
    policy.require_volume_ownership = true;
    policy.cache_mode = DeviceCacheMode::kNoBuffering;
    return policy;
  }
};

// ---------------------------------------------------------------------------
// Geometry and request validation
// ---------------------------------------------------------------------------
// A geometry is usable only if every field is a sane power of two and the
// device size is a whole number of logical sectors.  Backends must fail the
// open (for writes) instead of silently assuming 512 bytes.
bool IsValidDeviceGeometry(const DeviceGeometry& geometry,
                           std::string* reason);
Status ValidateDeviceGeometry(const DeviceGeometry& geometry);

// Bounds check with overflow-safe arithmetic.  Returns false (and a reason)
// for a zero-sized device, an offset/size outside the device, or an overflow.
bool ValidateDeviceRequest(uint64_t device_size, uint64_t offset, uint64_t size,
                           std::string* reason);

// True when offset, size and buffer address already satisfy the unbuffered
// I/O alignment rule for `sector_size`.  `sector_size` must be a power of two.
bool IsDirectDeviceRequest(uint64_t offset, uint64_t size, const void* buffer,
                           uint32_t sector_size);

inline uint64_t AlignDownDevice(uint64_t value, uint32_t alignment) {
  return value & ~static_cast<uint64_t>(alignment - 1);
}

// Rounds up without wrapping.  Returns false when the result would overflow.
bool AlignUpDeviceChecked(uint64_t value, uint32_t alignment, uint64_t* out);

// ---------------------------------------------------------------------------
// Classification and policy
// ---------------------------------------------------------------------------
// A cheap path-string hint.  The authoritative classification always comes
// from the opened handle (STORAGE_DEVICE_DESCRIPTOR bus type, removable flag
// and device type), because a path is not a device identity.
DeviceClass ClassifyDevicePath(std::string_view path);

// Class-level eligibility: geometry, device class, offline/read-only media,
// system/pagefile flags.  Does not consider volume ownership.
WriteEligibility EvaluateDeviceWriteClass(
    DeviceClass device_class, const DeviceGeometry& geometry,
    bool read_only_media, bool offline, bool is_system_disk, bool has_pagefile,
    const DeviceOpenPolicy& policy, std::string* reason);

// Full eligibility: class checks plus the volume-ownership requirement.
WriteEligibility EvaluateDeviceWriteEligibility(
    DeviceClass device_class, const DeviceGeometry& geometry,
    bool read_only_media, bool offline, bool is_system_disk, bool has_pagefile,
    bool ownership_confirmed, const DeviceOpenPolicy& policy,
    std::string* reason);

// ---------------------------------------------------------------------------
// Windows error classification
// ---------------------------------------------------------------------------
enum class DeviceErrorKind : uint8_t {
  kNone = 0,
  kDeviceRemoved,  // terminal: the device object is gone
  kMediaChanged,   // terminal for the current handle
  kMediaError,     // the operation failed; the device may still be usable
  kWriteProtected,
  kAccessDenied,
  kInvalidHandle,
  kNotReady,
  kOther,
};

// `win32_error` is the DWORD returned by GetLastError(), kept as uint32_t so
// this header stays portable.
DeviceErrorKind ClassifyDeviceError(uint32_t win32_error);

// True for errors after which the current handle must never be used again.
bool IsTerminalDeviceError(uint32_t win32_error);

// ---------------------------------------------------------------------------
// Cache-mode parsing (qualification matrix and XFSWIN_DEVICE_CACHE_MODE)
// ---------------------------------------------------------------------------
bool ParseDeviceCacheMode(std::string_view text, DeviceCacheMode* out);

// ---------------------------------------------------------------------------
// Text helpers for diagnostics and JSON output
// ---------------------------------------------------------------------------
const char* ToString(DeviceClass value);
const char* ToString(DeviceBusType value);
const char* ToString(DeviceCacheMode value);
const char* ToString(DeviceState value);
const char* ToString(WriteEligibility value);
const char* ToString(DeviceErrorKind value);

// ---------------------------------------------------------------------------
// Test-only fault plan (XFSWIN_DEVICE_FAULT)
//
// The syntax is a comma-separated list of `name[:occurrence]`:
//   read_fail:3       fail the 3rd logical Read
//   write_fail:2      fail the 2nd logical Write
//   read_short:2      make the 2nd Read return one short transfer first
//   write_short:2     make the 2nd Write return one short transfer first
//   flush_fail:1      fail the 1st Flush
//   invalidate_after:5  mark the handle invalid after the 5th operation
//
// Counters are per opened handle.  The plan is only honoured when the
// environment variable is set; it never changes the default durability
// contract.
struct DeviceFaultPlan {
  uint32_t read_fail_at = 0;
  uint32_t write_fail_at = 0;
  uint32_t read_short_at = 0;
  uint32_t write_short_at = 0;
  uint32_t flush_fail_at = 0;
  uint32_t invalidate_after = 0;

  bool enabled() const {
    return read_fail_at != 0 || write_fail_at != 0 || read_short_at != 0 ||
           write_short_at != 0 || flush_fail_at != 0 ||
           invalidate_after != 0;
  }

  static DeviceFaultPlan Parse(std::string_view text);
};

}  // namespace xfs
