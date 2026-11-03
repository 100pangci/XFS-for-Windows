// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - Windows discovery backend.
//
// This file owns enumeration only.  Verification and policy are in
// device_discovery.cpp; the raw-device lifecycle is DEV-1's backend.
#include "xfs/device_discovery.h"
#include "xfs/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xfs/windows_volume.h"

#ifdef _WIN32
#include <windows.h>

#include <cwchar>

#include "xfs/windows_raw_device.h"

namespace xfs {
namespace {

std::wstring ToWide(const std::string& utf8) {
  if (utf8.empty()) return {};
  int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      out.data(), len);
  return out;
}

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                      out.data(), len, nullptr, nullptr);
  return out;
}

bool EqualsAsciiCaseInsensitive(const std::string& a, const char* b) {
  if (a.size() != std::strlen(b)) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 32);
    if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 32);
    if (ca != cb) return false;
  }
  return true;
}

std::vector<ActiveXfsMount> FindActiveXfsMounts(
    std::vector<std::string>* warnings) {
  std::vector<ActiveXfsMount> mounts;
  std::string error;
  const auto letters = EnumerateDriveLetters(&error);
  if (!error.empty() && warnings) warnings->push_back(error);
  for (const auto& letter : letters) {
    const std::wstring root =
        std::wstring(1, static_cast<wchar_t>(letter.letter)) + L":\\";
    wchar_t fs_name[261] = {};
    wchar_t label[261] = {};
    DWORD serial = 0;
    XFS_DEBUG("discovery: inspecting drive " << letter.letter << ":");
    if (!GetVolumeInformationW(root.c_str(), label, 260, &serial, nullptr,
                               nullptr, fs_name, 260)) {
      // A drive letter can exist while its volume refuses access (for example
      // a mount owned by another account).  Report why instead of silently
      // treating it as a non-XFS volume.
      XFS_DEBUG("discovery: drive " << letter.letter
                                    << ": volume information failed (error "
                                    << GetLastError() << ")");
      continue;
    }
    XFS_DEBUG("discovery: drive " << letter.letter
                                  << ": filesystem=" << WideToUtf8(fs_name));
    if (!EqualsAsciiCaseInsensitive(WideToUtf8(fs_name), "XFS")) continue;
    ActiveXfsMount mount;
    mount.drive = std::string(1, letter.letter) + ":";
    mount.label = WideToUtf8(label);
    mount.serial_number = static_cast<uint32_t>(serial);
    mounts.push_back(std::move(mount));
  }
  return mounts;
}

const RawVolumeInfo* FindVolumeForCandidate(
    const std::vector<RawVolumeInfo>& volumes, uint32_t disk_number,
    uint64_t offset_bytes, uint64_t length_bytes, uint32_t partition_number,
    bool* exact) {
  *exact = false;
  // Exact identity: one extent whose disk, starting offset and length all
  // equal the XFS candidate.  Only this relationship may authorize a drive
  // letter takeover; a match by partition number alone is display data.
  const RawVolumeInfo* exact_volume = nullptr;
  size_t exact_matches = 0;
  for (const auto& volume : volumes) {
    if (volume.extents.size() != 1) continue;
    const VolumeExtent& extent = volume.extents.front();
    if (extent.disk_number != disk_number) continue;
    if (extent.starting_offset != offset_bytes) continue;
    if (extent.length_bytes != length_bytes) continue;
    if (partition_number != 0 && volume.partition_number != 0 &&
        volume.partition_number != partition_number) {
      continue;
    }
    ++exact_matches;
    exact_volume = &volume;
  }
  if (exact_matches == 1) {
    *exact = true;
    return exact_volume;
  }
  if (exact_matches > 1) {
    // Two Windows volumes claiming the same extent is not a relationship we
    // can act on.  Return no correlation rather than guessing.
    return nullptr;
  }
  // Approximate correlation for display: same disk and starting offset but a
  // different reported extent length (for example a multi-extent volume).
  for (const auto& volume : volumes) {
    for (const auto& extent : volume.extents) {
      if (extent.disk_number == disk_number &&
          extent.starting_offset == offset_bytes) {
        return &volume;
      }
    }
  }
  // Last resort for drivers that do not report extents at all.
  for (const auto& volume : volumes) {
    if (partition_number != 0 && volume.partition_number == partition_number &&
        volume.disk_number == disk_number) {
      return &volume;
    }
  }
  return nullptr;
}

PartitionLocation MakeLocation(const RawDiskInfo& disk,
                               const RawPartitionInfo* partition,
                               bool whole_disk) {
  PartitionLocation location;
  location.disk_number = disk.disk_number;
  location.disk_path = disk.device_path;
  location.whole_disk = whole_disk;
  location.removable = disk.is_removable;
  location.device_class = disk.device_class;
  location.bus_type = disk.bus_type;
  location.logical_sector_size = disk.logical_sector_size;
  location.physical_sector_size = disk.physical_sector_size;
  location.model = disk.model;
  location.serial_number = disk.serial_number;
  location.stable_id = disk.stable_id;
  location.system_disk = disk.is_system_disk;
  location.pagefile_disk = disk.has_pagefile;
  location.offline = disk.offline;
  location.read_only = disk.read_only;
  location.disk_size_bytes = disk.size_bytes;
  if (partition) {
    location.partition_number = partition->number;
    location.offset_bytes = partition->offset_bytes;
    location.length_bytes = partition->length_bytes;
  } else {
    location.partition_number = 0;
    location.offset_bytes = 0;
    location.length_bytes = disk.size_bytes;
  }
  return location;
}

uint32_t XfsVolumeSerial(const XfsSuperblock& sb) {
  return static_cast<uint32_t>(sb.uuid[0] | (sb.uuid[1] << 8) |
                               (sb.uuid[2] << 16) |
                               (static_cast<uint32_t>(sb.uuid[3]) << 24));
}

bool VolumeTouchesDisk(const RawVolumeInfo& volume, uint32_t disk_number) {
  if (!volume.extents.empty()) {
    for (const auto& extent : volume.extents) {
      if (extent.disk_number == disk_number) return true;
    }
    return false;
  }
  return volume.disk_number == disk_number;
}

// True when a volume other than `candidate` on the same physical disk has a
// filesystem Windows recognizes.  The DEV-1 read-write open locks/dismounts
// every volume on the disk, so mounting XFS read-write there would disturb
// that filesystem.
bool DiskHasOtherFilesystems(const std::vector<RawVolumeInfo>& volumes,
                             const RawVolumeInfo* candidate,
                             uint32_t disk_number) {
  for (const auto& volume : volumes) {
    if (&volume == candidate) continue;
    if (!volume.has_filesystem) continue;
    if (VolumeTouchesDisk(volume, disk_number)) return true;
  }
  return false;
}

std::string DiskLabel(const RawDiskInfo& disk) {
  if (!disk.model.empty()) return disk.model;
  return disk.device_path;
}

}  // namespace

Result<DiscoveryReport> EnumerateXfsFilesystems(
    const DiscoveryOptions& options, std::string* error) {
  DiscoveryReport report;
  auto mark_incomplete_disk = [&report](uint32_t disk_number) {
    if (std::find(report.incomplete_disks.begin(),
                  report.incomplete_disks.end(),
                  disk_number) == report.incomplete_disks.end()) {
      report.incomplete_disks.push_back(disk_number);
    }
  };
  std::string enumerate_error;
  XFS_DEBUG("discovery: enumerating physical drives");
  auto disks = EnumeratePhysicalDrives(&enumerate_error);
  XFS_DEBUG("discovery: physical drives=" << disks.size());
  if (disks.empty() && !enumerate_error.empty()) {
    if (error) *error = enumerate_error;
    return XFS_ERR(XfsError::kPermissionDenied, enumerate_error)
        .Metadata("device discovery");
  }
  if (!enumerate_error.empty()) report.warnings.push_back(enumerate_error);

  std::string volume_error;
  XFS_DEBUG("discovery: enumerating volumes");
  auto volumes = EnumerateVolumes(&volume_error);
  XFS_DEBUG("discovery: volumes=" << volumes.size());
  if (!volume_error.empty()) report.warnings.push_back(volume_error);
  XFS_DEBUG("discovery: inspecting drive letters");
  report.active_mounts = FindActiveXfsMounts(&report.warnings);
  XFS_DEBUG("discovery: xfswin mounts=" << report.active_mounts.size());

  for (const auto& disk : disks) {
    if (disk.offline) {
      report.warnings.push_back(
          "skipping offline disk " + std::to_string(disk.disk_number) + " (" +
          DiskLabel(disk) + ")");
      continue;
    }
    std::string open_error;
    auto device =
        OpenRawDevice(ToWide(disk.device_path), false, false, &open_error);
    if (!device) {
      report.incomplete = true;
      ++report.retryable_failures;
      mark_incomplete_disk(disk.disk_number);
      report.warnings.push_back(
          "cannot read disk " + std::to_string(disk.disk_number) + " (" +
          DiskLabel(disk) + "): " + open_error);
      continue;
    }

    struct Candidate {
      const RawPartitionInfo* partition = nullptr;
      bool whole_disk = false;
      uint64_t offset = 0;
      uint64_t length = 0;
      uint32_t number = 0;
    };
    std::vector<Candidate> candidates;
    for (const auto& partition : disk.partitions) {
      if (partition.length_bytes == 0) continue;
      Candidate c;
      c.partition = &partition;
      c.offset = partition.offset_bytes;
      c.length = partition.length_bytes;
      c.number = partition.number;
      candidates.push_back(c);
    }
    if (candidates.empty() && options.include_whole_disk &&
        disk.size_bytes != 0) {
      Candidate c;
      c.whole_disk = true;
      c.offset = 0;
      c.length = disk.size_bytes;
      c.number = 0;
      candidates.push_back(c);
    }

    for (const auto& candidate : candidates) {
      const PartitionLocation location =
          MakeLocation(disk, candidate.partition, candidate.whole_disk);
      XfsProbeResult probe = ProbeXfsFilesystem(
          *device, candidate.offset, candidate.length, options);
      if (probe.state == XfsProbeState::kUnreadable) {
        report.incomplete = true;
        ++report.retryable_failures;
        mark_incomplete_disk(disk.disk_number);
      }
      if (probe.state == XfsProbeState::kNoXfs) continue;

      bool volume_exact = false;
      const RawVolumeInfo* volume = FindVolumeForCandidate(
          volumes, disk.disk_number, candidate.offset, candidate.length,
          candidate.number, &volume_exact);

      if (probe.state == XfsProbeState::kValid) {
        DiscoveredFilesystem fs;
        fs.location = location;
        fs.probe = std::move(probe);
        if (volume) {
          fs.windows_volume_found = true;
          fs.volume_identity_exact = volume_exact;
          fs.volume_unrecognized_raw = volume->unrecognized_filesystem;
          fs.volume_device_path = volume->device_path;
          fs.disk_has_other_filesystems = DiskHasOtherFilesystems(
              volumes, volume, disk.disk_number);
          if (!volume->device_path.empty()) {
            fs.volume_paths.push_back(volume->device_path);
          }
          fs.mount_points = volume->mount_points;
          fs.volume_label = volume->label;
          fs.volume_filesystem = volume->filesystem;
        }
        const uint32_t serial = XfsVolumeSerial(fs.probe.superblock);
        for (const auto& mount : report.active_mounts) {
          if (mount.serial_number != serial) continue;
          if (!mount.label.empty() && !fs.probe.label.empty() &&
              mount.label != fs.probe.label) {
            continue;
          }
          fs.xfswin_mounted = true;
          fs.xfswin_drive = mount.drive;
          break;
        }
        report.filesystems.push_back(std::move(fs));
        continue;
      }

      RejectedFilesystem rejected;
      rejected.location = location;
      rejected.probe = std::move(probe);
      if (volume) {
        rejected.windows_volume_found = true;
        rejected.mount_points = volume->mount_points;
        rejected.volume_label = volume->label;
        rejected.volume_filesystem = volume->filesystem;
      }
      report.rejected.push_back(std::move(rejected));
    }
    device->Close();
  }
  return report;
}

}  // namespace xfs

#else  // !_WIN32

namespace xfs {

Result<DiscoveryReport> EnumerateXfsFilesystems(
    const DiscoveryOptions&, std::string* error) {
  if (error) *error = "device discovery is only supported on Windows";
  return XFS_ERR(XfsError::kUnsupportedFeature,
                 "device discovery is only supported on Windows");
}

}  // namespace xfs

#endif  // _WIN32