// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - UX-1 automount configuration.
//
// This is deliberately a small, versioned, human-readable file, not a
// database.  The filesystem UUID is the identity; a preferred drive letter,
// a mount mode and (for writable entries) the DEV-1 identity pins are policy
// data attached to that UUID.  Nothing here selects a device by disk number.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xfs/device_policy.h"
#include "xfs/error.h"
#include "xfs/uuid.h"

namespace xfs {

enum class AutomountMode : uint8_t {
  kReadOnly = 0,
  kReadWrite = 1,
};

const char* ToString(AutomountMode mode);
bool ParseAutomountMode(std::string_view text, AutomountMode* out);

struct AutomountEntry {
  Uuid fs_uuid{};
  bool enabled = false;
  AutomountMode mode = AutomountMode::kReadOnly;
  // "E:" or empty for automatic allocation.
  std::string preferred_drive;
  // DEV-1 identity pins.  They are optional for a read-only entry and
  // mandatory for a writable one.
  std::string expected_serial;
  std::string expected_stable_id;
  uint64_t expected_size_bytes = 0;
  // Expected physical device class.  kUnknown means "not pinned" (the
  // read-only entries written by older versions).  A writable entry pins the
  // class it was enrolled against; a different class on a later insertion
  // falls back to read-only instead of writing to an unexpected device.
  DeviceClass expected_device_class = DeviceClass::kUnknown;
  // Explicit qualification acknowledgements.  `device_qualification` mirrors
  // the CLI's --device-write-qualification.  The class flags mirror
  // --allow-removable-disk-write / --allow-fixed-disk-write; fixed physical
  // disks are refused for automount read-write by ValidateAutomountEntry.
  bool device_qualification = false;
  bool allow_removable_disk = false;
  bool allow_fixed_disk = false;
};

struct AutomountConfig {
  static constexpr uint32_t kCurrentVersion = 1;

  uint32_t version = kCurrentVersion;
  // Event coalescing window and the low-frequency reconciliation period.
  uint32_t debounce_ms = 750;
  uint32_t reconcile_seconds = 60;
  std::vector<AutomountEntry> entries;

  const AutomountEntry* Find(const Uuid& uuid) const;
  AutomountEntry* Find(const Uuid& uuid);
  // Adds or replaces the entry for its UUID.
  void Upsert(const AutomountEntry& entry);
  bool Remove(const Uuid& uuid);
};

struct AutomountConfigLoad {
  AutomountConfig config;
  std::vector<std::string> warnings;
};

// Missing file is not an error: it yields the default (empty) configuration.
Result<AutomountConfigLoad> LoadAutomountConfig(
    const std::filesystem::path& path);
Result<AutomountConfigLoad> ParseAutomountConfig(std::string_view text);

// Stable, human-readable serialization.  Entries are sorted by UUID.
std::string SerializeAutomountConfig(const AutomountConfig& config);

// Writes via a temporary file in the same directory, flushes and renames, so
// an interrupted write cannot leave a half-valid configuration.
Status SaveAutomountConfig(const std::filesystem::path& path,
                           const AutomountConfig& config);

// Entry-level policy validation.  Read-only entries never need qualification.
// Writable entries need the explicit acknowledgement, a serial, a stable
// device id and a size pin; fixed physical disks remain refused.
Status ValidateAutomountEntry(const AutomountEntry& entry);

}  // namespace xfs