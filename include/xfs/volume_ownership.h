// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - whole-disk volume ownership classification.
//
// A writable whole-disk open must lock and dismount every volume that could
// expose the same block range.  Windows reports volume paths through
// FindFirstVolumeW, including optical drives.  Optical media has no disk
// extents and is not part of any disk's block namespace, so it cannot be
// mounted from the disk being opened.  Everything else that cannot be
// classified must still refuse the writable open.
#pragma once

#include <cstdint>

namespace xfs {

// Windows GetDriveTypeW return values.  Keeping the mapping here lets the
// safety decision be unit-tested on every platform without including
// windows.h.
enum class VolumeDriveType : uint32_t {
  kUnknown = 0,   // DRIVE_UNKNOWN
  kNoRootDir = 1, // DRIVE_NO_ROOT_DIR
  kRemovable = 2, // DRIVE_REMOVABLE
  kFixed = 3,     // DRIVE_FIXED
  kRemote = 4,    // DRIVE_REMOTE
  kCdRom = 5,     // DRIVE_CDROM
  kRamDisk = 6,   // DRIVE_RAMDISK
};

// True only for optical media.  A CD/DVD volume cannot contain a filesystem
// on the disk whose ownership is being acquired, so failing to read its disk
// extents is not a reason to refuse the whole writable open.  Removable,
// fixed, remote, RAM-disk, no-root and unknown volumes remain safety-relevant
// and must still refuse when their relation cannot be established.
inline bool VolumeTypeIsProvablyNotOnLocalDisk(VolumeDriveType type) {
  return type == VolumeDriveType::kCdRom;
}

}  // namespace xfs
