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

}  // namespace xfs