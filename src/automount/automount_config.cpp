// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/automount_config.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

#include "xfs/drive_letter.h"

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

bool ParseBool(std::string_view text, bool* out) {
  const std::string value = Lower(Trim(text));
  if (value == "true" || value == "yes" || value == "1" || value == "on") {
    *out = true;
    return true;
  }
  if (value == "false" || value == "no" || value == "0" || value == "off") {
    *out = false;
    return true;
  }
  return false;
}

bool ParseU64Decimal(std::string_view text, uint64_t* out) {
  const std::string value = Trim(text);
  if (value.empty()) return false;
  uint64_t result = 0;
  for (char c : value) {
    if (c < '0' || c > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (result > (UINT64_MAX - digit) / 10) return false;
    result = result * 10 + digit;
  }
  *out = result;
  return true;
}

bool ParseDeviceClass(std::string_view text, DeviceClass* out) {
  const std::string value = Lower(Trim(text));
  if (value.empty() || value == "unknown" || value == "any") {
    *out = DeviceClass::kUnknown;
    return true;
  }
  if (value == "removable-disk" || value == "removable" || value == "usb") {
    *out = DeviceClass::kRemovableDisk;
    return true;
  }
  if (value == "virtual-disk" || value == "virtual" || value == "vhd") {
    *out = DeviceClass::kVirtualDisk;
    return true;
  }
  if (value == "fixed-disk" || value == "fixed") {
    *out = DeviceClass::kFixedDisk;
    return true;
  }
  if (value == "image-file" || value == "image") {
    *out = DeviceClass::kImageFile;
    return true;
  }
  if (value == "volume") {
    *out = DeviceClass::kVolume;
    return true;
  }
  return false;
}

std::string LineError(size_t line, const std::string& message) {
  return "line " + std::to_string(line) + ": " + message;
}

}  // namespace

const char* ToString(AutomountMode mode) {
  switch (mode) {
    case AutomountMode::kReadOnly: return "read-only";
    case AutomountMode::kReadWrite: return "read-write";
  }
  return "read-only";
}

bool ParseAutomountMode(std::string_view input, AutomountMode* out) {
  const std::string value = Lower(Trim(input));
  if (value == "read-only" || value == "readonly" || value == "ro") {
    *out = AutomountMode::kReadOnly;
    return true;
  }
  // read-write/rw are canonical; experimental-rw/experimental are accepted so
  // configurations written before the profile graduated still load.  The next
  // save rewrites them as `mode = read-write`.
  if (value == "read-write" || value == "rw" || value == "experimental-rw" ||
      value == "experimental") {
    *out = AutomountMode::kReadWrite;
    return true;
  }
  return false;
}

const AutomountEntry* AutomountConfig::Find(const Uuid& uuid) const {
  for (const auto& entry : entries) {
    if (UuidEqual(entry.fs_uuid, uuid)) return &entry;
  }
  return nullptr;
}

AutomountEntry* AutomountConfig::Find(const Uuid& uuid) {
  for (auto& entry : entries) {
    if (UuidEqual(entry.fs_uuid, uuid)) return &entry;
  }
  return nullptr;
}

void AutomountConfig::Upsert(const AutomountEntry& entry) {
  if (auto* existing = Find(entry.fs_uuid)) {
    *existing = entry;
    return;
  }
  entries.push_back(entry);
}

bool AutomountConfig::Remove(const Uuid& uuid) {
  for (auto it = entries.begin(); it != entries.end(); ++it) {
    if (UuidEqual(it->fs_uuid, uuid)) {
      entries.erase(it);
      return true;
    }
  }
  return false;
}

Status ValidateAutomountEntry(const AutomountEntry& entry) {
  if (IsNullUuid(entry.fs_uuid)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "automount entry has no filesystem UUID");
  }
  if (!entry.preferred_drive.empty()) {
    auto normalized = NormalizeDriveLetter(entry.preferred_drive);
    if (!normalized.ok() || normalized.value() == "A:" ||
        normalized.value() == "B:") {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "preferred drive must be a letter from C: to Z: or "
                     "auto")
          .Metadata("automount");
    }
  }
  if (entry.mode != AutomountMode::kReadWrite) return OkStatus();

  if (!entry.device_qualification) {
    return XFS_ERR(XfsError::kPermissionDenied,
                   "automount read-write requires explicit device write "
                   "qualification (--device-write-qualification)")
        .Metadata("automount");
  }
  // Windows exposes no serial number for its own virtual disks (VHD/VHDX
  // report an empty STORAGE_DEVICE_DESCRIPTOR serial).  The stable device id
  // (the volume DeviceGuid) and the size are the identity there.  Every
  // physical device class still has to pin its serial.
  const bool serial_required =
      entry.expected_device_class != DeviceClass::kVirtualDisk;
  if (entry.expected_stable_id.empty() || entry.expected_size_bytes == 0 ||
      (serial_required && entry.expected_serial.empty())) {
    return XFS_ERR(XfsError::kPermissionDenied,
                   "automount read-write requires the DEV-1 identity pins: "
                   "expected serial, stable device id and size (virtual disks "
                   "have no serial and are pinned by stable id, size and "
                   "class)")
        .Metadata("automount");
  }
  if (entry.allow_fixed_disk) {
    return XFS_ERR(XfsError::kPermissionDenied,
                   "fixed physical disks are not qualified for automount "
                   "read-write; only the tested removable/virtual profile may "
                   "be enrolled")
        .Metadata("automount");
  }
  if (entry.expected_device_class == DeviceClass::kFixedDisk) {
    return XFS_ERR(XfsError::kPermissionDenied,
                   "fixed physical disks are not qualified for automount "
                   "read-write")
        .Metadata("automount");
  }
  if (entry.expected_device_class == DeviceClass::kRemovableDisk &&
      !entry.allow_removable_disk) {
    return XFS_ERR(XfsError::kPermissionDenied,
                   "a removable-disk pin requires the explicit removable-disk "
                   "qualification opt-in")
        .Metadata("automount");
  }
  // A virtual disk is writable under the DEV-1 policy without a class opt-in.
  // A removable disk needs the same explicit --allow-removable-disk-write
  // acknowledgement here; the mount path re-checks the class it actually
  // probed, so an entry cannot claim one class and mount another.
  if (!entry.allow_removable_disk) {
    return OkStatus();
  }
  return OkStatus();
}

Result<AutomountConfigLoad> ParseAutomountConfig(std::string_view text) {
  AutomountConfigLoad load;
  AutomountConfig& config = load.config;
  bool saw_version = false;
  bool have_entry = false;
  AutomountEntry current;
  size_t line_number = 0;

  auto finish_entry = [&]() -> Status {
    if (!have_entry) return OkStatus();
    if (IsNullUuid(current.fs_uuid)) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "filesystem section has an invalid UUID");
    }
    if (config.Find(current.fs_uuid)) {
      return XFS_ERR(XfsError::kAlreadyExists,
                     "duplicate filesystem section for " +
                         FormatUuid(current.fs_uuid));
    }
    config.entries.push_back(current);
    current = AutomountEntry{};
    have_entry = false;
    return OkStatus();
  };

  std::istringstream stream{std::string(text)};
  std::string raw;
  while (std::getline(stream, raw)) {
    ++line_number;
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    const std::string line = Trim(raw);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;

    if (line.front() == '[' && line.back() == ']') {
      Status finished = finish_entry();
      if (!finished.ok()) return finished.error();
      const std::string inner = Trim(
          std::string_view(line).substr(1, line.size() - 2));
      const size_t quote = inner.find('"');
      const size_t last_quote = inner.rfind('"');
      if (Lower(Trim(std::string_view(inner).substr(0, quote))) !=
              "filesystem" ||
          quote == std::string::npos || last_quote == quote) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       LineError(line_number,
                                 "expected [filesystem \"<uuid>\"]"))
            .Metadata("automount");
      }
      const std::string uuid_text = inner.substr(quote + 1,
                                                 last_quote - quote - 1);
      Uuid uuid{};
      if (!ParseUuid(uuid_text, &uuid)) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       LineError(line_number, "invalid filesystem UUID"))
            .Metadata("automount");
      }
      current = AutomountEntry{};
      current.fs_uuid = uuid;
      have_entry = true;
      continue;
    }

    const size_t equals = line.find('=');
    if (equals == std::string::npos) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     LineError(line_number, "expected key = value"))
          .Metadata("automount");
    }
    const std::string key = Lower(Trim(std::string_view(line).substr(0, equals)));
    const std::string value = Trim(std::string_view(line).substr(equals + 1));

    if (!have_entry) {
      if (key == "version") {
        uint64_t parsed = 0;
        if (!ParseU64Decimal(value, &parsed) || parsed == 0) {
          return XFS_ERR(XfsError::kInvalidArgument,
                         LineError(line_number, "invalid version"))
              .Metadata("automount");
        }
        if (parsed > AutomountConfig::kCurrentVersion) {
          return XFS_ERR(XfsError::kUnsupportedFeature,
                         LineError(line_number,
                                   "configuration was written by a newer "
                                   "xfswin version"))
              .Metadata("automount");
        }
        config.version = static_cast<uint32_t>(parsed);
        saw_version = true;
      } else if (key == "debounce-ms") {
        uint64_t parsed = 0;
        if (!ParseU64Decimal(value, &parsed) || parsed > 60000) {
          return XFS_ERR(XfsError::kInvalidArgument,
                         LineError(line_number, "invalid debounce-ms"))
              .Metadata("automount");
        }
        config.debounce_ms = static_cast<uint32_t>(parsed);
      } else if (key == "reconcile-seconds") {
        uint64_t parsed = 0;
        if (!ParseU64Decimal(value, &parsed) || parsed < 5 || parsed > 86400) {
          return XFS_ERR(XfsError::kInvalidArgument,
                         LineError(line_number, "invalid reconcile-seconds"))
              .Metadata("automount");
        }
        config.reconcile_seconds = static_cast<uint32_t>(parsed);
      } else {
        load.warnings.push_back(LineError(line_number,
                                          "unknown key '" + key + "' ignored"));
      }
      continue;
    }

    if (key == "enabled") {
      if (!ParseBool(value, &current.enabled)) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       LineError(line_number, "enabled must be true or false"))
            .Metadata("automount");
      }
    } else if (key == "mode") {
      if (!ParseAutomountMode(value, &current.mode)) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       LineError(line_number,
                                 "mode must be read-only or read-write"))
            .Metadata("automount");
      }
    } else if (key == "drive") {
      if (Lower(value) == "auto" || value.empty()) {
        current.preferred_drive.clear();
      } else {
        auto normalized = NormalizeDriveLetter(value);
        if (!normalized.ok()) {
          return XFS_ERR(XfsError::kInvalidArgument,
                         LineError(line_number, "invalid drive letter"))
              .Metadata("automount");
        }
        current.preferred_drive = normalized.value();
      }
    } else if (key == "expected-serial") {
      current.expected_serial = value;
    } else if (key == "expected-stable-id") {
      current.expected_stable_id = value;
    } else if (key == "expected-size") {
      if (!ParseU64Decimal(value, &current.expected_size_bytes)) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       LineError(line_number,
                                 "expected-size must be a byte count"))
            .Metadata("automount");
      }
    } else if (key == "expected-device-class") {
      if (!ParseDeviceClass(value, &current.expected_device_class)) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       LineError(line_number,
                                 "expected-device-class must be one of "
                                 "unknown, removable-disk, virtual-disk, "
                                 "fixed-disk, image-file or volume"))
            .Metadata("automount");
      }
    } else if (key == "device-qualification") {
      if (!ParseBool(value, &current.device_qualification)) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       LineError(line_number,
                                 "device-qualification must be true or false"))
            .Metadata("automount");
      }
    } else if (key == "allow-removable-disk") {
      if (!ParseBool(value, &current.allow_removable_disk)) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       LineError(line_number,
                                 "allow-removable-disk must be true or false"))
            .Metadata("automount");
      }
    } else if (key == "allow-fixed-disk") {
      if (!ParseBool(value, &current.allow_fixed_disk)) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       LineError(line_number,
                                 "allow-fixed-disk must be true or false"))
            .Metadata("automount");
      }
    } else if (key == "removal-policy-overridden") {
      if (!ParseBool(value, &current.removal_policy_overridden)) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       LineError(line_number,
                                 "removal-policy-overridden must be true or "
                                 "false"))
            .Metadata("automount");
      }
    } else {
      load.warnings.push_back(LineError(
          line_number, "unknown filesystem key '" + key + "' ignored"));
    }
  }

  Status finished = finish_entry();
  if (!finished.ok()) return finished.error();
  if (!saw_version && !config.entries.empty()) {
    load.warnings.push_back(
        "configuration has no version key; treating it as version 1");
  }
  config.version = AutomountConfig::kCurrentVersion;
  for (const auto& entry : config.entries) {
    Status valid = ValidateAutomountEntry(entry);
    if (!valid.ok()) {
      return XFS_ERR(valid.error().code,
                     "filesystem " + FormatUuid(entry.fs_uuid) + ": " +
                         valid.error().message)
          .Metadata("automount");
    }
  }
  return load;
}

Result<AutomountConfigLoad> LoadAutomountConfig(
    const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (ec) {
      return XFS_ERR(XfsError::kIoError,
                     "cannot inspect configuration: " + ec.message());
    }
    return AutomountConfigLoad{};
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return XFS_ERR(XfsError::kIoError,
                   "cannot open configuration: " + path.string());
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  auto parsed = ParseAutomountConfig(buffer.str());
  if (!parsed.ok()) {
    return XFS_ERR(parsed.error().code,
                   path.string() + ": " + parsed.error().message);
  }
  return parsed.value();
}

std::string SerializeAutomountConfig(const AutomountConfig& config) {
  std::vector<const AutomountEntry*> sorted;
  sorted.reserve(config.entries.size());
  for (const auto& entry : config.entries) sorted.push_back(&entry);
  std::sort(sorted.begin(), sorted.end(),
            [](const AutomountEntry* a, const AutomountEntry* b) {
              return FormatUuid(a->fs_uuid) < FormatUuid(b->fs_uuid);
            });

  std::ostringstream out;
  out << "# xfswin automount configuration\n";
  out << "# Filesystem identity is the XFS UUID; drive letters are preferences.\n";
  out << "version = " << AutomountConfig::kCurrentVersion << "\n";
  out << "debounce-ms = " << config.debounce_ms << "\n";
  out << "reconcile-seconds = " << config.reconcile_seconds << "\n";
  for (const AutomountEntry* entry : sorted) {
    out << "\n[filesystem \"" << FormatUuid(entry->fs_uuid) << "\"]\n";
    out << "enabled = " << (entry->enabled ? "true" : "false") << "\n";
    out << "mode = " << ToString(entry->mode) << "\n";
    out << "drive = "
        << (entry->preferred_drive.empty() ? "auto" : entry->preferred_drive)
        << "\n";
    out << "expected-serial = " << entry->expected_serial << "\n";
    out << "expected-stable-id = " << entry->expected_stable_id << "\n";
    out << "expected-size = " << entry->expected_size_bytes << "\n";
    out << "expected-device-class = "
        << ToString(entry->expected_device_class) << "\n";
    out << "device-qualification = "
        << (entry->device_qualification ? "true" : "false") << "\n";
    out << "allow-removable-disk = "
        << (entry->allow_removable_disk ? "true" : "false") << "\n";
    out << "allow-fixed-disk = "
        << (entry->allow_fixed_disk ? "true" : "false") << "\n";
    out << "removal-policy-overridden = "
        << (entry->removal_policy_overridden ? "true" : "false") << "\n";
  }
  return out.str();
}

Status SaveAutomountConfig(const std::filesystem::path& path,
                           const AutomountConfig& config) {
  for (const auto& entry : config.entries) {
    Status valid = ValidateAutomountEntry(entry);
    if (!valid.ok()) return valid;
  }
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return XFS_ERR(XfsError::kIoError,
                     "cannot create configuration directory: " + ec.message());
    }
  }
  static std::atomic<uint64_t> sequence{0};
  const std::filesystem::path temporary =
      path.string() + ".tmp-" + std::to_string(
          static_cast<unsigned long long>(sequence.fetch_add(1)));
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      return XFS_ERR(XfsError::kIoError,
                     "cannot write configuration: " + temporary.string());
    }
    file << SerializeAutomountConfig(config);
    file.flush();
    if (!file) {
      std::filesystem::remove(temporary, ec);
      return XFS_ERR(XfsError::kIoError,
                     "cannot flush configuration: " + temporary.string());
    }
  }
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    // Windows refuses rename over an existing read-only or open file.  The
    // caller still gets a clear error instead of a half-written config.
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return XFS_ERR(XfsError::kIoError,
                   "cannot replace configuration: " + ec.message());
  }
  return OkStatus();
}

}  // namespace xfs