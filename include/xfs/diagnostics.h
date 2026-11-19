// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xfs {

struct DiagnosticDevice {
  std::string label;
  std::string uuid;
  std::string state;
  std::string device_class;
  std::string bus_type;
  std::string serial_number;
  std::string stable_id;
  std::string log_state;
  std::string writable_profile;
  std::string automount_mode = "not enrolled";
  std::string effective_mode;
  bool automount_enabled = false;
  bool discovered = false;
  bool enrolled = false;
  bool mounted = false;
  bool read_only_known = true;
  std::string drive;
  uint64_t capacity_bytes = 0;
  uint64_t device_size_bytes = 0;
  uint64_t free_bytes = 0;
  uint32_t logical_sector_bytes = 0;
  uint32_t physical_sector_bytes = 0;
  uint32_t block_size = 0;
  uint32_t inode_size = 0;
  uint32_t allocation_groups = 0;
  bool read_only = true;
  std::vector<std::string> features;
};

struct DiagnosticSnapshot {
  static constexpr uint32_t kSchemaVersion = 1;
  uint32_t schema_version = kSchemaVersion;
  std::string product_version;
  std::string winfsp_version;
  std::string service_state;
  std::string service_detail;
  std::string discovery_state;
  std::string discovery_detail;
  std::vector<std::string> failures;
  std::vector<DiagnosticDevice> devices;
};

// Redacts filesystem and device paths from human-readable error strings.
std::string RedactDiagnosticText(const std::string& text);
std::string FormatDiagnosticsText(const DiagnosticSnapshot& snapshot);
std::string FormatDiagnosticsJson(const DiagnosticSnapshot& snapshot,
                                  bool include_identifiers = false);
DiagnosticSnapshot CollectDiagnostics();

}  // namespace xfs
