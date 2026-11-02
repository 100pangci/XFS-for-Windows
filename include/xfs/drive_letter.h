// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - drive-letter selection.
//
// The allocator is deliberately platform independent: the Windows layer fills
// DriveLetterUsage from GetLogicalDriveStringsW/QueryDosDeviceW, and the same
// selection rules are unit-tested on Linux.  A letter is "used" if Windows
// lists it at all, which covers network mappings, SUBST drives, mounted
// volumes, and a drive mounted by xfswin.  The allocator never evicts an
// existing mapping.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xfs/error.h"
#include "xfs/windows_volume.h"

namespace xfs {

struct DriveLetterChoice {
  std::string drive;          // "E:"
  // A drive-letter mount point that currently belongs to the same underlying
  // volume and must be detached before the WinFsp mount ("F:"), or empty.
  // When it equals `drive`, this is a same-letter takeover; when it differs,
  // the Windows letter is released because the filesystem is mounted
  // elsewhere.  Directory mount points are never released.
  std::string release_drive;
  bool preferred_unavailable = false;  // requested letter was occupied
  std::string note;           // human-readable fallback explanation
};

// Accepts "E:", "e:", "E".  Returns the normalized "E:".
Result<std::string> NormalizeDriveLetter(std::string_view text);

// True for a normalized single letter in A..Z.
bool IsValidDriveLetter(std::string_view text);

// True for "F:" or "F:\\": a normal drive-root mount point.
bool IsDriveLetterRoot(std::string_view mount_point);

// Returns the only normal drive-letter mount point in the list as "F:".  An
// empty string means there is none, or there is more than one drive letter
// on the volume (a configuration takeover must not guess).
std::string SingleDriveLetterMountPoint(
    const std::vector<std::string>& mount_points);

// Chooses a free drive letter.  Auto order is the first free letter from D:
// through Z:; A:, B: and C: are never chosen automatically.  An explicit
// `preferred` letter is honored when free; if occupied the allocator falls
// back to the same D:..Z: order (starting at D:, not after the preference)
// and reports it in `note` so the caller can tell the user.
Result<DriveLetterChoice> ChooseDriveLetter(std::string_view preferred,
                                            const DriveLetterUsage& usage);

// Drive-letter selection for an automount candidate whose underlying Windows
// volume may already have a RAW drive letter.  Priority:
//   1. the explicit preferred letter when it is free (the existing same-volume
//      letter is then still released, so no unusable RAW letter remains);
//   2. the preferred letter when it is exactly the existing same-volume letter
//      (a same-letter takeover);
//   3. the existing same-volume letter when the preferred one is occupied;
//   4. the first free letter from D: through Z:.
// A:, B: and C: are never taken over.  The caller must have verified that
// `existing_same_volume` really belongs to the same volume.
Result<DriveLetterChoice> ChooseDriveLetter(
    std::string_view preferred, const DriveLetterUsage& usage,
    std::string_view existing_same_volume);

}  // namespace xfs