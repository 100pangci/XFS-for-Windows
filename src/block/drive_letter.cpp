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

Result<DriveLetterChoice> ChooseDriveLetter(std::string_view preferred,
                                            const DriveLetterUsage& usage) {
  std::string normalized;
  if (!preferred.empty()) {
    auto result = NormalizeDriveLetter(preferred);
    if (!result.ok()) return result.error();
    normalized = result.value();
  }

  auto letter_is_free = [&](char letter) {
    return !usage.IsUsed(letter);
  };
  auto make = [](char letter) { return std::string(1, letter) + ":"; };

  if (!normalized.empty()) {
    const char letter = normalized[0];
    if (letter_is_free(letter)) {
      return DriveLetterChoice{normalized, false, {}};
    }
    // Preferred letter is occupied: never evict the existing device.  Report
    // the conflict and find the next free auto letter.
    const std::string requested = normalized;
    // Start looking after the requested letter.
    for (int step = 1; step < 26; ++step) {
      const char candidate =
          static_cast<char>('A' + ((letter - 'A' + step) % 26));
      if (candidate == 'A' || candidate == 'B' || candidate == 'C') continue;
      if (letter_is_free(candidate)) {
        return DriveLetterChoice{
            make(candidate), true,
            "drive " + requested +
                " is already in use; choosing " + make(candidate) + " instead"};
      }
    }
    return XFS_ERR(XfsError::kNoSpace,
                   "no free drive letter is available (requested " +
                       requested + " is in use)")
        .Metadata("drive letter");
  }

  // Auto order: E:..Z:, then D:.  C: is the system drive; A:/B: are reserved
  // for removable floppy-class devices and WinFsp drive mounts are not
  // appropriate there.
  for (char letter = 'E'; letter <= 'Z'; ++letter) {
    if (letter_is_free(letter)) return DriveLetterChoice{make(letter), false, {}};
  }
  if (letter_is_free('D')) return DriveLetterChoice{"D:", false, {}};
  return XFS_ERR(XfsError::kNoSpace,
                 "no free drive letter is available")
      .Metadata("drive letter");
}

}  // namespace xfs