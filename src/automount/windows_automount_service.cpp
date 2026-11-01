// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - UX-1 resident automount service.

#ifndef WINVER
#define WINVER 0x0602
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "xfs/automount_service.h"

// WinFsp 2.1 uses the WDK PNTSTATUS type, which the user-mode Windows SDK does
// not define.  Mirror the adapter's include order: no status macros from
// windows.h, then ntstatus.h.  cfgmgr32/winsvc are ordinary user-mode SDK
// headers and can be included in between.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
#include <ntstatus.h>
#include <cfgmgr32.h>
#include <winsvc.h>
typedef NTSTATUS* PNTSTATUS;

#include <winfsp/winfsp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "winfsp/winfsp_adapter.h"
#include "xfs/automount_config.h"
#include "xfs/automount_engine.h"
#include "xfs/device_discovery.h"
#include "xfs/filesystem.h"
#include "xfs/partition_device.h"
#include "xfs/uuid.h"
#include "xfs/windows_raw_device.h"
#include "xfs/windows_volume.h"

namespace xfs {
namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr wchar_t kServiceName[] = L"XFSWinAutomount";
constexpr wchar_t kServiceDisplayName[] = L"XFS for Windows automount";
constexpr wchar_t kServiceDescription[] =
    L"Discovers attached XFS filesystems and mounts configured filesystems "
    L"under a free drive letter. Read-only unless a filesystem was explicitly "
    L"enrolled for qualified experimental read-write access.";

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
  std::string out(static_cast<size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), length, nullptr, nullptr);
  return out;
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0);
  std::wstring out(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), length);
  return out;
}

std::wstring Win32ErrorText(DWORD code) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring text = L"error " + std::to_wstring(code);
  if (length != 0 && buffer) {
    text += L": ";
    text.append(buffer, length);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                             text.back() == L' ')) {
      text.pop_back();
    }
  }
  if (buffer) LocalFree(buffer);
  return text;
}

void ServiceLog(ULONG type, const std::wstring& text) {
  // FspServiceLog follows the wsprintf signature, which predates const.
  FspServiceLog(type, const_cast<PWSTR>(L"%s"), text.c_str());
}

Status ServiceFailure(const char* operation, DWORD code,
                      const std::wstring& context = {}) {
  std::string message = operation;
  message += " failed";
  if (!context.empty()) message += " for " + WideToUtf8(context);
  message += ": " + WideToUtf8(Win32ErrorText(code));
  const XfsError kind = code == ERROR_ACCESS_DENIED ? XfsError::kPermissionDenied
                                                    : XfsError::kIoError;
  return XFS_ERR(kind, std::move(message)).Metadata("automount-service");
}

struct ScmHandle {
  SC_HANDLE handle = nullptr;
  ~ScmHandle() {
    if (handle) CloseServiceHandle(handle);
  }
};

class ServiceHandle {
 public:
  ServiceHandle() = default;
  ServiceHandle(SC_HANDLE handle) : handle_(handle) {}
  ~ServiceHandle() { reset(); }
  ServiceHandle(const ServiceHandle&) = delete;
  ServiceHandle& operator=(const ServiceHandle&) = delete;
  ServiceHandle(ServiceHandle&& other) noexcept
      : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  ServiceHandle& operator=(ServiceHandle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  SC_HANDLE get() const { return handle_; }
  explicit operator bool() const { return handle_ != nullptr; }
  void reset() {
    if (handle_) CloseServiceHandle(handle_);
    handle_ = nullptr;
  }

 private:
  SC_HANDLE handle_ = nullptr;
};

std::wstring ExecutablePath() {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD copied = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0) return {};
    if (copied < buffer.size() - 1) {
      return std::wstring(buffer.data(), copied);
    }
    if (buffer.size() >= 32768) return {};
    buffer.resize(buffer.size() * 2);
  }
}

bool QueryServiceState(SC_HANDLE service, SERVICE_STATUS_PROCESS* status,
                       std::string* error) {
  DWORD needed = 0;
  if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                            reinterpret_cast<LPBYTE>(status),
                            sizeof(*status), &needed)) {
    if (error) *error = WideToUtf8(Win32ErrorText(GetLastError()));
    return false;
  }
  return true;
}

bool WaitForState(SC_HANDLE service, DWORD desired, DWORD timeout_ms,
                  std::string* error) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    SERVICE_STATUS_PROCESS status{};
    if (!QueryServiceState(service, &status, error)) return false;
    if (status.dwCurrentState == desired) return true;
    if (desired == SERVICE_RUNNING &&
        status.dwCurrentState == SERVICE_STOPPED) {
      if (error) {
        *error = "the service stopped while starting (Win32 exit code " +
                 std::to_string(status.dwWin32ExitCode) + ")";
      }
      return false;
    }
    if (desired == SERVICE_STOPPED &&
        status.dwCurrentState == SERVICE_STOPPED) {
      return true;
    }
    if (GetTickCount64() >= deadline) {
      if (error) *error = "timed out waiting for the service to change state";
      return false;
    }
    Sleep(100);
  }
}

std::string FormatLogText(const AutomountLogRecord& record) {
  auto clean = [](std::string text) {
    for (char& c : text) {
      if (c == '\r' || c == '\n') c = ' ';
    }
    return text;
  };
  std::string line = "event=" + clean(record.event);
  if (!record.uuid.empty()) line += " uuid=" + clean(record.uuid);
  if (!record.drive.empty()) line += " drive=" + clean(record.drive);
  if (!record.device.empty()) line += " device=" + clean(record.device);
  if (!record.result.empty()) line += " result=" + clean(record.result);
  if (!record.detail.empty()) {
    line += " detail=\"" + clean(record.detail) + "\"";
  }
  return line;
}

// ---------------------------------------------------------------------------
// Device notification registration
// ---------------------------------------------------------------------------

const GUID kDiskInterfaceClass = {
    0x53F56307,
    0xB6BF,
    0x11D0,
    {0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B}};
const GUID kVolumeInterfaceClass = {
    0x53F5630D,
    0xB6BF,
    0x11D0,
    {0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B}};

struct ServiceRuntime;

// ---------------------------------------------------------------------------
// Windows platform: discovery, drive letters and WinFsp mount sessions
// ---------------------------------------------------------------------------

bool OpenErrorMayBeTransient(const std::string& error) {
  // The raw-device open reports the Win32 code in its message.  Only a device
  // that is not ready yet is retryable; a filesystem or policy error never is.
  // ERROR_NOT_READY (21), ERROR_DEVICE_NOT_CONNECTED (1167) and the two
  // missing-path codes occur while a just-inserted USB device is still being
  // enumerated.
  return error.find("error 21:") != std::string::npos ||
         error.find("error 1167:") != std::string::npos ||
         error.find("error 2:") != std::string::npos ||
         error.find("error 3:") != std::string::npos;
}

class WindowsAutomountPlatform final : public AutomountPlatform {
 public:
  WindowsAutomountPlatform() = default;
  ~WindowsAutomountPlatform() override {
    std::map<std::string, std::unique_ptr<winfsp::MountSession>> sessions;
    {
      std::lock_guard<std::mutex> lock(session_mutex_);
      sessions.swap(sessions_);
    }
    for (auto& entry : sessions) {
      if (entry.second) entry.second->Stop();
    }
  }

  Result<DiscoveryReport> Discover(
      const DiscoveryOptions& options) override {
    std::string error;
    auto report = EnumerateXfsFilesystems(options, &error);
    if (!report.ok()) {
      return XFS_ERR(XfsError::kIoError,
                     error.empty() ? report.error().message : error);
    }
    return std::move(report.value());
  }

  std::vector<DriveLetterInfo> ListDriveLetters(std::string* error) override {
    return EnumerateDriveLetters(error);
  }

  AutomountMountStart StartMount(const DiscoveredFilesystem& filesystem,
                                 const AutomountEntry& entry,
                                 const std::string& drive) override {
    AutomountMountStart result;
    if (filesystem.location.disk_path.empty()) {
      result.status = XFS_ERR(XfsError::kInvalidArgument,
                              "the discovered filesystem has no raw device path");
      return result;
    }
    const bool read_write =
        entry.mode == AutomountMode::kExperimentalReadWrite;

    // DEV-1: the entry's explicit qualification and identity pins are the only
    // way an automount session may write.  Fixed disks are never writable, and
    // a removable disk still needs the explicit class opt-in.
    DeviceOpenPolicy policy;
    policy.writable = read_write;
    policy.exclusive = read_write;
    policy.require_volume_ownership = read_write;
    policy.allow_virtual_disk = true;
    policy.allow_removable_disk = entry.allow_removable_disk;
    policy.allow_fixed_disk = false;
    policy.qualification_override =
        read_write && entry.device_qualification;
    policy.expected_serial = entry.expected_serial;
    policy.expected_stable_id = entry.expected_stable_id;
    policy.expected_size_bytes = entry.expected_size_bytes;
    policy.cache_mode = read_write ? DeviceCacheMode::kNoBuffering
                                   : DeviceCacheMode::kBuffered;

    std::string error;
    auto device = OpenRawDevice(Utf8ToWide(filesystem.location.disk_path),
                                policy, &error);
    if (!device) {
      result.retryable = OpenErrorMayBeTransient(error);
      result.status = XFS_ERR(XfsError::kIoError,
                              "cannot open the XFS device: " + error);
      return result;
    }

    if (!filesystem.location.whole_disk &&
        (filesystem.location.offset_bytes != 0 ||
         filesystem.location.length_bytes != 0)) {
      uint64_t length = filesystem.location.length_bytes;
      if (length == 0) {
        if (filesystem.location.offset_bytes >= device->Size()) {
          result.status = XFS_ERR(XfsError::kOutOfRange,
                                  "the XFS partition starts outside the device");
          return result;
        }
        length = device->Size() - filesystem.location.offset_bytes;
      }
      auto view = OffsetBlockDevice::Create(
          std::move(device), filesystem.location.offset_bytes, length);
      if (!view.ok()) {
        result.status = view.error();
        return result;
      }
      device = std::move(view.value());
    }

    MountOptions options;
    options.read_write = read_write;
    options.perf_stats = false;
    auto mounted = XfsFilesystem::Mount(std::move(device), options);
    if (!mounted.ok()) {
      // A filesystem/feature/log error is never a device-not-ready retry.
      result.status = mounted.error();
      return result;
    }

    winfsp::MountConfig config;
    config.mount_point = Utf8ToWide(drive);
    config.read_write = read_write;
    config.verbose = false;
    config.perf_stats = false;
    auto session = winfsp::MountSession::Start(std::move(mounted.value()),
                                               config);
    if (!session.ok()) {
      result.status = session.error();
      return result;
    }
    const std::string id = session.value()->id();
    {
      std::lock_guard<std::mutex> lock(session_mutex_);
      sessions_[id] = std::move(session.value());
    }
    result.session_id = id;
    return result;
  }

  Status StopMount(const std::string& session_id) override {
    std::unique_ptr<winfsp::MountSession> session;
    {
      std::lock_guard<std::mutex> lock(session_mutex_);
      const auto it = sessions_.find(session_id);
      if (it == sessions_.end()) return OkStatus();  // already stopped
      session = std::move(it->second);
      sessions_.erase(it);
    }
    const Status stopped = session->Stop();
    if (!stopped.ok()) {
      // The mount point is released even when the durable unmount failed;
      // report the durability problem in the service log, not as a pending
      // drive letter that would be retried forever.
      AutomountLogRecord record;
      record.level = AutomountLogLevel::kWarn;
      record.event = "unmount";
      record.result = "durability-error";
      record.detail = stopped.error().Format();
      EmitLog(record);
    }
    return OkStatus();
  }

  bool IsSessionAlive(const std::string& session_id) override {
    std::lock_guard<std::mutex> lock(session_mutex_);
    const auto it = sessions_.find(session_id);
    return it != sessions_.end() && it->second && it->second->alive();
  }

  void EmitLog(const AutomountLogRecord& record) override {
    const std::wstring text = L"XFS automount: " +
                              Utf8ToWide(FormatLogText(record));
    if (record.level == AutomountLogLevel::kDebug) {
      OutputDebugStringW((text + L"\n").c_str());
      return;
    }
    ULONG type = EVENTLOG_INFORMATION_TYPE;
    if (record.level == AutomountLogLevel::kWarn) {
      type = EVENTLOG_WARNING_TYPE;
    } else if (record.level == AutomountLogLevel::kError) {
      type = EVENTLOG_ERROR_TYPE;
    }
    FspServiceLog(type, const_cast<PWSTR>(L"%s"), text.c_str());
  }

 private:
  std::mutex session_mutex_;
  std::map<std::string, std::unique_ptr<winfsp::MountSession>> sessions_;
};

// ---------------------------------------------------------------------------
// Service runtime
// ---------------------------------------------------------------------------

struct ServiceRuntime {
  ServiceRuntime() = default;
  ~ServiceRuntime() {
    if (stop_event && worker.joinable()) {
      SetEvent(stop_event);
      worker.join();
    }
    UnregisterNotifications();
    if (config_notify) {
      FindCloseChangeNotification(config_notify);
      config_notify = nullptr;
    }
    if (device_event) {
      CloseHandle(device_event);
      device_event = nullptr;
    }
    if (stop_event) {
      CloseHandle(stop_event);
      stop_event = nullptr;
    }
  }

  void UnregisterNotifications() {
    if (disk_notification) {
      CM_Unregister_Notification(disk_notification);
      disk_notification = nullptr;
    }
    if (volume_notification) {
      CM_Unregister_Notification(volume_notification);
      volume_notification = nullptr;
    }
  }

  HANDLE stop_event = nullptr;
  HANDLE device_event = nullptr;
  HANDLE config_notify = nullptr;
  HCMNOTIFICATION disk_notification = nullptr;
  HCMNOTIFICATION volume_notification = nullptr;
  AutomountServicePaths paths;
  std::unique_ptr<WindowsAutomountPlatform> platform;
  std::unique_ptr<AutomountEngine> engine;
  std::thread worker;
};

CONFIGRET RegisterInterfaceNotifications(const GUID& interface_class,
                                         ServiceRuntime& runtime,
                                         HCMNOTIFICATION* handle);

DWORD CALLBACK DeviceNotifyCallback(HCMNOTIFICATION, PVOID context,
                                    CM_NOTIFY_ACTION action,
                                    PCM_NOTIFY_EVENT_DATA, DWORD) {
  auto* runtime = static_cast<ServiceRuntime*>(context);
  if (!runtime) return ERROR_SUCCESS;
  switch (action) {
    case CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL:
    case CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL:
    case CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE:
      if (runtime->device_event) SetEvent(runtime->device_event);
      break;
    default:
      break;
  }
  return ERROR_SUCCESS;
}

CONFIGRET RegisterInterfaceNotifications(const GUID& interface_class,
                                         ServiceRuntime& runtime,
                                         HCMNOTIFICATION* handle) {
  CM_NOTIFY_FILTER filter{};
  filter.cbSize = sizeof(filter);
  filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
  filter.u.DeviceInterface.ClassGuid = interface_class;
  return CM_Register_Notification(&filter, &runtime, DeviceNotifyCallback,
                                  handle);
}

HANDLE CreateConfigWatcher(const std::filesystem::path& directory) {
  if (directory.empty()) return nullptr;
  return FindFirstChangeNotificationW(
      directory.wstring().c_str(), FALSE,
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
          FILE_NOTIFY_CHANGE_LAST_WRITE);
}

DWORD WaitTimeoutFor(std::optional<AutomountTimePoint> next,
                     AutomountTimePoint now) {
  if (!next) return INFINITE;
  if (*next <= now) return 0;
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(*next - now);
  const uint64_t milliseconds = static_cast<uint64_t>(remaining.count());
  // Leave INFINITE (0xFFFFFFFF) reserved for "no deadline".
  if (milliseconds >= 0xFFFFFFFEull) return 0xFFFFFFFD;
  return static_cast<DWORD>(std::max<uint64_t>(1, milliseconds));
}

void WorkerMain(ServiceRuntime& runtime) {
  enum class WakeKind : uint8_t { kStop, kDevice, kConfig };
  while (true) {
    runtime.engine->Tick(AutomountClock::now());
    const AutomountTimePoint now = AutomountClock::now();
    const DWORD timeout =
        WaitTimeoutFor(runtime.engine->NextWake(now), now);

    HANDLE handles[3] = {};
    WakeKind kinds[3] = {};
    DWORD count = 0;
    handles[count] = runtime.stop_event;
    kinds[count++] = WakeKind::kStop;
    if (runtime.device_event) {
      handles[count] = runtime.device_event;
      kinds[count++] = WakeKind::kDevice;
    }
    if (runtime.config_notify) {
      handles[count] = runtime.config_notify;
      kinds[count++] = WakeKind::kConfig;
    }

    const DWORD wake =
        WaitForMultipleObjects(count, handles, FALSE, timeout);
    if (wake == WAIT_FAILED) {
      AutomountLogRecord record;
      record.level = AutomountLogLevel::kError;
      record.event = "wait";
      record.result = "failed";
      record.detail = WideToUtf8(Win32ErrorText(GetLastError()));
      runtime.platform->EmitLog(record);
      WaitForSingleObject(runtime.stop_event, 1000);
      continue;
    }
    if (wake < WAIT_OBJECT_0 || wake >= WAIT_OBJECT_0 + count) {
      continue;  // timeout
    }
    const WakeKind kind = kinds[wake - WAIT_OBJECT_0];
    if (kind == WakeKind::kStop) break;
    const AutomountTimePoint event_time = AutomountClock::now();
    if (kind == WakeKind::kDevice) {
      ResetEvent(runtime.device_event);
      runtime.engine->NotifyDeviceChanged(event_time);
    } else if (kind == WakeKind::kConfig) {
      if (!FindNextChangeNotification(runtime.config_notify)) {
        const DWORD code = GetLastError();
        FindCloseChangeNotification(runtime.config_notify);
        runtime.config_notify = nullptr;
        AutomountLogRecord record;
        record.level = AutomountLogLevel::kWarn;
        record.event = "config-watch";
        record.result = "disabled";
        record.detail = WideToUtf8(Win32ErrorText(code));
        runtime.platform->EmitLog(record);
      }
      runtime.engine->NotifySettingsChanged(event_time);
    }
  }
  runtime.engine->Stop(AutomountClock::now());
}

NTSTATUS OnServiceStart(FSP_SERVICE* service, ULONG, PWSTR*) {
  auto runtime = std::make_unique<ServiceRuntime>();
  runtime->paths = DefaultAutomountServicePaths();
  const Status directory = EnsureAutomountServicePaths(runtime->paths);
  if (!directory.ok()) {
    ServiceLog(EVENTLOG_ERROR_TYPE,
               L"XFS automount: " +
                   Utf8ToWide(directory.error().Format()));
    return STATUS_ACCESS_DENIED;
  }

  runtime->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  runtime->device_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!runtime->stop_event || !runtime->device_event) {
    ServiceLog(EVENTLOG_ERROR_TYPE,
               L"XFS automount: cannot create wake events: " +
                   Win32ErrorText(GetLastError()));
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  runtime->platform = std::make_unique<WindowsAutomountPlatform>();
  AutomountSettings settings;
  runtime->engine = std::make_unique<AutomountEngine>(*runtime->platform,
                                                     settings);
  const std::filesystem::path config_file = runtime->paths.config_file;
  runtime->engine->SetConfigLoader(
      [config_file] { return LoadAutomountConfig(config_file); });

  CONFIGRET disk_result = RegisterInterfaceNotifications(
      kDiskInterfaceClass, *runtime, &runtime->disk_notification);
  CONFIGRET volume_result = RegisterInterfaceNotifications(
      kVolumeInterfaceClass, *runtime, &runtime->volume_notification);
  if (disk_result != CR_SUCCESS || volume_result != CR_SUCCESS) {
    AutomountLogRecord record;
    record.level = AutomountLogLevel::kWarn;
    record.event = "device-notifications";
    record.result = "partial";
    record.detail =
        "device notification registration failed (disk " +
        std::to_string(static_cast<uint32_t>(disk_result)) + ", volume " +
        std::to_string(static_cast<uint32_t>(volume_result)) +
        "); relying on the low-frequency reconciliation pass";
    runtime->platform->EmitLog(record);
  }
  runtime->config_notify = CreateConfigWatcher(runtime->paths.directory);

  runtime->engine->Start(AutomountClock::now());
  try {
    runtime->worker = std::thread(WorkerMain, std::ref(*runtime));
  } catch (...) {
    runtime->engine->Stop(AutomountClock::now());
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  const std::wstring config_file_wide =
      runtime->paths.config_file.wstring();
  service->UserContext = runtime.release();
  ServiceLog(EVENTLOG_INFORMATION_TYPE,
             L"XFS automount: service started; configuration " +
                 config_file_wide);
  return STATUS_SUCCESS;
}

NTSTATUS OnServiceStop(FSP_SERVICE* service) {
  auto* runtime = static_cast<ServiceRuntime*>(service->UserContext);
  if (!runtime) return STATUS_SUCCESS;
  // Unmounting several filesystems can take longer than the default stop
  // timeout; ask the SCM for more time before joining the worker.
  FspServiceRequestTime(service, 30000);
  SetEvent(runtime->stop_event);
  if (runtime->worker.joinable()) runtime->worker.join();
  runtime->engine->Stop(AutomountClock::now());
  runtime->UnregisterNotifications();
  delete runtime;
  service->UserContext = nullptr;
  ServiceLog(EVENTLOG_INFORMATION_TYPE, L"XFS automount: service stopped");
  return STATUS_SUCCESS;
}

NTSTATUS OnServiceControl(FSP_SERVICE* service, ULONG control, ULONG,
                          PVOID) {
  if (control == SERVICE_CONTROL_SHUTDOWN) {
    FspServiceStop(service);
  }
  return STATUS_SUCCESS;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public service API
// ---------------------------------------------------------------------------

AutomountServicePaths DefaultAutomountServicePaths() {
  AutomountServicePaths paths;
  paths.service_name = WideToUtf8(kServiceName);
  if (const wchar_t* override_path = _wgetenv(L"XFSWIN_AUTOMOUNT_CONFIG")) {
    if (*override_path != L'\0') {
      paths.config_file = std::filesystem::path(override_path);
      paths.directory = paths.config_file.parent_path();
      return paths;
    }
  }
  std::wstring program_data;
  wchar_t buffer[MAX_PATH + 1] = {};
  const DWORD length =
      GetEnvironmentVariableW(L"ProgramData", buffer, MAX_PATH + 1);
  if (length > 0 && length <= MAX_PATH) {
    program_data.assign(buffer, length);
  } else {
    program_data = L"C:\\ProgramData";
  }
  paths.directory = std::filesystem::path(program_data) / L"xfswin";
  paths.config_file = paths.directory / L"automount.ini";
  return paths;
}

Status EnsureAutomountServicePaths(const AutomountServicePaths& paths) {
  if (paths.directory.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "the automount configuration directory is empty");
  }
  std::error_code ec;
  std::filesystem::create_directories(paths.directory, ec);
  if (ec) {
    return XFS_ERR(XfsError::kIoError,
                   "cannot create the automount configuration directory " +
                       paths.directory.string() + ": " + ec.message());
  }
  return OkStatus();
}

const char* ToString(AutomountServiceState state) {
  switch (state) {
    case AutomountServiceState::kNotInstalled: return "not installed";
    case AutomountServiceState::kStopped: return "stopped";
    case AutomountServiceState::kStartPending: return "start pending";
    case AutomountServiceState::kStopPending: return "stop pending";
    case AutomountServiceState::kRunning: return "running";
    case AutomountServiceState::kPaused: return "paused";
    case AutomountServiceState::kUnknown: return "unknown";
  }
  return "unknown";
}

AutomountServiceStatusInfo QueryAutomountServiceStatus() {
  AutomountServiceStatusInfo result;
  ScmHandle scm;
  scm.handle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm.handle) {
    const DWORD code = GetLastError();
    result.state = AutomountServiceState::kUnknown;
    result.detail = WideToUtf8(Win32ErrorText(code));
    return result;
  }
  ServiceHandle service(OpenServiceW(scm.handle, kServiceName,
                                     SERVICE_QUERY_STATUS));
  if (!service) {
    const DWORD code = GetLastError();
    if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
      result.state = AutomountServiceState::kNotInstalled;
      result.detail.clear();
      return result;
    }
    result.state = AutomountServiceState::kUnknown;
    result.detail = WideToUtf8(Win32ErrorText(code));
    return result;
  }
  SERVICE_STATUS_PROCESS status{};
  std::string error;
  if (!QueryServiceState(service.get(), &status, &error)) {
    result.state = AutomountServiceState::kUnknown;
    result.detail = error;
    return result;
  }
  switch (status.dwCurrentState) {
    case SERVICE_STOPPED:
      result.state = AutomountServiceState::kStopped;
      if (status.dwWin32ExitCode != ERROR_SUCCESS) {
        result.detail = "last exit code " +
                        std::to_string(status.dwWin32ExitCode);
      }
      break;
    case SERVICE_START_PENDING:
      result.state = AutomountServiceState::kStartPending;
      break;
    case SERVICE_STOP_PENDING:
      result.state = AutomountServiceState::kStopPending;
      break;
    case SERVICE_RUNNING:
      result.state = AutomountServiceState::kRunning;
      break;
    case SERVICE_PAUSED:
      result.state = AutomountServiceState::kPaused;
      break;
    default:
      result.state = AutomountServiceState::kUnknown;
      break;
  }
  return result;
}

Status InstallAutomountService(std::string* message) {
  AutomountServicePaths paths = DefaultAutomountServicePaths();
  Status directory = EnsureAutomountServicePaths(paths);
  if (!directory.ok()) return directory;

  ScmHandle scm;
  scm.handle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (!scm.handle) return ServiceFailure("open the service manager", GetLastError());

  const std::wstring executable = ExecutablePath();
  if (executable.empty()) {
    return XFS_ERR(XfsError::kIoError,
                   "cannot determine the xfswin executable path")
        .Metadata("automount-service");
  }
  const std::wstring command =
      L"\"" + executable + L"\" automount run";
  ServiceHandle service(CreateServiceW(
      scm.handle, kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS,
      SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
      command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  if (!service) {
    const DWORD code = GetLastError();
    if (code == ERROR_SERVICE_EXISTS) {
      return XFS_ERR(XfsError::kAlreadyExists,
                     "the XFS automount service is already installed")
          .Metadata("automount-service");
    }
    return ServiceFailure("create the automount service", code, kServiceName);
  }
  SERVICE_DESCRIPTIONW description{};
  description.lpDescription = const_cast<LPWSTR>(kServiceDescription);
  ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION,
                        &description);
  if (message) {
    *message = "installed " + WideToUtf8(kServiceDisplayName) +
               " (automatic start); configuration: " +
               paths.config_file.string();
  }
  return OkStatus();
}

Status UninstallAutomountService(std::string* message) {
  ScmHandle scm;
  scm.handle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm.handle) return ServiceFailure("open the service manager", GetLastError());
  ServiceHandle service(OpenServiceW(
      scm.handle, kServiceName,
      DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS));
  if (!service) {
    const DWORD code = GetLastError();
    if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
      return XFS_ERR(XfsError::kNotFound,
                     "the XFS automount service is not installed")
          .Metadata("automount-service");
    }
    return ServiceFailure("open the automount service", code, kServiceName);
  }
  SERVICE_STATUS_PROCESS status{};
  std::string error;
  if (!QueryServiceState(service.get(), &status, &error)) {
    return XFS_ERR(XfsError::kIoError, error);
  }
  if (status.dwCurrentState != SERVICE_STOPPED) {
    SERVICE_STATUS ignored{};
    if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &ignored)) {
      const DWORD code = GetLastError();
      if (code != ERROR_SERVICE_NOT_ACTIVE) {
        return ServiceFailure("stop the automount service", code, kServiceName);
      }
    }
    if (!WaitForState(service.get(), SERVICE_STOPPED, 30000, &error)) {
      return XFS_ERR(XfsError::kIoError,
                     "the automount service did not stop: " + error);
    }
  }
  if (!DeleteService(service.get())) {
    const DWORD code = GetLastError();
    if (code != ERROR_SERVICE_MARKED_FOR_DELETE) {
      return ServiceFailure("delete the automount service", code, kServiceName);
    }
    if (message) *message = "the automount service removal is pending";
    return OkStatus();
  }
  if (message) {
    *message =
        "removed the XFS automount service; configuration was preserved";
  }
  return OkStatus();
}

Status StartAutomountService(std::string* message) {
  ScmHandle scm;
  scm.handle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm.handle) return ServiceFailure("open the service manager", GetLastError());
  ServiceHandle service(OpenServiceW(
      scm.handle, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS));
  if (!service) {
    const DWORD code = GetLastError();
    if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
      return XFS_ERR(XfsError::kNotFound,
                     "the XFS automount service is not installed; run "
                     "`xfswin automount install` first")
          .Metadata("automount-service");
    }
    return ServiceFailure("open the automount service", code, kServiceName);
  }
  SERVICE_STATUS_PROCESS status{};
  std::string error;
  if (!QueryServiceState(service.get(), &status, &error)) {
    return XFS_ERR(XfsError::kIoError, error);
  }
  if (status.dwCurrentState == SERVICE_RUNNING) {
    if (message) *message = "the automount service is already running";
    return OkStatus();
  }
  if (!StartServiceW(service.get(), 0, nullptr)) {
    const DWORD code = GetLastError();
    if (code != ERROR_SERVICE_ALREADY_RUNNING) {
      return ServiceFailure("start the automount service", code, kServiceName);
    }
  }
  if (!WaitForState(service.get(), SERVICE_RUNNING, 30000, &error)) {
    return XFS_ERR(XfsError::kIoError,
                   "the automount service did not start: " + error);
  }
  if (message) *message = "the automount service is running";
  return OkStatus();
}

Status StopAutomountService(std::string* message) {
  ScmHandle scm;
  scm.handle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm.handle) return ServiceFailure("open the service manager", GetLastError());
  ServiceHandle service(OpenServiceW(
      scm.handle, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS));
  if (!service) {
    const DWORD code = GetLastError();
    if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
      return XFS_ERR(XfsError::kNotFound,
                     "the XFS automount service is not installed")
          .Metadata("automount-service");
    }
    return ServiceFailure("open the automount service", code, kServiceName);
  }
  SERVICE_STATUS_PROCESS status{};
  std::string error;
  if (!QueryServiceState(service.get(), &status, &error)) {
    return XFS_ERR(XfsError::kIoError, error);
  }
  if (status.dwCurrentState == SERVICE_STOPPED) {
    if (message) *message = "the automount service is not running";
    return OkStatus();
  }
  SERVICE_STATUS ignored{};
  if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &ignored)) {
    const DWORD code = GetLastError();
    if (code != ERROR_SERVICE_NOT_ACTIVE) {
      return ServiceFailure("stop the automount service", code, kServiceName);
    }
  }
  if (!WaitForState(service.get(), SERVICE_STOPPED, 30000, &error)) {
    return XFS_ERR(XfsError::kIoError,
                   "the automount service did not stop: " + error);
  }
  if (message) *message = "the automount service is stopped";
  return OkStatus();
}

int RunAutomountService() {
  FSP_SERVICE* service = nullptr;
  NTSTATUS status =
      FspServiceCreate(const_cast<PWSTR>(kServiceName), OnServiceStart,
                       OnServiceStop, OnServiceControl, &service);
  if (status != STATUS_SUCCESS || !service) {
    return 1;
  }
  FspServiceAllowConsoleMode(service);
  FspServiceAcceptControl(service,
                          SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
  status = FspServiceLoop(service);
  const ULONG exit_code = FspServiceGetExitCode(service);
  FspServiceDelete(service);
  if (status != STATUS_SUCCESS) return 1;
  return static_cast<int>(exit_code);
}

}  // namespace xfs