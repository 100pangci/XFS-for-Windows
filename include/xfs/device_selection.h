// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - UX-1 interactive device selection.
//
// Selection and presentation live above the discovery layer: discovery only
// reports facts, this module turns them into a stable console menu and parses
// a user's answer.  The console I/O itself stays in the CLI so the rules are
// unit-testable and CI never needs an interactive terminal.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xfs/device_discovery.h"
#include "xfs/error.h"

namespace xfs {

// A drive-letter answer.  `automatic` means "pick a free letter"; otherwise
// `drive` is normalized ("E:") and the caller must not silently replace it.
struct DriveSelection {
  bool automatic = true;
  std::string drive;
};

// Human-readable byte count ("58.20 GiB"), stable across the CLI surfaces.
std::string FormatByteCount(uint64_t bytes);

// "removable", "fixed", "virtual", ...
std::string DescribeFilesystemClass(const PartitionLocation& location);

// "mounted at E: (xfswin)" / "mounted by Windows at E:\\" / empty.
std::string DescribeMountState(const DiscoveredFilesystem& filesystem);

// One menu row for a discovered, verified XFS filesystem.
std::string FormatFilesystemMenuEntry(size_t index,
                                       const DiscoveredFilesystem& filesystem);

// The complete list, one line per filesystem.  `index` is 1-based.  Rejected
// candidates are deliberately not selectable.
std::vector<std::string> FormatDeviceMenu(const DiscoveryReport& report);

// Diagnostics for candidates that looked like XFS but failed verification.
std::vector<std::string> FormatRejectedDevices(const DiscoveryReport& report);

// Accepts a 1-based menu number ("2", "#2"), a filesystem UUID, or a
// (case-insensitive) unique label.  Returns a 0-based index into
// `filesystems`.  An ambiguous label is an error, never an arbitrary pick.
Result<size_t> SelectDiscoveredFilesystem(
    std::string_view input, const std::vector<DiscoveredFilesystem>& filesystems);

// Empty input or "auto" selects automatic allocation.  Otherwise the answer
// must be a single drive letter; A: and B: are rejected because they are
// historically floppy letters and are never chosen by the allocator.
Result<DriveSelection> ParseDriveSelection(std::string_view input);

}  // namespace xfs