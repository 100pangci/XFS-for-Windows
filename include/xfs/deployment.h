// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - REL-1 installer deployment contract.
//
// The product installer is the native per-machine Rust setup.  Setup itself
// never owns configuration: the %ProgramData%\xfswin directory (configuration,
// identity enrollment, sessions and maintenance records) is outside the
// installed product tree and survives upgrade and ordinary uninstall.
//
// The maintenance protocol uses protected state and a startup mutex:
//
//   fence                 %ProgramData%\xfswin\maintenance.fence
//                         While it exists, no new mount session may start.
//                         Every process that creates a mount session must
//                         check it under the startup mutex.
//   sessions\*.lease      Held with an exclusive byte-range lock for the whole
//                         life of a mount session.  A cleanup pass signals the
//                         owning session and waits for the lease to disappear;
//                         a stale lease (owner gone) is removed.
//   startup mutex         Serializes "check fence + create lease" against
//                         "create fence + scan sessions", so neither side can
//                         miss the other.
//   capture file          %ProgramData%\xfswin\installer\rollback.ini
//                         Original service context, written once by prepare
//                         and consumed by rollback.
//   apply marker          %ProgramData%\xfswin\installer\apply.state
//                         Written just before apply/remove changes the service.
//                         Rollback quiescence is a no-op without this marker.
//
// A lease only prevents file replacement on current Windows: an open handle
// does not block a rename unless it was opened without FILE_SHARE_DELETE.
// That is why the fence refuses new sessions and why the installer verifies
// that no session is live before it splits the install transaction.
//
// WinFsp runtime validation is not a registry-only check: the loader opens the
// official shared runtime, resolves FspVersion and the FSCTL entry points, and
// reads the version resource, so a DLL that merely exists under a plausible
// name cannot pass.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "xfs/error.h"

namespace xfs {

// ---------------------------------------------------------------------------
// Runtime WinFsp loader (WinFsp is a separately installed shared runtime;
// this product never carries an app-local WinFsp DLL).
// ---------------------------------------------------------------------------
struct WinFspRuntimeInfo {
  bool available = false;
  std::wstring dll_path;      // resolved module path
  std::string version;        // "2.1.25156" when readable
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t build = 0;
  // The build ships native x64 and native arm64 binaries and is linked
  // against the matching winfsp-x64/winfsp-a64 import library.
  std::wstring expected_dll;
  std::string diagnostic;
};

// Compile-time architecture expected by this binary ("x64", "arm64", "x86").
const char* RuntimeArchitecture();

// Loads the matching official WinFsp runtime and validates it.  The module
// stays loaded for the process lifetime: FspLoad/GetProcAddress resolve
// against the same module the validation saw.  Returns kNotFound when the
// runtime is absent and kUnsupportedFeature when the present runtime is not a
// supported 2.x build.
Status PreloadWinFspRuntime(WinFspRuntimeInfo* info = nullptr);

// Read-only probe.  Loads (and keeps) the runtime when it is present.
WinFspRuntimeInfo ProbeWinFspRuntime();

// A PE image must match this process's native architecture.  Returns false
// with a diagnostic when the file is missing, unreadable, not a PE image or
// built for a different machine type.  This is used by the installer to
// reject a package whose xfswin.exe is the wrong architecture before any
// service action; it does not load the image.
bool ValidateExecutableArchitecture(const std::filesystem::path& path,
                                    std::string* error);

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
struct DeploymentPaths {
  // %ProgramData%\xfswin
  std::filesystem::path root;
  // root\sessions  (leases; live session registry)
  std::filesystem::path sessions;
  // root\installer (captured rollback context, maintenance records)
  std::filesystem::path installer;
  // root\maintenance.fence
  std::filesystem::path fence;
  // root\automount.ini and root\sessions.ini stay where UX-1 put them; the
  // deployment layer never rewrites them.
};

// %ProgramData%\xfswin.  There is no environment or command-line override in
// production: the fence, the session leases and the installer capture must
// always name the same machine-wide state the LocalSystem service observes.
// Resolves the machine known folder instead of reading a process environment
// variable. Returns empty paths when Windows cannot resolve it; maintenance
// and mount startup reject those paths. Isolated tests pass a private root
// explicitly through ResolveDeploymentPaths() / InstallerOptions::state_root.
DeploymentPaths DefaultDeploymentPaths();
DeploymentPaths ResolveDeploymentPaths(const std::filesystem::path& root);

// Creates the directories with an ACL that grants only LocalSystem and the
// local Administrators group write access (untrusted users must not be able
// to plant fences or fake leases).  An existing directory with a weaker ACL
// is tightened; failure to tighten is an error.
Status EnsureDeploymentPaths(const DeploymentPaths& paths);

// ---------------------------------------------------------------------------
// Maintenance fence and mount-session leases
// ---------------------------------------------------------------------------
struct MaintenanceFenceInfo {
  bool exists = false;
  bool readable = true;
  std::string operation;  // install | upgrade | uninstall | rollback
  std::string install_path;
  uint32_t pid = 0;
};

Status ReadMaintenanceFence(const DeploymentPaths& paths,
                            MaintenanceFenceInfo* info);

// True when new mount sessions must be refused.  An unreadable fence is
// treated as active (fail closed).
bool MaintenanceFenceActive(const DeploymentPaths& paths);

// True while this image's distro directories may still see a session.  Never
// writes; the installer's exact check is CheckActiveSessions.
struct MountSessionRecord {
  std::string mount_point;   // "X:" or "D:\path"
  uint32_t pid = 0;
  std::string executable;    // full image path of the owning process
  std::string started;       // UTC timestamp recorded by the owner
  std::string session_file;
  // True only when the SCM reports this pid as the running automount service.
  // An image path that merely equals our installed xfswin.exe is not proof of
  // service ownership: a manual mount runs the same image.
  bool owned_by_service = false;
};

struct ActiveSessionScan {
  std::vector<MountSessionRecord> sessions;
  std::vector<std::string> warnings;  // stale leases removed, unreadable files
};

// Scans the session lease directory.  A lease is live only when the owning
// process still holds its byte-range lock.  Service ownership is decided from
// the SCM-reported service PID, never from a matching executable path.  Stale
// files are removed.  Holds the startup mutex so a concurrent mount start
// cannot slip between the scan and the caller's decision.
Result<ActiveSessionScan> ScanActiveSessions(const DeploymentPaths& paths,
                                             const std::wstring& expected_exe);

// Installs the in-process unmount signal used by StopMountSessions.  Only the
// process that owns a mount session can signal it, so an external installer
// relies on the service stop for service-owned sessions and on the fence to
// refuse new manual sessions; the hook exists for in-process callers and
// tests.  Passing nullptr clears the hook.
void SetMountUnmountHook(Status (*hook)(const std::wstring& mount_point));

// Signals the mount session named in `records` to unmount and waits (bounded)
// for its lease to disappear.  Returns a list of sessions that did not stop.
struct SessionStopResult {
  bool all_stopped = true;
  std::vector<std::string> remaining;   // "mount-point (pid N): reason"
  std::vector<std::string> mounted;     // drive roots that still resolve
};
Status StopMountSessions(const DeploymentPaths& paths,
                         const std::wstring& expected_exe,
                         const std::vector<MountSessionRecord>& records,
                         uint32_t timeout_ms, SessionStopResult* result,
                         std::vector<std::string>* recoverable_detail);

// A guard held for the whole life of one mount session.  Acquire() takes the
// shared startup mutex, refuses when the maintenance fence exists, and creates
// the locked lease before releasing the mutex.  Releasing happens in the
// destructor after the session's clean unmount attempt, so an installer can
// never observe a released lease while the drive letter still exists.
class MountStartGate {
 public:
  MountStartGate();
  ~MountStartGate();
  MountStartGate(const MountStartGate&) = delete;
  MountStartGate& operator=(const MountStartGate&) = delete;
  MountStartGate(MountStartGate&&) = delete;
  MountStartGate& operator=(MountStartGate&&) = delete;

  // Empty paths are refused: product callers always pass the machine-wide
  // DefaultDeploymentPaths(). `mount_point` is "X:" or an absolute directory;
  // `owner_executable` identifies the process image in the lease.
  Status Acquire(const DeploymentPaths& paths, const std::wstring& mount_point,
                 const std::wstring& owner_executable);
  void Release();
  bool held() const { return lease_handle_ != nullptr; }
  const std::string& session_file() const { return session_file_; }

 private:
  void* lease_handle_ = nullptr;     // HANDLE
  void* mutex_handle_ = nullptr;     // HANDLE (startup mutex)
  std::string session_file_;
  DeploymentPaths paths_;
};

// ---------------------------------------------------------------------------
// Service registry operations for the installer
// ---------------------------------------------------------------------------
enum class InstallerServiceState : uint8_t {
  kNotInstalled = 0,
  kStopped,
  kStartPending,
  kStopPending,
  kRunning,
  kPaused,
  kUnknown,
};

const char* ToString(InstallerServiceState state);

struct InstallerServiceConfig {
  bool installed = false;
  std::wstring name;
  std::wstring display_name;
  std::wstring binary_path;      // configured binary (no arguments)
  std::wstring raw_image_path;   // full SCM ImagePath
  uint32_t start_type = 0;       // SERVICE_AUTO_START etc.
  bool delayed_auto_start = false;
  std::wstring account;
  std::vector<std::wstring> dependencies;
  bool running = false;
  bool stop_pending = false;
  bool start_pending = false;
  InstallerServiceState state = InstallerServiceState::kNotInstalled;
  std::string detail;            // last query / exit code detail
};

Status QueryInstallerService(InstallerServiceConfig* config);
// Creates the service at `binary_path` or updates its image path, error
// control and (when requested) the description.  Ownership is validated by
// the caller: an existing service must already be this product's.
Status EnsureInstallerService(const std::wstring& binary_path,
                              const std::wstring& display_name,
                              InstallerServiceConfig* config);
Status StartInstallerService(std::string* message);
Status StopInstallerService(std::string* message);
Status DeleteInstallerService(bool* pending);
// Waits until the SCM no longer reports the service (deletion completes
// asynchronously).  Returns ok once the service is gone, including when it
// was already absent.
Status WaitInstallerServiceDeleted(uint32_t timeout_ms);

// Parses an SCM ImagePath ("\"C:\...\xfswin.exe\" automount run") into the
// executable path.  Returns an empty string when it cannot be parsed.
std::wstring ParseServiceImagePath(const std::wstring& image_path);

// Writes a durable record of clean-stop / drive-letter-restore failures so
// the installer can report them after the service process is gone.  The file
// is %ProgramData%\xfswin\installer\shutdown.ini and is never deleted by
// upgrade/uninstall.
Status SaveShutdownFailureRecord(const DeploymentPaths& paths,
                                 const std::vector<std::string>& failures);

// ---------------------------------------------------------------------------
// Installer transaction state
// ---------------------------------------------------------------------------
enum class InstallerAction : uint8_t {
  kInstall = 0,
  kUpgrade,
  kUninstall,
};

const char* ToString(InstallerAction action);
bool ParseInstallerAction(const std::string& text, InstallerAction* action);

struct InstallerServiceSnapshot {
  bool installed = false;
  bool running = false;
  std::wstring binary_path;
  uint32_t start_type = 0;
  bool delayed_auto_start = false;
  std::wstring account;
  std::vector<std::wstring> dependencies;
};

struct InstallerCapture {
  uint32_t format = 1;
  InstallerAction action = InstallerAction::kInstall;
  std::string install_path;
  std::string product_version;
  std::string architecture;
  std::string created_utc;
  InstallerServiceSnapshot original_service;
  // For a virtualized run the execution context could not be captured; the
  // installer must then refuse an upgrade that cannot be undone.
  bool service_context_captured = false;
  std::vector<std::string> warnings;
};

struct InstallerOptions {
  InstallerAction action = InstallerAction::kInstall;
  std::filesystem::path install_path;   // absolute installed xfswin.exe
  std::filesystem::path state_root;     // empty = DefaultDeploymentPaths()
  // Installer-only failure injection (the acceptance test setup's /test-fail
  // flag passes this through): "before-service", "after-files",
  // "service-start", "verify" or empty.  "verify" makes the standalone verify
  // action fail after a successful apply, so setup rollback is exercised with
  // the new service already running.
  std::string injected_failure;
  // A fresh install may verify before the service exists; upgrade/uninstall
  // verification requires it unless this is set.
  bool allow_service_absent = false;
  uint32_t stop_timeout_ms = 30000;
  // The setup runs rollback and commit after its transaction branch has been
  // decided.  With this set (--action auto), rollback and commit take their
  // install/upgrade/uninstall operation from the fence and capture that
  // prepare wrote instead of trusting the verb's own action.
  bool use_captured_action = false;
};

struct InstallerResult {
  bool ok = false;
  Status error;
  InstallerAction action = InstallerAction::kInstall;
  std::string operation;          // install | upgrade | uninstall
  std::string install_path;
  std::string product_version;
  std::string architecture;
  int64_t exit_code = 0;
  std::vector<std::string> messages;
  std::vector<std::string> warnings;
  std::vector<std::string> blockers;
  InstallerServiceConfig service;
  WinFspRuntimeInfo winfsp;
  std::vector<MountSessionRecord> stopped_sessions;
  std::vector<std::string> remaining_sessions;
  std::filesystem::path state_file;
  std::filesystem::path fence_file;
  std::filesystem::path recoverable_record;
  bool service_created = false;
  bool service_updated = false;
  bool service_absent_allowed = false;
  // "stable-after-rollback", "restoration-pending", ... (informational).
  std::string restore_state;
};

// Validates and normalizes the install path (absolute, file name xfswin.exe).
// Missing file is not an error here: prepare runs before InstallFiles.
Result<std::filesystem::path> NormalizeInstallPath(
    const std::filesystem::path& path);

// Queries the installed version.json next to xfswin.exe.  Returns empty
// fields when absent; never scans disks.
struct InstalledVersionInfo {
  bool present = false;
  std::string version;
  std::string git_sha;
  std::string architecture;
};
InstalledVersionInfo ReadInstalledVersion(const std::filesystem::path& exe);

// The six agreed installer operations.  All are safe to call repeatedly at
// their protocol point; all report failure through the Status, and none
// touches ProgramData configuration, identity or RW enrollment.
InstallerResult PrepareInstaller(const InstallerOptions& options);
InstallerResult ApplyInstaller(const InstallerOptions& options);
InstallerResult RemoveInstaller(const InstallerOptions& options);
InstallerResult VerifyInstaller(const InstallerOptions& options);
InstallerResult CommitInstaller(const InstallerOptions& options);
InstallerResult RollbackInstaller(const InstallerOptions& options);

// Rollback stop phase used by the setup rollback sequence.  It executes before
// the setup restores the previous files, so this operation clean-stops the
// transaction-owned automount service (the only way to release a running
// xfswin.exe image) while leaving the captured rollback state and the fence
// untouched.  RollbackInstaller then runs after the files are back and
// restores the captured SCM state.  With no
// active fence there is nothing to stop and the operation succeeds.  It also
// succeeds without touching the service when the transaction never reached
// apply/remove: prepare writes the fence and capture before it refuses an
// upgrade with live mounts, and that refusal must leave the original service
// running.  ApplyInstaller and RemoveInstaller write installer\apply.state
// immediately before their first SCM change; that marker is the gate.
InstallerResult QuiesceInstaller(const InstallerOptions& options);

// Text/JSON rendering for the CLI.  `verb` is one of prepare|apply|remove|
// verify|commit|rollback and is included in the JSON document.
std::string FormatInstallerResult(const InstallerResult& result,
                                  const std::string& verb, bool json);

}  // namespace xfs
