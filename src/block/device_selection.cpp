// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/device_selection.h"

#include <cctype>
#include <cstdio>
#include <sstream>
#include <vector>

#include "xfs/drive_letter.h"
#include "xfs/uuid.h"

namespace xfs {
namespace {

std::string Trim(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end &&
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::string Lower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

bool AllDigits(const std::string& text) {
  if (text.empty()) return false;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

std::string LocationName(const PartitionLocation& location) {
  std::string name = location.disk_path.empty()
                         ? "disk " + std::to_string(location.disk_number)
                         : location.disk_path;
  if (location.partition_number != 0) {
    name += " partition " + std::to_string(location.partition_number);
  } else {
    name += " (whole disk)";
  }
  return name;
}

std::string LabelOrFallback(const DiscoveredFilesystem& filesystem) {
  return filesystem.probe.label.empty() ? "(no label)"
                                        : filesystem.probe.label;
}

}  // namespace

std::string FormatByteCount(uint64_t bytes) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 5) {
    value /= 1024.0;
    ++unit;
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, units[unit]);
  return buffer;
}

std::string DescribeFilesystemClass(const PartitionLocation& location) {
  switch (location.device_class) {
    case DeviceClass::kRemovableDisk: return "removable";
    case DeviceClass::kFixedDisk: return "fixed";
    case DeviceClass::kVirtualDisk: return "virtual";
    case DeviceClass::kImageFile: return "image";
    case DeviceClass::kVolume: return "volume";
    case DeviceClass::kMemory: return "memory";
    case DeviceClass::kUnknown:
      return location.removable ? "removable" : "unknown";
  }
  return "unknown";
}

std::string DescribeMountState(const DiscoveredFilesystem& filesystem) {
  if (filesystem.xfswin_mounted && !filesystem.xfswin_drive.empty()) {
    return "mounted at " + filesystem.xfswin_drive + " (xfswin)";
  }
  if (!filesystem.mount_points.empty()) {
    std::string state = "mounted by Windows at";
    for (size_t i = 0; i < filesystem.mount_points.size(); ++i) {
      if (i != 0) state += ",";
      state += " " + filesystem.mount_points[i];
    }
    return state;
  }
  return {};
}

std::string FormatFilesystemMenuEntry(
    size_t index, const DiscoveredFilesystem& filesystem) {
  std::ostringstream out;
  out << "  [" << index << "] " << LabelOrFallback(filesystem) << "  "
      << FormatByteCount(filesystem.probe.filesystem_bytes) << " ("
      << FormatByteCount(filesystem.probe.free_bytes) << " free)  "
      << DescribeFilesystemClass(filesystem.location) << "  UUID "
      << FormatUuid(filesystem.probe.superblock.uuid) << "  "
      << LocationName(filesystem.location);
  switch (filesystem.probe.log_state) {
    case LogState::kDirty:
      out << "  [journal dirty]";
      break;
    case LogState::kUnknown:
      out << "  [journal state unknown]";
      break;
    case LogState::kClean:
      break;
  }
  const std::string mount_state = DescribeMountState(filesystem);
  if (!mount_state.empty()) out << "  [" << mount_state << "]";
  return out.str();
}

std::vector<std::string> FormatDeviceMenu(const DiscoveryReport& report) {
  std::vector<std::string> lines;
  size_t index = 0;
  for (const auto& filesystem : report.filesystems) {
    ++index;
    lines.push_back(FormatFilesystemMenuEntry(index, filesystem));
  }
  return lines;
}

std::vector<std::string> FormatRejectedDevices(const DiscoveryReport& report) {
  std::vector<std::string> lines;
  for (const auto& rejected : report.rejected) {
    // Only XFS-looking candidates are useful here.  A plain non-XFS partition
    // is not an error the user needs to see.
    if (!rejected.probe.magic_seen) continue;
    std::string line = "  [!] " + LocationName(rejected.location) + ": " +
                       DescribeProbeRefusal(rejected.probe);
    if (!rejected.probe.label.empty()) {
      line += " (label '" + rejected.probe.label + "')";
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

Result<size_t> SelectDiscoveredFilesystem(
    std::string_view input,
    const std::vector<DiscoveredFilesystem>& filesystems) {
  const std::string text = Trim(input);
  if (text.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument, "no filesystem was selected");
  }
  if (filesystems.empty()) {
    return XFS_ERR(XfsError::kNotFound,
                   "no XFS filesystems were discovered");
  }

  std::string number_text = text;
  if (number_text.front() == '#') number_text.erase(0, 1);
  if (AllDigits(number_text)) {
    uint64_t number = 0;
    if (number_text.size() > 9) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "selection number is out of range");
    }
    for (char c : number_text) {
      number = number * 10 + static_cast<uint64_t>(c - '0');
    }
    if (number < 1 || number > filesystems.size()) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "selection must be a number from 1 to " +
                         std::to_string(filesystems.size()));
    }
    return static_cast<size_t>(number - 1);
  }

  Uuid uuid{};
  if (ParseUuid(text, &uuid)) {
    for (size_t i = 0; i < filesystems.size(); ++i) {
      if (UuidEqual(filesystems[i].probe.superblock.uuid, uuid)) {
        return i;
      }
    }
    return XFS_ERR(XfsError::kNotFound,
                   "no attached XFS filesystem has UUID " + text);
  }

  const std::string wanted = Lower(text);
  std::vector<size_t> matches;
  for (size_t i = 0; i < filesystems.size(); ++i) {
    if (Lower(filesystems[i].probe.label) == wanted) matches.push_back(i);
  }
  if (matches.size() == 1) return matches.front();
  if (matches.size() > 1) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "more than one attached filesystem is labeled '" + text +
                       "'; select it by number or UUID");
  }
  return XFS_ERR(XfsError::kNotFound,
                 "no attached XFS filesystem matches '" + text +
                     "'; enter a menu number, UUID, or label");
}

Result<DriveSelection> ParseDriveSelection(std::string_view input) {
  const std::string text = Trim(input);
  if (text.empty() || Lower(text) == "auto") {
    return DriveSelection{};
  }
  auto normalized = NormalizeDriveLetter(text);
  if (!normalized.ok()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "drive letter must be a single letter such as E: or "
                   "'auto'");
  }
  if (normalized.value() == "A:" || normalized.value() == "B:") {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "A: and B: are not valid mount letters");
  }
  DriveSelection selection;
  selection.automatic = false;
  selection.drive = normalized.value();
  return selection;
}

}  // namespace xfs