// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/diagnostics.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "xfs/automount_config.h"
#include "xfs/device_discovery.h"
#include "xfs/uuid.h"
#include "xfswin_version.h"
#ifdef _WIN32
#include <windows.h>
#include "xfs/deployment.h"
#endif
#ifdef XFSWIN_HAS_WINFSP
#include "xfs/automount_service.h"
#endif

namespace xfs {
DiagnosticSnapshot CollectDiagnostics() {
  DiagnosticSnapshot snapshot;
  snapshot.product_version = XFSWIN_VERSION;
#ifdef _WIN32
  const auto winfsp = ProbeWinFspRuntime();
  snapshot.winfsp_version = winfsp.version;
  if (!winfsp.available) {
    std::string diagnostic = winfsp.diagnostic;
    std::transform(diagnostic.begin(), diagnostic.end(), diagnostic.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    snapshot.winfsp_version = diagnostic.find("unsupported") != std::string::npos
                                  ? "unsupported"
                                  : diagnostic.find("not found") != std::string::npos
                                        ? "not-present" : "unavailable";
    if (!winfsp.diagnostic.empty()) snapshot.failures.push_back("WinFsp runtime probe failed");
  }
#ifdef XFSWIN_HAS_WINFSP
  const auto service = QueryAutomountServiceStatus();
  snapshot.service_state = ToString(service.state);
  snapshot.service_detail = service.detail;
#else
  snapshot.service_state = "unavailable (built without WinFsp support)";
#endif
#ifdef XFSWIN_HAS_WINFSP
  AutomountConfig config;
  bool config_available = false;
  const auto paths = DefaultAutomountServicePaths();
  auto loaded_config = LoadAutomountConfig(paths.config_file);
  if (loaded_config.ok()) {
    config = loaded_config.value().config;
    config_available = true;
    for (const auto& warning : loaded_config.value().warnings)
      snapshot.failures.push_back("automount configuration warning: " + warning);
  } else {
    snapshot.failures.push_back("automount configuration unavailable: " +
                                loaded_config.error().Format());
  }
  AutomountRuntimeStatus runtime;
  if (snapshot.service_state == "running") {
    auto loaded_runtime = LoadAutomountRuntimeStatus(DefaultAutomountRuntimeStatusPath());
    if (loaded_runtime.ok()) runtime = std::move(loaded_runtime.value());
    else snapshot.failures.push_back("runtime mount status unavailable: " +
                                     loaded_runtime.error().Format());
  }
#endif
  std::string error;
  const auto result = EnumerateXfsFilesystems(DiscoveryOptions{}, &error);
  DiscoveryReport report;
  if (!result.ok()) {
    switch (result.error().code) {
      case XfsError::kPermissionDenied: snapshot.discovery_state = "permission-denied"; break;
      case XfsError::kNotFound: snapshot.discovery_state = "not-present"; break;
      case XfsError::kUnsupportedFeature: snapshot.discovery_state = "unsupported"; break;
      default: snapshot.discovery_state = "unavailable"; break;
    }
    snapshot.discovery_detail = result.error().Format();
    snapshot.failures.emplace_back("device discovery failed");
  } else {
    report = result.value();
    snapshot.discovery_state = report.incomplete ? "partial" : "complete";
    snapshot.discovery_detail = error;
  }
  for (const auto& fs : report.filesystems) {
    DiagnosticDevice d;
    d.label = fs.probe.label;
    d.discovered = true;
    d.enrolled = false;
    d.mounted = fs.xfswin_mounted;
    d.uuid = FormatUuid(fs.probe.superblock.uuid);
    d.state = ToString(fs.probe.state);
    d.device_class = ToString(fs.location.device_class);
    d.bus_type = ToString(fs.location.bus_type);
    d.serial_number = fs.location.serial_number;
    d.stable_id = fs.location.stable_id;
    d.log_state = ToString(fs.probe.log_state);
    d.writable_profile = fs.probe.writable_profile_detail;
    d.drive = fs.xfswin_drive;
    d.capacity_bytes = fs.probe.filesystem_bytes;
    d.device_size_bytes = fs.location.disk_size_bytes;
    d.free_bytes = fs.probe.free_bytes;
    d.logical_sector_bytes = fs.location.logical_sector_size;
    d.physical_sector_bytes = fs.location.physical_sector_size;
    d.block_size = fs.probe.geometry.block_size;
    d.inode_size = fs.probe.geometry.inode_size;
    d.allocation_groups = fs.probe.geometry.agcount;
    d.read_only = true;
    d.read_only_known = false;
    if (fs.xfswin_mounted && fs.xfswin_drive.size() == 2) {
      const char root[] = {fs.xfswin_drive[0], ':', '\\', '\0'};
      DWORD volume_flags = 0;
      if (GetVolumeInformationA(root, nullptr, 0, nullptr, nullptr,
                                &volume_flags, nullptr, 0)) {
        d.read_only = (volume_flags & FILE_READ_ONLY_VOLUME) != 0;
        d.read_only_known = true;
      } else {
        d.read_only_known = false;
        snapshot.failures.emplace_back("mounted volume access mode unavailable");
      }
    }
#ifdef XFSWIN_HAS_WINFSP
    if (config_available) {
      if (const auto* entry = config.Find(fs.probe.superblock.uuid)) {
        d.automount_mode = ToString(entry->mode);
        d.automount_enabled = entry->enabled;
        d.enrolled = true;
      }
    }
    for (const auto& mount : runtime.mounts) {
      if (UuidEqual(mount.fs_uuid, fs.probe.superblock.uuid)) {
        d.effective_mode = mount.mode;
        break;
      }
    }
#endif
    if (d.effective_mode.empty() && fs.xfswin_mounted && d.read_only_known)
      d.effective_mode = d.read_only ? "read-only" : "read-write";
    const auto& f = fs.probe.superblock.features;
    if (f.ftype) d.features.emplace_back("ftype");
    if (f.finobt) d.features.emplace_back("finobt");
    if (f.rmapbt) d.features.emplace_back("rmapbt");
    if (f.reflink) d.features.emplace_back("reflink");
    if (f.inobtcnt) d.features.emplace_back("inobtcount");
    if (f.bigtime) d.features.emplace_back("bigtime");
    if (f.nrext64) d.features.emplace_back("nrext64");
    if (f.parent) d.features.emplace_back("parent");
    snapshot.devices.push_back(std::move(d));
  }
  for (const auto& mount : report.active_mounts) {
    const auto found = std::find_if(snapshot.devices.begin(), snapshot.devices.end(),
        [&](const DiagnosticDevice& d) { return d.drive == mount.drive && d.mounted; });
    if (found != snapshot.devices.end()) continue;
    DiagnosticDevice active;
    active.label = mount.label;
    active.state = "mounted (raw scan unavailable)";
    active.mounted = true;
    active.drive = mount.drive;
    snapshot.devices.push_back(std::move(active));
  }
#ifdef XFSWIN_HAS_WINFSP
  auto find_device = [&](const Uuid& uuid) -> DiagnosticDevice* {
    const std::string formatted = FormatUuid(uuid);
    for (auto& device : snapshot.devices) if (device.uuid == formatted) return &device;
    return nullptr;
  };
  if (config_available) {
    for (const auto& entry : config.entries) {
      auto* device = find_device(entry.fs_uuid);
      if (!device) {
        DiagnosticDevice offline;
        offline.uuid = FormatUuid(entry.fs_uuid);
        offline.state = "not-present";
        offline.enrolled = true;
        offline.automount_mode = ToString(entry.mode);
        offline.automount_enabled = entry.enabled;
        offline.drive = entry.preferred_drive;
        snapshot.devices.push_back(std::move(offline));
      } else {
        device->enrolled = true;
        device->automount_mode = ToString(entry.mode);
        device->automount_enabled = entry.enabled;
      }
    }
  }
  for (const auto& mount : runtime.mounts) {
    auto* device = find_device(mount.fs_uuid);
    if (!device) {
      DiagnosticDevice active;
      active.uuid = FormatUuid(mount.fs_uuid);
      active.label = mount.label;
      active.state = "mounted (device scan unavailable)";
      active.mounted = true;
      active.drive = mount.drive;
      active.effective_mode = mount.mode;
      snapshot.devices.push_back(std::move(active));
    } else {
      device->mounted = true;
      device->effective_mode = mount.mode;
      if (!mount.drive.empty()) device->drive = mount.drive;
    }
  }
#endif
  for (const auto& rejected : report.rejected) {
    snapshot.failures.emplace_back(std::string("filesystem refused: ") +
                                   DescribeProbeRefusal(rejected.probe));
  }
  for (const auto& warning : report.warnings) snapshot.failures.push_back(warning);
  if (report.incomplete) snapshot.failures.emplace_back("some devices could not be read");
#else
  snapshot.winfsp_version = "not available on this platform";
  snapshot.service_state = "not available on this platform";
  snapshot.discovery_state = "not available on this platform";
  snapshot.discovery_detail = "Windows device discovery requires Windows";
  snapshot.failures.emplace_back("device discovery is platform-specific");
#endif
  return snapshot;
}

}  // namespace xfs
