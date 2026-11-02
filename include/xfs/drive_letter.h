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
  bool preferred_unavailable = false;  // requested letter was occupied
  std::string note;           // human-readable fallback explanation
};

// Accepts "E:", "e:", "E".  Returns the normalized "E:".
Result<std::string> NormalizeDriveLetter(std::string_view text);

// True for a normalized single letter in A..Z.
bool IsValidDriveLetter(std::string_view text);

// Chooses a free drive letter.  Auto order is the first free letter from D:
// through Z:; A:, B: and C: are never chosen automatically.  An explicit
// `preferred` letter is honored when free; if occupied the allocator falls
// back to the same D:..Z: order (starting at D:, not after the preference)
// and reports it in `note` so the caller can tell the user.
Result<DriveLetterChoice> ChooseDriveLetter(std::string_view preferred,
                                            const DriveLetterUsage& usage);

}  // namespace xfs