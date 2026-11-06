// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// REL-1 deployment layer tests.  These exercise the maintenance fence, the
// session lease registry, the captured installer state and the rendering of
// installer results in an isolated temporary state root.  They never create,
// start or stop a Windows service and never open a device or volume.

#include "test_util.h"

#ifdef _WIN32

#include <windows.h>

#include <aclapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xfs/deployment.h"
#include "xfs/error.h"

namespace {

namespace fs = std::filesystem;
using xfs::DeploymentPaths;

class TempRoot {
 public:
  TempRoot() {
    static std::atomic<uint32_t> counter{0};
    path_ = fs::temp_directory_path() /
            ("xfswin-rel1-" +
             std::to_string(static_cast<unsigned long>(GetCurrentProcessId())) +
             "-" + std::to_string(++counter));
    std::error_code ec;
    fs::remove_all(path_, ec);
    fs::create_directories(path_);
  }
  ~TempRoot() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempRoot(const TempRoot&) = delete;
  TempRoot& operator=(const TempRoot&) = delete;

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

std::string ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}

void WriteFile(const fs::path& path, const std::string& text) {
  std::error_code ec;
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path(), ec);
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0);
  std::wstring out(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), length);
  return out;
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), length, nullptr, nullptr);
  return out;
}

DeploymentPaths FreshPaths(const TempRoot& root) {
  DeploymentPaths paths = xfs::ResolveDeploymentPaths(root.path());
  const xfs::Status ensured = xfs::EnsureDeploymentPaths(paths);
  CHECK(ensured.ok());
  return paths;
}

std::string FenceText(const std::string& operation,
                      const std::string& install_path) {
  return "xfswin-maintenance-fence 1\noperation=" + operation +
         "\ninstall_path=" + install_path + "\npid=4242\n";
}

std::string CaptureText(xfs::InstallerAction action,
                        const std::string& install_path) {
  std::ostringstream out;
  out << "xfswin-installer-state 1\n"
      << "state_version=1\n"
      << "action=" << xfs::ToString(action) << "\n"
      << "install_path=" << install_path << "\n"
      << "product_version=0.1.0\n"
      << "architecture=x64\n"
      << "created_utc=2026-01-01T00:00:00Z\n"
      << "service_context_captured=1\n"
      << "original_service_installed=0\n"
      << "original_service_running=0\n";
  return out.str();
}

xfs::InstallerOptions OptionsFor(xfs::InstallerAction action,
                                 const fs::path& exe, const fs::path& root) {
  xfs::InstallerOptions options;
  options.action = action;
  options.install_path = exe;
  options.state_root = root;
  return options;
}

void WriteSyntheticPe(const fs::path& path, uint16_t machine) {
  std::vector<uint8_t> image(0x200, 0);
  image[0] = 'M';
  image[1] = 'Z';
  const uint32_t pe_offset = 0x80;
  image[0x3C] = static_cast<uint8_t>(pe_offset & 0xFF);
  image[0x3D] = static_cast<uint8_t>((pe_offset >> 8) & 0xFF);
  image[0x3E] = static_cast<uint8_t>((pe_offset >> 16) & 0xFF);
  image[0x3F] = static_cast<uint8_t>((pe_offset >> 24) & 0xFF);
  image[pe_offset] = 'P';
  image[pe_offset + 1] = 'E';
  image[pe_offset + 2] = 0;
  image[pe_offset + 3] = 0;
  image[pe_offset + 4] = static_cast<uint8_t>(machine & 0xFF);
  image[pe_offset + 5] = static_cast<uint8_t>((machine >> 8) & 0xFF);
  std::error_code ec;
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path(), ec);
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(image.data()),
             static_cast<std::streamsize>(image.size()));
}

uint16_t NativeMachine() {
  const std::string native = xfs::RuntimeArchitecture();
  if (native == "arm64") return IMAGE_FILE_MACHINE_ARM64;
  if (native == "x86") return IMAGE_FILE_MACHINE_I386;
  return IMAGE_FILE_MACHINE_AMD64;
}

}  // namespace

TEST(rel1_fence_roundtrip_and_gate) {
  TempRoot root;
  DeploymentPaths paths = FreshPaths(root);
  const std::wstring owner = L"C:\\Program Files\\XFS-for-Windows\\xfswin.exe";

  xfs::MaintenanceFenceInfo info;
  CHECK(xfs::ReadMaintenanceFence(paths, &info).ok());
  CHECK(!info.exists);
  CHECK(!xfs::MaintenanceFenceActive(paths));

  WriteFile(paths.fence, FenceText("upgrade", "C:\\Apps\\xfswin.exe"));
  CHECK(xfs::ReadMaintenanceFence(paths, &info).ok());
  CHECK(info.exists);
  CHECK(info.readable);
  CHECK_STR_EQ(info.operation, "upgrade");
  CHECK_STR_EQ(info.install_path, "C:\\Apps\\xfswin.exe");
  CHECK_EQ(info.pid, 4242u);
  CHECK(xfs::MaintenanceFenceActive(paths));

  xfs::MountStartGate blocked;
  xfs::Status refused = blocked.Acquire(paths, L"X:", owner);
  CHECK(!refused.ok());
  CHECK_STR_EQ(refused.error().metadata, "maintenance-fence");
  CHECK(!blocked.held());
  CHECK(blocked.session_file().empty());

  std::error_code ec;
  fs::remove(paths.fence, ec);
  CHECK(!ec);
  CHECK(!xfs::MaintenanceFenceActive(paths));

  xfs::MountStartGate gate;
  CHECK(gate.Acquire(paths, L"X:", owner).ok());
  CHECK(gate.held());
  CHECK(!gate.session_file().empty());
  const std::string lease_name = gate.session_file();
  std::error_code lease_error;
  CHECK(fs::exists(paths.sessions / Wide(lease_name), lease_error));
  CHECK(!lease_error);

  auto scan = xfs::ScanActiveSessions(paths, owner);
  CHECK(scan.ok());
  CHECK_EQ(scan.value().sessions.size(), 1u);
  CHECK_STR_EQ(scan.value().sessions[0].mount_point, "X:");
  CHECK_EQ(scan.value().sessions[0].pid,
           static_cast<uint32_t>(GetCurrentProcessId()));
  // Ownership comes from the SCM-reported service PID.  The test process is
  // not the automount service even though the lease names the installed image.
  CHECK(!scan.value().sessions[0].owned_by_service);

  gate.Release();
  CHECK(!gate.held());
  CHECK(gate.session_file().empty());
  CHECK(!fs::exists(paths.sessions / Wide(lease_name), lease_error));

  auto after = xfs::ScanActiveSessions(paths, owner);
  CHECK(after.ok());
  CHECK(after.value().sessions.empty());
}

TEST(rel1_gate_disabled_for_empty_root) {
  xfs::MountStartGate gate;
  CHECK(gate.Acquire(xfs::DeploymentPaths{}, L"X:", L"xfswin.exe").ok());
  CHECK(!gate.held());
  gate.Release();
}

TEST(rel1_unreadable_fence_fails_closed) {
  TempRoot root;
  DeploymentPaths paths = FreshPaths(root);
  std::error_code ec;
  CHECK(fs::create_directories(paths.fence, ec));
  CHECK(!ec);

  xfs::MaintenanceFenceInfo info;
  xfs::Status read = xfs::ReadMaintenanceFence(paths, &info);
  CHECK(!read.ok());
  CHECK(info.exists);
  CHECK(!info.readable);
  CHECK(xfs::MaintenanceFenceActive(paths));

  xfs::MountStartGate gate;
  xfs::Status refused =
      gate.Acquire(paths, L"X:", L"C:\\Apps\\xfswin.exe");
  CHECK(!refused.ok());
  CHECK_STR_EQ(refused.error().metadata, "maintenance-fence");
  CHECK(!gate.held());

  fs::remove(paths.fence, ec);
  CHECK(!ec);
}

TEST(rel1_stale_lease_is_removed) {
  TempRoot root;
  DeploymentPaths paths = FreshPaths(root);
  const fs::path lease = paths.sessions / L"mount-9999-1-0.lease";
  WriteFile(lease,
            "xfswin-mount-lease 1\npid=9999\nmount_point=Y:\n"
            "executable=C:\\gone\\xfswin.exe\n");
  CHECK(fs::exists(lease));

  auto scan = xfs::ScanActiveSessions(paths, L"C:\\Apps\\xfswin.exe");
  CHECK(scan.ok());
  CHECK(scan.value().sessions.empty());
  CHECK(!scan.value().warnings.empty());
  CHECK(!fs::exists(lease));
}

TEST(rel1_stop_signals_and_reports_remaining_sessions) {
  TempRoot root;
  DeploymentPaths paths = FreshPaths(root);
  const std::wstring owner = L"C:\\Apps\\xfswin.exe";
  xfs::MountStartGate gate;
  CHECK(gate.Acquire(paths, L"X:", owner).ok());

  auto scan = xfs::ScanActiveSessions(paths, owner);
  CHECK(scan.ok());
  CHECK_EQ(scan.value().sessions.size(), 1u);
  // A lease created by this test process is a manual session: it must never be
  // treated as service-owned just because its image path matches.
  xfs::SessionStopResult manual;
  std::vector<std::string> manual_detail;
  xfs::Status manual_status = xfs::StopMountSessions(
      paths, owner, scan.value().sessions, 0, &manual, &manual_detail);
  CHECK(!manual_status.ok());
  CHECK(!manual.all_stopped);
  CHECK(!manual.remaining.empty());
  CHECK(manual.remaining.front().find("manual mount") != std::string::npos);

  // StopMountSessions only signals sessions explicitly marked as owned by the
  // SCM service; simulate that classification for the hook paths.
  std::vector<xfs::MountSessionRecord> owned = scan.value().sessions;
  owned[0].owned_by_service = true;

  xfs::SetMountUnmountHook(nullptr);
  xfs::SessionStopResult no_hook;
  std::vector<std::string> no_hook_detail;
  xfs::Status no_hook_status =
      xfs::StopMountSessions(paths, owner, owned, 0, &no_hook,
                             &no_hook_detail);
  CHECK(!no_hook_status.ok());
  CHECK(!no_hook.all_stopped);
  CHECK(!no_hook.remaining.empty());
  CHECK(!no_hook_detail.empty());

  xfs::SetMountUnmountHook(
      +[](const std::wstring&) { return xfs::OkStatus(); });
  xfs::SessionStopResult stuck;
  std::vector<std::string> stuck_detail;
  xfs::Status stuck_status =
      xfs::StopMountSessions(paths, owner, owned, 100, &stuck,
                             &stuck_detail);
  CHECK(!stuck_status.ok());
  CHECK(!stuck.all_stopped);
  CHECK(!stuck.remaining.empty());
  CHECK(!stuck_detail.empty());
  xfs::SetMountUnmountHook(nullptr);

  // The lease stays live because the fake hook does not release the gate.
  CHECK(gate.held());
  gate.Release();
}

TEST(rel1_parse_installer_action_and_names) {
  xfs::InstallerAction action = xfs::InstallerAction::kUninstall;
  CHECK(xfs::ParseInstallerAction("install", &action));
  CHECK(action == xfs::InstallerAction::kInstall);
  CHECK(xfs::ParseInstallerAction("upgrade", &action));
  CHECK(action == xfs::InstallerAction::kUpgrade);
  CHECK(xfs::ParseInstallerAction("uninstall", &action));
  CHECK(action == xfs::InstallerAction::kUninstall);
  CHECK(!xfs::ParseInstallerAction("Install", &action));
  CHECK(!xfs::ParseInstallerAction("", &action));
  CHECK(!xfs::ParseInstallerAction("remove", &action));
  CHECK(!xfs::ParseInstallerAction("install", nullptr));
  CHECK_STR_EQ(xfs::ToString(xfs::InstallerAction::kInstall), "install");
  CHECK_STR_EQ(xfs::ToString(xfs::InstallerAction::kUpgrade), "upgrade");
  CHECK_STR_EQ(xfs::ToString(xfs::InstallerAction::kUninstall), "uninstall");
}

TEST(rel1_parse_service_image_path) {
  CHECK_STR_EQ(
      Narrow(xfs::ParseServiceImagePath(
          L"\"C:\\Program Files\\XFS for Windows\\xfswin.exe\" automount run")),
      "C:\\Program Files\\XFS for Windows\\xfswin.exe");
  CHECK_STR_EQ(Narrow(xfs::ParseServiceImagePath(
                   L"C:\\Apps\\xfswin.exe automount run")),
               "C:\\Apps\\xfswin.exe");
  CHECK_STR_EQ(Narrow(xfs::ParseServiceImagePath(
                   L"   \"C:\\Apps\\xfswin.exe\"")),
               "C:\\Apps\\xfswin.exe");
  CHECK(xfs::ParseServiceImagePath(L"").empty());
  CHECK(xfs::ParseServiceImagePath(L"\"unterminated").empty());
}

TEST(rel1_normalize_install_path) {
  TempRoot root;
  const fs::path good = root.path() / "XFS-for-Windows" / "XFswin.EXE";
  auto normalized = xfs::NormalizeInstallPath(good);
  CHECK(normalized.ok());
  CHECK(normalized.value().is_absolute());
  CHECK(normalized.value().filename().wstring() == L"XFswin.EXE");

  const fs::path dotted =
      root.path() / "XFS-for-Windows" / "sub" / ".." / "xfswin.exe";
  auto dotted_result = xfs::NormalizeInstallPath(dotted);
  CHECK(dotted_result.ok());
  CHECK(dotted_result.value().parent_path().filename().wstring() ==
        L"XFS-for-Windows");

  auto relative = xfs::NormalizeInstallPath("xfswin.exe");
  CHECK(!relative.ok());
  CHECK(relative.error().code == xfs::XfsError::kInvalidArgument);

  auto other_name = xfs::NormalizeInstallPath(root.path() / "xfswin.com");
  CHECK(!other_name.ok());

  auto empty = xfs::NormalizeInstallPath(fs::path());
  CHECK(!empty.ok());
}

TEST(rel1_validate_executable_architecture) {
  TempRoot root;
  const fs::path good = root.path() / "xfswin.exe";
  WriteSyntheticPe(good, NativeMachine());
  std::string error = "unset";
  const std::string native = xfs::RuntimeArchitecture();
  if (native == "arm64ec") {
    // ARM64EC builds are not a supported delivery target; validation must
    // reject even a machine-matching image.
    CHECK(!xfs::ValidateExecutableArchitecture(good, &error));
    CHECK(!error.empty());
  } else {
    CHECK(xfs::ValidateExecutableArchitecture(good, &error));
    CHECK(error.empty());
  }

  const fs::path wrong = root.path() / "wrong.exe";
  const uint16_t other_machine = NativeMachine() == IMAGE_FILE_MACHINE_AMD64
                                     ? IMAGE_FILE_MACHINE_ARM64
                                     : IMAGE_FILE_MACHINE_AMD64;
  WriteSyntheticPe(wrong, other_machine);
  CHECK(!xfs::ValidateExecutableArchitecture(wrong, &error));
  CHECK(!error.empty());

  const fs::path not_pe = root.path() / "notpe.exe";
  WriteFile(not_pe, "not an executable");
  CHECK(!xfs::ValidateExecutableArchitecture(not_pe, &error));
  CHECK(!error.empty());

  CHECK(!xfs::ValidateExecutableArchitecture(root.path() / "missing.exe",
                                             &error));
  CHECK(!error.empty());
}

TEST(rel1_read_installed_version) {
  TempRoot root;
  const fs::path install = root.path() / "install";
  const fs::path exe = install / "xfswin.exe";
  WriteFile(exe, "");
  WriteFile(install / "version.json",
            "{\n"
            "  \"product\": \"XFS-for-Windows\",\n"
            "  \"version\": \"0.1.0\",\n"
            "  \"git_sha\": \"0123456789abcdef\",\n"
            "  \"architecture\": \"x64\",\n"
            "  \"production_build\": true\n"
            "}\n");

  const xfs::InstalledVersionInfo info = xfs::ReadInstalledVersion(exe);
  CHECK(info.present);
  CHECK_STR_EQ(info.version, "0.1.0");
  CHECK_STR_EQ(info.git_sha, "0123456789abcdef");
  CHECK_STR_EQ(info.architecture, "x64");

  const xfs::InstalledVersionInfo missing =
      xfs::ReadInstalledVersion(root.path() / "absent" / "xfswin.exe");
  CHECK(!missing.present);
  CHECK(missing.version.empty());
}

TEST(rel1_shutdown_failure_record) {
  TempRoot root;
  DeploymentPaths paths = FreshPaths(root);
  xfs::Status saved = xfs::SaveShutdownFailureRecord(
      paths, {"drive X: was not restored", "session Y: still mounted"});
  CHECK(saved.ok());
  const fs::path record = paths.installer / L"shutdown.ini";
  CHECK(fs::exists(record));
  const std::string text = ReadFile(record);
  CHECK(text.find("xfswin-shutdown 1") != std::string::npos);
  CHECK(text.find("failure=drive X: was not restored") != std::string::npos);
  CHECK(text.find("failure=session Y: still mounted") != std::string::npos);

  CHECK(!xfs::SaveShutdownFailureRecord(xfs::DeploymentPaths{}, {"x"}).ok());
}

TEST(rel1_commit_clears_matching_maintenance_state) {
  TempRoot root;
  DeploymentPaths paths = FreshPaths(root);
  const fs::path exe = root.path() / "xfswin.exe";
  WriteFile(paths.fence, FenceText("install", "C:\\Apps\\xfswin.exe"));
  WriteFile(paths.installer / L"rollback.ini",
            CaptureText(xfs::InstallerAction::kInstall, "C:\\Apps\\xfswin.exe"));

  xfs::InstallerResult result = xfs::CommitInstaller(
      OptionsFor(xfs::InstallerAction::kInstall, exe, root.path()));
  CHECK(result.ok);
  CHECK_EQ(result.exit_code, 0);
  CHECK_STR_EQ(result.restore_state, "stable");
  CHECK(!fs::exists(paths.fence));
  CHECK(!fs::exists(paths.installer / L"rollback.ini"));
}

TEST(rel1_commit_rejects_action_mismatch_and_keeps_fence) {
  TempRoot root;
  DeploymentPaths paths = FreshPaths(root);
  const fs::path exe = root.path() / "xfswin.exe";
  WriteFile(paths.fence, FenceText("upgrade", "C:\\Apps\\xfswin.exe"));
  WriteFile(paths.installer / L"rollback.ini",
            CaptureText(xfs::InstallerAction::kUpgrade,
                        "C:\\Apps\\xfswin.exe"));

  xfs::InstallerResult result = xfs::CommitInstaller(
      OptionsFor(xfs::InstallerAction::kInstall, exe, root.path()));
  CHECK(!result.ok);
  CHECK(result.error.error().code == xfs::XfsError::kBadValue);
  CHECK(fs::exists(paths.fence));
  CHECK(fs::exists(paths.installer / L"rollback.ini"));
}

TEST(rel1_commit_without_fence_removes_stray_state) {
  TempRoot root;
  DeploymentPaths paths = FreshPaths(root);
  const fs::path exe = root.path() / "xfswin.exe";
  WriteFile(paths.installer / L"rollback.ini",
            CaptureText(xfs::InstallerAction::kInstall, "C:\\Apps\\xfswin.exe"));

  xfs::InstallerResult result = xfs::CommitInstaller(
      OptionsFor(xfs::InstallerAction::kInstall, exe, root.path()));
  CHECK(result.ok);
  CHECK_STR_EQ(result.restore_state, "stable");
  CHECK(!fs::exists(paths.installer / L"rollback.ini"));
}

TEST(rel1_installer_result_text_and_json) {
  xfs::InstallerResult result;
  result.ok = true;
  result.action = xfs::InstallerAction::kUpgrade;
  result.operation = "upgrade";
  result.install_path = "C:\\Program Files\\XFS-for-Windows\\xfswin.exe";
  result.product_version = "0.1.0";
  result.architecture = "x64";
  result.exit_code = 0;
  result.restore_state = "stable";
  result.service.installed = true;
  result.service.running = true;
  result.service.start_type = 2;
  result.service.state = xfs::InstallerServiceState::kRunning;
  result.service.binary_path =
      L"C:\\Program Files\\XFS-for-Windows\\xfswin.exe";
  result.messages.push_back("service started");
  result.messages.push_back("line\nbreak");
  result.warnings.push_back("a warning");
  result.blockers.push_back("a blocker");
  result.remaining_sessions.push_back("X: (pid 7)");

  const std::string text = xfs::FormatInstallerResult(result, "prepare", false);
  CHECK(text.find("verb=prepare") != std::string::npos);
  CHECK(text.find("result=ok") != std::string::npos);
  CHECK(text.find("operation=upgrade") != std::string::npos);
  CHECK(text.find("install-path=C:\\Program Files") != std::string::npos);
  CHECK(text.find("service=running (running)") != std::string::npos);
  CHECK(text.find("service-binary=C:\\Program Files") != std::string::npos);
  CHECK(text.find("message=service started") != std::string::npos);
  CHECK(text.find("message=line") != std::string::npos);
  CHECK(text.find("warning=a warning") != std::string::npos);
  CHECK(text.find("blocker=a blocker") != std::string::npos);
  CHECK(text.find("remaining-session=X: (pid 7)") != std::string::npos);
  CHECK(text.find("restore-state=stable") != std::string::npos);
  CHECK(text.find("error=") == std::string::npos);

  const std::string json = xfs::FormatInstallerResult(result, "verify", true);
  CHECK(json.find("\"verb\":\"verify\"") != std::string::npos);
  CHECK(json.find("\"ok\":true") != std::string::npos);
  CHECK(json.find("\"action\":\"upgrade\"") != std::string::npos);
  CHECK(json.find("\"install_path\":\"C:\\\\Program Files\\\\"
                  "XFS-for-Windows\\\\xfswin.exe\"") != std::string::npos);
  CHECK(json.find("\"messages\":[\"service started\",\"line\\nbreak\"]") !=
        std::string::npos);
  CHECK(json.find("\"warnings\":[\"a warning\"]") != std::string::npos);
  CHECK(json.find("\"service\":{") != std::string::npos);
  CHECK(json.find("\"winfsp\":{") != std::string::npos);
  CHECK(json.find("\"error\":") == std::string::npos);

  xfs::InstallerResult failed;
  failed.action = xfs::InstallerAction::kInstall;
  failed.operation = "install";
  failed.exit_code = 3;
  failed.error =
      xfs::XFS_ERR(xfs::XfsError::kBadValue, "boom \"quoted\"").Metadata(
          "installer");
  const std::string failed_text =
      xfs::FormatInstallerResult(failed, "apply", false);
  CHECK(failed_text.find("result=failed") != std::string::npos);
  CHECK(failed_text.find("exit-code=3") != std::string::npos);
  CHECK(failed_text.find("error=") != std::string::npos);
  const std::string failed_json =
      xfs::FormatInstallerResult(failed, "apply", true);
  CHECK(failed_json.find("\"ok\":false") != std::string::npos);
  CHECK(failed_json.find("\"exit_code\":3") != std::string::npos);
  CHECK(failed_json.find("\"error\":{") != std::string::npos);
  CHECK(failed_json.find("boom \\\"quoted\\\"") != std::string::npos);
}

TEST(rel1_deployment_directories_use_protected_dacl) {
  TempRoot root;
  DeploymentPaths paths = FreshPaths(root);
  const fs::path directories[] = {paths.root, paths.sessions, paths.installer};
  for (const fs::path& directory : directories) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD queried = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(directory.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
        &descriptor);
    CHECK_EQ(queried, static_cast<DWORD>(ERROR_SUCCESS));
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    CHECK(GetSecurityDescriptorControl(descriptor, &control, &revision));
    // D:P in the SDDL must reach the object: inherited ACEs from a weaker
    // parent may not stay in effect.
    CHECK((control & SE_DACL_PROTECTED) != 0);
    CHECK((control & SE_DACL_PRESENT) != 0);
    LocalFree(descriptor);
  }
}

TEST(rel1_default_paths_ignore_environment_override) {
  TempRoot root;
  const wchar_t* name = L"XFSWIN_DEPLOYMENT_STATE_DIR";
  wchar_t previous[32768] = {};
  const DWORD previous_length = GetEnvironmentVariableW(name, previous, 32768);
  CHECK(SetEnvironmentVariableW(name, root.path().c_str()));
  const DeploymentPaths paths = xfs::DefaultDeploymentPaths();
  if (previous_length > 0 && previous_length < 32768) {
    SetEnvironmentVariableW(name, previous);
  } else {
    SetEnvironmentVariableW(name, nullptr);
  }
  // The production root is fixed at %ProgramData%\xfswin; a process-controlled
  // environment variable must not redirect the fence or the lease directory.
  CHECK(paths.root.filename().wstring() == L"xfswin");
  CHECK(paths.root != root.path());
  CHECK(paths.fence == paths.root / L"maintenance.fence");
  CHECK(paths.sessions == paths.root / L"sessions");
}

TEST(rel1_rollback_quiesce_without_transaction_is_noop) {
  TempRoot root;
  const fs::path exe = root.path() / "xfswin.exe";
  WriteFile(exe, "");
  xfs::InstallerResult result = xfs::QuiesceInstaller(
      OptionsFor(xfs::InstallerAction::kUpgrade, exe, root.path()));
  CHECK(result.ok);
  CHECK_EQ(result.exit_code, 0);
  CHECK_STR_EQ(result.restore_state, "stable");
  const DeploymentPaths paths = xfs::ResolveDeploymentPaths(root.path());
  CHECK(!fs::exists(paths.fence));
}

TEST(rel1_rollback_quiesce_ignores_prepare_only_fence) {
  TempRoot root;
  DeploymentPaths paths = FreshPaths(root);
  const fs::path exe = root.path() / "xfswin.exe";
  WriteFile(paths.fence, FenceText("upgrade", "C:\\Apps\\xfswin.exe"));
  WriteFile(paths.installer / L"rollback.ini",
            CaptureText(xfs::InstallerAction::kUpgrade,
                        "C:\\Apps\\xfswin.exe"));

  // Prepare writes the fence and capture before it refuses an upgrade with
  // live mounts.  The rollback stop action must not stop the original service
  // in that case: apply/remove never ran, so no apply marker exists.
  xfs::InstallerResult result = xfs::QuiesceInstaller(
      OptionsFor(xfs::InstallerAction::kUpgrade, exe, root.path()));
  CHECK(result.ok);
  CHECK_EQ(result.exit_code, 0);
  CHECK_STR_EQ(result.restore_state, "maintenance-active");
  CHECK(fs::exists(paths.fence));
  CHECK(fs::exists(paths.installer / L"rollback.ini"));
}

TEST(rel1_commit_with_captured_action_uses_the_fence) {
  TempRoot root;
  DeploymentPaths paths = FreshPaths(root);
  const fs::path exe = root.path() / "xfswin.exe";
  WriteFile(paths.fence, FenceText("upgrade", "C:\\Apps\\xfswin.exe"));
  WriteFile(paths.installer / L"rollback.ini",
            CaptureText(xfs::InstallerAction::kUpgrade,
                        "C:\\Apps\\xfswin.exe"));
  WriteFile(paths.installer / L"apply.state", "xfswin-installer-apply 1\n");

  // The MSI commit action passes --action auto; the helper must take the
  // operation from the fence/capture prepare wrote.  Commit finishes the
  // transaction, so it also clears the apply marker.
  xfs::InstallerOptions options =
      OptionsFor(xfs::InstallerAction::kInstall, exe, root.path());
  options.use_captured_action = true;
  xfs::InstallerResult result = xfs::CommitInstaller(options);
  CHECK(result.ok);
  CHECK(result.action == xfs::InstallerAction::kUpgrade);
  CHECK(!fs::exists(paths.fence));
  CHECK(!fs::exists(paths.installer / L"rollback.ini"));
  CHECK(!fs::exists(paths.installer / L"apply.state"));
}

#endif  // _WIN32
