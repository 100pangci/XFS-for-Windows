// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/drive_letter.h"

#include <cctype>

namespace xfs {

bool DriveLetterUsage::IsUsed(char letter) const {
  if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 32);
  if (letter < 'A' || letter > 'Z') return false;
  return used[static_cast<size_t>(letter - 'A')];
}

DriveLetterKind DriveLetterUsage::KindOf(char letter) const {
  if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 32);
  if (letter < 'A' || letter > 'Z') return DriveLetterKind::kUnknown;
  return kinds[static_cast<size_t>(letter - 'A')];
}

std::string DriveLetterUsage::TargetOf(char letter) const {
  if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 32);
  if (letter < 'A' || letter > 'Z') return {};
  return targets[static_cast<size_t>(letter - 'A')];
}

bool IsValidDriveLetter(std::string_view text) {
  if (text.size() != 2 || text[1] != ':') return false;
  char c = text[0];
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  return c >= 'A' && c <= 'Z';
}

Result<std::string> NormalizeDriveLetter(std::string_view text) {
  if (text.empty()) return std::string();
  std::string value(text);
  if (value.size() == 1) {
    char c = value[0];
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    if (c >= 'A' && c <= 'Z') return std::string(1, c) + ":";
    return XFS_ERR(XfsError::kInvalidArgument,
                   "invalid drive letter: " + std::string(text));
  }
  if (value.size() != 2 || value[1] != ':') {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "invalid drive letter: " + std::string(text));
  }
  char c = value[0];
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  if (c < 'A' || c > 'Z') {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "invalid drive letter: " + std::string(text));
  }
  return std::string(1, c) + ":";
}

bool IsDriveLetterRoot(std::string_view mount_point) {
  if (mount_point.size() == 2) {
    return mount_point[1] == ':' &&
           ((mount_point[0] >= 'A' && mount_point[0] <= 'Z') ||
            (mount_point[0] >= 'a' && mount_point[0] <= 'z'));
  }
  if (mount_point.size() != 3) return false;
  return mount_point[1] == ':' && mount_point[2] == '\\' &&
         ((mount_point[0] >= 'A' && mount_point[0] <= 'Z') ||
          (mount_point[0] >= 'a' && mount_point[0] <= 'z'));
}

std::string SingleDriveLetterMountPoint(
    const std::vector<std::string>& mount_points) {
  std::string found;
  for (const auto& mount_point : mount_points) {
    if (!IsDriveLetterRoot(mount_point)) continue;  // directory mount point
    if (!found.empty()) return {};                  // more than one letter
    char letter = mount_point[0];
    if (letter >= 'a' && letter <= 'z') {
      letter = static_cast<char>(letter - 32);
    }
    found = std::string(1, letter) + ":";
  }
  return found;
}

namespace {

// Takeover is deliberately limited to the same D:..Z: range the automatic
// allocator uses.  A: and B: are floppy-class letters, and C: is the system
// drive on every normal Windows installation.
bool IsTakeoverLetter(std::string_view drive) {
  return drive.size() == 2 && drive[1] == ':' && drive[0] >= 'D' &&
         drive[0] <= 'Z';
}

}  // namespace

Result<DriveLetterChoice> ChooseDriveLetter(std::string_view preferred,
                                            const DriveLetterUsage& usage) {
  return ChooseDriveLetter(preferred, usage, std::string_view());
}

Result<DriveLetterChoice> ChooseDriveLetter(
    std::string_view preferred, const DriveLetterUsage& usage,
    std::string_view existing_same_volume,
    std::string_view remembered_letter) {
  std::string requested;
  if (!preferred.empty()) {
    auto result = NormalizeDriveLetter(preferred);
    if (!result.ok()) return result.error();
    requested = result.value();
  }

  std::string existing;
  if (!existing_same_volume.empty()) {
    auto result = NormalizeDriveLetter(existing_same_volume);
    if (result.ok() && IsTakeoverLetter(result.value())) {
      existing = result.value();
    }
  }

  auto letter_is_free = [&](char letter) {
    return !usage.IsUsed(letter);
  };
  auto make = [](char letter) { return std::string(1, letter) + ":"; };

  // Priority 1: an explicitly preferred letter that is free.  When Windows
  // has also assigned a different RAW letter to this volume, that letter is
  // released too, so the user does not keep an unusable RAW drive.
  if (!requested.empty() && letter_is_free(requested[0])) {
    DriveLetterChoice choice;
    choice.drive = requested;
    choice.release_drive = existing;
    if (!existing.empty()) {
      choice.note = "the Windows volume letter " + existing +
                    " will be released before mounting " + requested;
    }
    return choice;
  }

  // Priority 2: the preferred letter is the existing same-volume RAW letter.
  if (!requested.empty() && !existing.empty() && requested == existing) {
    DriveLetterChoice choice;
    choice.drive = existing;
    choice.release_drive = existing;
    return choice;
  }

  // Priority 3: the preferred letter is occupied; keep the letter Windows
  // already assigned to this volume instead of falling to an unrelated one.
  if (!requested.empty() && !existing.empty()) {
    DriveLetterChoice choice;
    choice.drive = existing;
    choice.release_drive = existing;
    choice.preferred_unavailable = true;
    choice.note = "drive " + requested +
                  " is already in use; taking over the Windows volume letter " +
                  existing + " instead";
    return choice;
  }

  // Priority 4: no existing Windows letter for this volume.
  if (requested.empty() && !existing.empty()) {
    DriveLetterChoice choice;
    choice.drive = existing;
    choice.release_drive = existing;
    return choice;
  }

  // Reinsertion: a takeover deletes the volume's Windows mount point, and
  // Windows does not recreate it when the same device is plugged back in.
  // Reuse the letter this service used for the filesystem before removal, so
  // the device comes back at the same place and no later RAW letter can
  // appear next to an unrelated allocation.  An explicit preference and an
  // existing same-volume letter above still take priority.
  std::string remembered;
  if (!remembered_letter.empty()) {
    auto result = NormalizeDriveLetter(remembered_letter);
    if (result.ok() && IsTakeoverLetter(result.value()) &&
        letter_is_free(result.value()[0])) {
      remembered = result.value();
    }
  }
  if (!remembered.empty()) {
    DriveLetterChoice choice;
    choice.drive = remembered;
    // The remembered letter is a fallback below an explicit preference.  If
    // the preference is occupied, say so: the caller reports "mounted as X
    // instead" instead of pretending the configured letter was honored.
    choice.preferred_unavailable = !requested.empty();
    if (!requested.empty()) {
      choice.note = "drive " + requested +
                    " is already in use; reusing the previous drive letter " +
                    remembered + " instead";
    }
    return choice;
  }

  // Auto order: D:..Z:.  C: is the system drive; A:/B: are reserved for
  // removable floppy-class devices and WinFsp drive mounts are not appropriate
  // there.  The same scan serves as the fallback when a preferred letter is
  // occupied: it always starts at D:, never at the letter after the
  // preference, so a blocked E: can still fall back to a free D:.
  for (char letter = 'D'; letter <= 'Z'; ++letter) {
    if (!letter_is_free(letter)) continue;
    const std::string chosen = make(letter);
    if (requested.empty()) {
      return DriveLetterChoice{chosen, {}, {}, false, {}};
    }
    return DriveLetterChoice{
        chosen,
        {},
        {},
        true,
        "drive " + requested + " is already in use; choosing " + chosen +
            " instead"};
  }
  if (requested.empty()) {
    return XFS_ERR(XfsError::kNoSpace,
                   "no free drive letter is available")
        .Metadata("drive letter");
  }
  return XFS_ERR(XfsError::kNoSpace,
                 "no free drive letter is available (requested " + requested +
                     " is in use)")
      .Metadata("drive letter");
}

}  // namespace xfs