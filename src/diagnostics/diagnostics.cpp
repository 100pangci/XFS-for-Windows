// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/diagnostics.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

#include "xfs/automount_config.h"
#include "xfs/device_discovery.h"
#include "xfs/uuid.h"
#include "xfswin_version.h"

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef _WIN32
#include "xfs/deployment.h"
#endif
#ifdef XFSWIN_HAS_WINFSP
#include "xfs/automount_service.h"
#endif

namespace xfs {
namespace {

std::string JsonString(const std::string& input) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : input) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) out << "\\u" << std::hex << std::setw(4)
                           << std::setfill('0') << static_cast<unsigned>(c)
                           << std::dec;
        else out << static_cast<char>(c);
    }
  }
  out << '"';
  return out.str();
}

}  // namespace

std::string RedactDiagnosticText(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    // Windows drive and device paths, UNC paths, and volume GUID paths are
    // replaced as one token. Diagnostic data never contains identity pins.
    const bool drive_path = i + 2 < text.size() &&
        std::isalpha(static_cast<unsigned char>(text[i])) && text[i + 1] == ':' &&
        (text[i + 2] == '\\' || text[i + 2] == '/');
    const bool unc_path = i + 1 < text.size() &&
        (text.compare(i, 2, "\\\\") == 0 || text.compare(i, 2, "//") == 0);
    if (drive_path || unc_path) {
      out += "[redacted path]";
      while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) &&
             text[i] != ')' && text[i] != ',' && text[i] != ';') ++i;
    } else {
      out.push_back(text[i++]);
    }
  }
  return out;
}

std::string FormatDiagnosticsText(const DiagnosticSnapshot& s) {
  std::ostringstream out;
  out << "XFS for Windows diagnostics (schema " << s.schema_version << ")\n"
      << "Version: " << RedactDiagnosticText(s.product_version) << "\n"
      << "WinFsp: " << RedactDiagnosticText(s.winfsp_version) << "\n"
      << "Automount service: " << RedactDiagnosticText(s.service_state);
  if (!s.service_detail.empty()) out << " (details omitted for privacy)";
  out << "\nDiscovery: " << RedactDiagnosticText(s.discovery_state);
  if (!s.discovery_detail.empty()) out << " (details omitted for privacy)";
  out << "\nDevices: " << s.devices.size() << '\n';
  for (const auto& d : s.devices) {
    out << "  [redacted label]  " << d.state << "  " << d.device_class
        << '/' << d.bus_type << "  " << d.capacity_bytes << " bytes  "
        << (d.read_only_known ? (d.read_only ? "read-only" : "read-write") : "access mode unknown");
    if (!d.drive.empty()) out << "  " << RedactDiagnosticText(d.drive);
    if (!d.log_state.empty()) out << "  log=" << d.log_state;
    if (!d.writable_profile.empty()) out << "  write-profile=" << RedactDiagnosticText(d.writable_profile);
    out << "  automount=" << RedactDiagnosticText(d.automount_mode);
    if (d.automount_mode != "not enrolled")
      out << (d.automount_enabled ? " (enabled)" : " (disabled)");
    if (!d.effective_mode.empty()) out << "  mounted-mode=" << RedactDiagnosticText(d.effective_mode);
    out << "  geometry=" << d.block_size << '/' << d.logical_sector_bytes << '/' << d.inode_size
        << "  ags=" << d.allocation_groups;
    if (!d.features.empty()) {
      out << "  features=";
      for (size_t i = 0; i < d.features.size(); ++i) out << (i ? "," : "") << d.features[i];
    }
    out << '\n';
  }
  if (!s.failures.empty()) out << "Partial failures: " << s.failures.size() << " (details omitted for privacy)\n";
  return out.str();
}

std::string FormatDiagnosticsJson(const DiagnosticSnapshot& s,
                                  bool include_identifiers) {
  std::ostringstream out;
  out << "{\"schema_version\":" << s.schema_version
      << ",\"product_version\":" << JsonString(RedactDiagnosticText(s.product_version))
      << ",\"winfsp_version\":" << JsonString(RedactDiagnosticText(s.winfsp_version))
      << ",\"service\":{\"state\":" << JsonString(RedactDiagnosticText(s.service_state))
      << ",\"detail\":" << JsonString(s.service_detail.empty() ? "" : "details omitted for privacy") << "}"
      << ",\"discovery\":{\"state\":" << JsonString(RedactDiagnosticText(s.discovery_state))
      << ",\"detail\":" << JsonString(s.discovery_detail.empty() ? "" : "details omitted for privacy") << "}"
      << ",\"devices\":[";
  for (size_t i = 0; i < s.devices.size(); ++i) {
    if (i) out << ',';
    const auto& d = s.devices[i];
    out << "{\"label\":" << JsonString(include_identifiers ? RedactDiagnosticText(d.label) : "[redacted]")
        << ",\"uuid\":" << JsonString(include_identifiers ? d.uuid : "[redacted]")
        << ",\"state\":" << JsonString(d.state)
        << ",\"device_class\":" << JsonString(d.device_class)
        << ",\"bus_type\":" << JsonString(d.bus_type)
        << ",\"serial_number\":" << JsonString(include_identifiers ? d.serial_number : "[redacted]")
        << ",\"stable_id\":" << JsonString(include_identifiers ? d.stable_id : "[redacted]")
        << ",\"discovered\":" << (d.discovered ? "true" : "false")
        << ",\"enrolled\":" << (d.enrolled ? "true" : "false")
        << ",\"mounted\":" << (d.mounted ? "true" : "false")
        << ",\"log_state\":" << JsonString(d.log_state)
        << ",\"writable_profile\":" << JsonString(RedactDiagnosticText(d.writable_profile))
        << ",\"automount_mode\":" << JsonString(RedactDiagnosticText(d.automount_mode))
        << ",\"automount_enabled\":" << (d.automount_enabled ? "true" : "false")
        << ",\"effective_mode\":" << JsonString(RedactDiagnosticText(d.effective_mode))
        << ",\"drive\":" << JsonString(RedactDiagnosticText(d.drive))
        << ",\"capacity_bytes\":" << d.capacity_bytes
        << ",\"device_size_bytes\":" << d.device_size_bytes
        << ",\"free_bytes\":" << d.free_bytes
        << ",\"logical_sector_bytes\":" << d.logical_sector_bytes
        << ",\"physical_sector_bytes\":" << d.physical_sector_bytes
        << ",\"block_size\":" << d.block_size
        << ",\"inode_size\":" << d.inode_size
        << ",\"allocation_groups\":" << d.allocation_groups
        << ",\"read_only\":" << (d.read_only_known ? (d.read_only ? "true" : "false") : "null") << ",\"features\":[";
    for (size_t j = 0; j < d.features.size(); ++j) {
      if (j) out << ',';
      out << JsonString(RedactDiagnosticText(d.features[j]));
    }
    out << "]}";
  }
  out << "],\"partial_failures\":[";
  for (size_t i = 0; i < s.failures.size(); ++i) {
    if (i) out << ',';
    out << JsonString("details omitted for privacy");
  }
  out << "]}\n";
  return out.str();
}



}  // namespace xfs
