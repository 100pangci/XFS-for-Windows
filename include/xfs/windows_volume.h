// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - Windows volume / drive-letter enumeration.
//
// This module is a read-only, native-API view of what Windows currently knows
// about volumes and drive letters.  It exists for UX-1 device discovery and
// drive-letter allocation; it never locks, dismounts or modifies a volume.
//
// Non-Windows builds return an empty vector and an explanatory error, so the
// portable test suite can link the same declarations.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "xfs/device_policy.h"
#include "xfs/error.h"

namespace xfs {

struct VolumeExtent {
  uint32_t disk_number = 0;
  uint64_t starting_offset = 0;
  uint64_t length_bytes = 0;
};

// One volume as returned by FindFirstVolumeW / FindNextVolumeW.
struct RawVolumeInfo {
  // "\\?\Volume{...}\" exactly as enumerated.
  std::string volume_name;
  // "\\?\Volume{...}" (no trailing backslash) for CreateFileW.
  std::string device_path;
  // Drive roots and directory mount points, UTF-8 (e.g. "E:\\").
  std::vector<std::string> mount_points;
  std::string label;
  std::string filesystem;  // "NTFS", "FAT32", "RAW", ...
  uint32_t serial_number = 0;
  bool has_filesystem = false;
  // GetVolumeInformationW failed with ERROR_UNRECOGNIZED_VOLUME.  This is the
  // only condition that makes a volume eligible for drive-letter takeover: a
  // volume whose properties merely could not be read is left alone.
  bool unrecognized_filesystem = false;
  uint32_t volume_info_error = 0;
  bool readable = false;
  std::string error;
  uint32_t disk_number = 0;
  uint32_t partition_number = 0;
  std::vector<VolumeExtent> extents;
};

// Enumerates every Windows volume.  A per-volume failure is recorded in that
// entry's `error`; only an enumeration-level failure fills `error` and stops
// the walk.  Never writes.
std::vector<RawVolumeInfo> EnumerateVolumes(std::string* error);

enum class DriveLetterKind : uint8_t {
  kUnknown = 0,
  kFixed,
  kRemovable,
  kNetwork,
  kCdrom,
  kRamdisk,
  kSubst,
};

struct DriveLetterInfo {
  char letter = 0;  // 'A'..'Z'
  DriveLetterKind kind = DriveLetterKind::kUnknown;
  // QueryDosDeviceW target ("\\Device\\HarddiskVolume3", "\\??\\C:\\dir", ...).
  std::string target;
};

// Enumerates letters currently visible in this session.  `usage` marks every
// letter present in GetLogicalDriveStringsW as used, including network
// mappings and SUBST drives.  The root-string walk is authoritative for
// "occupied"; QueryDosDeviceW only classifies why.
struct DriveLetterUsage {
  std::array<bool, 26> used{};
  std::array<DriveLetterKind, 26> kinds{};
  std::array<std::string, 26> targets{};

  bool IsUsed(char letter) const;
  DriveLetterKind KindOf(char letter) const;
  std::string TargetOf(char letter) const;
};

std::vector<DriveLetterInfo> EnumerateDriveLetters(std::string* error);

// ---------------------------------------------------------------------------
// Drive-letter takeover support (read-only queries + the two mount-point
// operations).  These never change Mount Manager policy: they remove and
// restore one normal drive-letter mount point, which is exactly the temporary
// association Windows has with the RAW volume.
// ---------------------------------------------------------------------------

struct VolumePathState {
  std::string volume_name;   // "\\?\Volume{...}\"
  std::string device_path;   // "\\?\Volume{...}"
  std::vector<std::string> mount_points;
  bool has_filesystem = false;
  bool unrecognized_filesystem = false;
  uint32_t volume_info_error = 0;
  std::string filesystem;
  bool readable = false;
  uint32_t disk_number = 0;
  uint32_t partition_number = 0;
  std::vector<VolumeExtent> extents;
};

// Finds a volume by its "\\?\Volume{...}" device path.  Returns nullptr when
// the volume is not currently enumerated.
const RawVolumeInfo* FindVolumeByDevicePath(
    const std::vector<RawVolumeInfo>& volumes, std::string_view device_path);

// Re-reads one volume.  Fails with kNotFound when the volume GUID is gone.
Result<VolumePathState> QueryVolumePathState(std::string_view device_path,
                                             std::string* error);

// Removes one normal drive-letter mount point ("F:\\") from its volume.
// ERROR_PATH_NOT_FOUND is treated as "already removed"; every other failure
// is returned.  Directory mount points are never touched.
Status RemoveDriveLetterMountPoint(std::string_view mount_point,
                                   std::string* error);

// Restores "F:\\" to `volume_name` and verifies that the volume's path list
// contains it again.  A letter that was taken by another volume is reported,
// never overwritten.
Status RestoreDriveLetterMountPoint(std::string_view mount_point,
                                    std::string_view volume_name,
                                    std::string* error);

// True while the volume GUID is still enumerated (the device is attached).
bool IsVolumePresent(std::string_view device_path);

// Waits, bounded, until `drive` is no longer listed as a logical drive.
bool WaitForDriveLetterRelease(std::string_view drive, uint32_t timeout_ms);

// Reads GetVolumeInformationW for a drive root ("F:" or "F:\\").  Returns
// false when Windows has no recognized filesystem there; `filesystem` and
// `serial` are filled only on success.
bool QueryDriveVolumeInformation(std::string_view drive,
                                 std::string* filesystem, uint32_t* serial,
                                 std::string* error);

// "\\?\Volume{...}\" currently mounted at a drive root ("F:\\"), via
// GetVolumeNameForVolumeMountPointW.  This is the authoritative reverse check
// before a captured mount point may be removed or restored.
Result<std::string> QueryVolumeNameForMountPoint(std::string_view mount_point);

}  // namespace xfs