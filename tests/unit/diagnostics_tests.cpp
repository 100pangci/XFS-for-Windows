// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "test_util.h"

#include "xfs/diagnostics.h"

TEST(DiagnosticsRedactDeviceAndFilesystemPaths) {
  const std::string input = "failed opening \\\\.\\PhysicalDrive4 at C:\\Users\\Alice\\disk.img";
  const std::string redacted = xfs::RedactDiagnosticText(input);
  CHECK(redacted.find("PhysicalDrive4") == std::string::npos);
  CHECK(redacted.find("Users\\Alice") == std::string::npos);
  CHECK(redacted.find("[redacted path]") != std::string::npos);
}

TEST(DiagnosticsLabelsAreRedactedUnlessIdentifiersAreExplicitlyIncluded) {
  xfs::DiagnosticSnapshot snapshot;
  xfs::DiagnosticDevice device;
  device.label = "private {volume} \"quoted\"";
  snapshot.devices.push_back(device);
  const auto safe = xfs::FormatDiagnosticsJson(snapshot);
  CHECK(safe.find("private") == std::string::npos);
  CHECK(safe.find("[redacted]") != std::string::npos);
  const auto detailed = xfs::FormatDiagnosticsJson(snapshot, true);
  CHECK(detailed.find("private {volume} \\\"quoted\\\"") != std::string::npos);
}

TEST(DiagnosticsJsonKeepsIndependentPartialFailures) {
  xfs::DiagnosticSnapshot snapshot;
  snapshot.product_version = "1.2.3";
  snapshot.service_state = "running";
  snapshot.discovery_state = "partial";
  snapshot.failures = {"service query failed", "device read failed"};
  xfs::DiagnosticDevice device;
  device.label = "private \"label\" {disk} \\ name";
  device.uuid = "00112233";
  device.capacity_bytes = 4096;
  device.read_only = true;
  snapshot.devices.push_back(device);
  const auto json = xfs::FormatDiagnosticsJson(snapshot);
  CHECK(json.find("\"schema_version\":1") != std::string::npos);
  CHECK(json.find("\"partial_failures\":[\"details omitted for privacy\",\"details omitted for privacy\"]") != std::string::npos);
  CHECK(json.find("service query failed") == std::string::npos);
  CHECK(json.find("\"label\":\"[redacted]\"") != std::string::npos);
  CHECK(json.find("private") == std::string::npos);
  CHECK(json.find("\"capacity_bytes\":4096") != std::string::npos);
  CHECK(json.find("\"read_only\":true") != std::string::npos);
  CHECK(json.find("00112233") == std::string::npos);
  CHECK(xfs::FormatDiagnosticsJson(snapshot, true).find("00112233") != std::string::npos);
}

TEST(DiagnosticsRedactSensitivePathsInEveryFormat) {
  xfs::DiagnosticSnapshot snapshot;
  snapshot.discovery_detail = "cannot access \\\\.\\PhysicalDrive9";
  snapshot.failures.push_back("volume C:\\private\\xfs.img unavailable");
  const auto json = xfs::FormatDiagnosticsJson(snapshot);
  const auto text = xfs::FormatDiagnosticsText(snapshot);
  CHECK(json.find("PhysicalDrive9") == std::string::npos);
  CHECK(json.find("private\\\\xfs.img") == std::string::npos);
  CHECK(text.find("PhysicalDrive9") == std::string::npos);
  CHECK(text.find("private\\xfs.img") == std::string::npos);
}
