// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - resident automount service (UX-1).
//
// The service is a thin Windows shell around the platform-independent
// automount engine: it turns device-arrival/removal notifications into engine
// wakeups, owns the WinFsp mount sessions, and installs/starts/stops itself
// through the Service Control Manager.  The engine, discovery and drive-letter
// policy stay in the core and are tested without a service or real USB.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "xfs/error.h"
#include "xfs/uuid.h"

namespace xfs {

struct AutomountServicePaths {
  // %ProgramData%\xfswin\automount.ini
  std::filesystem::path config_file;
  // %ProgramData%\xfswin
  std::filesystem::path directory;
  // Windows Event Log source used by FspServiceLog.
  std::string service_name;  // "XFSWinAutomount"
};

AutomountServicePaths DefaultAutomountServicePaths();

// Creates the configuration directory if necessary.  The service calls this at
// start so the configuration file watcher has a directory to observe.
Status EnsureAutomountServicePaths(const AutomountServicePaths& paths);

enum class AutomountServiceState : uint8_t {
  kNotInstalled = 0,
  kStopped,
  kStartPending,
  kStopPending,
  kRunning,
  kPaused,
  kUnknown,
};

const char* ToString(AutomountServiceState state);

struct AutomountServiceStatusInfo {
  AutomountServiceState state = AutomountServiceState::kNotInstalled;
  // Human-readable detail, including the Win32 error when the query failed.
  std::string detail;
  bool installed() const {
    return state != AutomountServiceState::kNotInstalled;
  }
};

// Read-only SCM query.  Never starts or changes the service.
AutomountServiceStatusInfo QueryAutomountServiceStatus();

// ---------------------------------------------------------------------------
// Live automount session state.
//
// The service is the only process that knows which drive letter was taken over
// from a RAW Windows volume and whether an enrolled read-write entry fell back
// to read-only.  It publishes that small snapshot so `xfswin status` can show
// the effective state even though the engine lives in the service process.
// The snapshot is not configuration and never authorizes anything.
// ---------------------------------------------------------------------------
struct AutomountRuntimeMount {
  Uuid fs_uuid{};
  std::string label;
  std::string drive;
  // Effective mode as reported by the engine: "read-only" or
  // "read-write".
  std::string mode;
  std::string configured_mode;
  bool drive_taken_over = false;
  std::string original_windows_drive;
  std::string detail;
};

struct AutomountRuntimeStatus {
  bool loaded = false;
  std::vector<AutomountRuntimeMount> mounts;
};

// %ProgramData%\xfswin\sessions.ini
std::filesystem::path DefaultAutomountRuntimeStatusPath();

// Atomically replaces the snapshot.  An empty list marks the service as no
// longer owning any mount; the CLI ignores a snapshot when the service is not
// running.
Status SaveAutomountRuntimeStatus(
    const std::filesystem::path& path,
    const std::vector<AutomountRuntimeMount>& mounts);

Result<AutomountRuntimeStatus> LoadAutomountRuntimeStatus(
    const std::filesystem::path& path);

Status InstallAutomountService(std::string* message);
// Explicit-path variants for callers that manage a service for a specific
// xfswin.exe; the wrappers below use the running process image.  The uninstall
// variant only removes a service that still points at that image.
Status InstallAutomountServiceAt(const std::filesystem::path& executable,
                                 std::string* message);
Status UninstallAutomountServiceAt(const std::filesystem::path& executable,
                                   std::string* message);
Status UninstallAutomountService(std::string* message);
Status StartAutomountService(std::string* message);
Status StopAutomountService(std::string* message);

// Runs the WinFsp service loop.  When the process was not started by the SCM
// this falls back to console mode (Ctrl+C stops), which is how the test suite
// exercises the real event loop.  Returns a process exit code.
int RunAutomountService();

}  // namespace xfs