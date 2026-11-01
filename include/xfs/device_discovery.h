// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - UX-1 device discovery.
//
// Discovery is read-only.  It enumerates physical disks and their partitions,
// reads the XFS superblock through the existing parser/verifier, asks the
// existing log scanner for the journal state and asks the existing writable
// policy whether the feature profile is compatible.  It never mounts, writes,
// repairs or guesses from the first four magic bytes alone.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xfs/device_policy.h"
#include "xfs/error.h"
#include "xfs/log_checker.h"
#include "xfs/superblock.h"

namespace xfs {

enum class XfsProbeState : uint8_t {
  kNoXfs = 0,     // no XFS superblock magic
  kValid,         // parser/verifier + mount policy accepted the filesystem
  kCorrupt,       // XFS magic present, superblock/geometry invalid
  kUnsupported,   // XFS magic present, feature profile refused by this build
  kUnreadable,    // the device could not be read (not-ready/removed/etc.)
};

const char* ToString(XfsProbeState state);

struct XfsProbeResult {
  XfsProbeState state = XfsProbeState::kNoXfs;
  bool magic_seen = false;
  // Parser/verifier error for a non-valid probe; kOk when valid.
  XfsError error_code = XfsError::kOk;
  std::string error;
  // Diagnostic only; the caller must use state, not these fields, after a
  // failed probe.
  XfsSuperblock superblock;
  XfsGeometry geometry;
  std::string label;
  uint64_t filesystem_bytes = 0;
  uint64_t free_bytes = 0;
  LogState log_state = LogState::kUnknown;
  std::string log_detail;
  bool writable_profile = false;
  // Writable profile summary when compatible, refusal reason otherwise.
  std::string writable_profile_detail;
  // Backend failure classification for kUnreadable.
  DeviceErrorKind device_error_kind = DeviceErrorKind::kNone;
};

struct PartitionLocation {
  uint32_t disk_number = 0;
  uint32_t partition_number = 0;  // 0 = the whole disk
  bool whole_disk = false;
  std::string disk_path;  // \\.\PhysicalDriveN
  uint64_t offset_bytes = 0;
  uint64_t length_bytes = 0;
  // Whole-disk capacity, independent of the partition length.  DEV-1 identity
  // pins and the raw-device open policy are expressed against this value.
  uint64_t disk_size_bytes = 0;
  bool removable = false;
  DeviceClass device_class = DeviceClass::kUnknown;
  DeviceBusType bus_type = DeviceBusType::kUnknown;
  uint32_t logical_sector_size = 0;
  uint32_t physical_sector_size = 0;
  std::string model;
  std::string serial_number;
  std::string stable_id;
  bool system_disk = false;
  bool pagefile_disk = false;
  bool offline = false;
  bool read_only = false;
};

struct DiscoveredFilesystem {
  PartitionLocation location;
  // Volume correlation from FindFirstVolumeW.  `volume_paths` may contain
  // several paths if Windows mounted one volume more than once.
  bool windows_volume_found = false;
  std::vector<std::string> volume_paths;
  std::vector<std::string> mount_points;  // "E:\\" or a directory mount
  std::string volume_label;
  std::string volume_filesystem;
  XfsProbeResult probe;
  // True when a drive letter currently reports the WinFsp "XFS" volume name
  // and the volume serial derived from this filesystem's UUID.
  bool xfswin_mounted = false;
  std::string xfswin_drive;  // "E:"
};

struct RejectedFilesystem {
  PartitionLocation location;
  bool windows_volume_found = false;
  std::vector<std::string> mount_points;
  std::string volume_label;
  std::string volume_filesystem;
  XfsProbeResult probe;
};

struct ActiveXfsMount {
  std::string drive;  // "E:"
  std::string label;
  uint32_t serial_number = 0;
};

struct DiscoveryOptions {
  bool probe_journal = true;
  bool probe_writable_profile = true;
  // Include a whole-disk candidate when the disk has no partition table.
  // XFS formatted directly onto a removable device (the DEV-1R shape) is
  // discovered this way.
  bool include_whole_disk = true;
};

struct DiscoveryReport {
  std::vector<DiscoveredFilesystem> filesystems;
  // Candidates that contain XFS magic but did not pass verification.  They
  // are diagnostics, not mountable filesystems.
  std::vector<RejectedFilesystem> rejected;
  std::vector<ActiveXfsMount> active_mounts;
  std::vector<std::string> warnings;
  // True when at least one candidate could not be read or a disk handle could
  // not be opened; an automount retry may succeed later.
  bool incomplete = false;
  uint64_t retryable_failures = 0;
};

// Read-only probe of one candidate region.  `available_bytes` is the
// partition length (or the disk length for a whole-disk candidate); the
// filesystem and its internal log must lie inside it.  This function never
// writes, mounts or recovers.
XfsProbeResult ProbeXfsFilesystem(BlockDevice& device, uint64_t byte_offset,
                                  uint64_t available_bytes,
                                  const DiscoveryOptions& options);

// Enumerates the XFS filesystems currently attached to this Windows machine.
// On non-Windows platforms returns an error and an empty report.  Requires the
// ability to open raw disks read-only (administrator on Windows).
Result<DiscoveryReport> EnumerateXfsFilesystems(const DiscoveryOptions& options,
                                               std::string* error);

// Human-readable one-line description of a probe refusal, suitable for a
// normal CLI surface.  Detailed Win32/parser text stays in `probe.error`.
std::string DescribeProbeRefusal(const XfsProbeResult& probe);

}  // namespace xfs