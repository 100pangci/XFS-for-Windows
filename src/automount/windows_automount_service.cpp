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
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
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
    L"enrolled for qualified read-write access.";

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
  // that is not ready yet or briefly busy is retryable; a filesystem or policy
  // error never is.  ERROR_NOT_READY (21), ERROR_DEVICE_NOT_CONNECTED (1167)
  // and the two missing-path codes occur while a just-inserted USB device is
  // still being enumerated.  ERROR_SHARING_VIOLATION (32) occurs when a handle
  // that is closing (the service's own previous session, Explorer, or an
  // antivirus scan) still holds the disk exclusively; the next attempt is
  // usually accepted, and treating it as fatal would leave the Windows RAW
  // letter in place next to an unrelated allocation.
  return error.find("error 21:") != std::string::npos ||
         error.find("error 32:") != std::string::npos ||
         error.find("error 1167:") != std::string::npos ||
         error.find("error 2:") != std::string::npos ||
         error.find("error 3:") != std::string::npos;
}

bool SameTextIgnoreCase(const std::string& a, const std::string& b) {
  std::string left(a);
  std::string right(b);
  for (char& c : left) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  for (char& c : right) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return left == right;
}

std::string NormalizedVolumePath(std::string text) {
  while (!text.empty() && text.back() == '\\') text.pop_back();
  return text;
}

bool EqualsAsciiCaseInsensitive(const std::string& a, const char* b) {
  const size_t length = std::strlen(b);
  if (a.size() != length) return false;
  for (size_t i = 0; i < length; ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 32);
    if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 32);
    if (ca != cb) return false;
  }
  return true;
}

uint32_t XfsVolumeSerial(const XfsSuperblock& sb) {
  return static_cast<uint32_t>(sb.uuid[0] | (sb.uuid[1] << 8) |
                               (sb.uuid[2] << 16) |
                               (static_cast<uint32_t>(sb.uuid[3]) << 24));
}

// Failure injection for the Windows integration tests.  The service runs as
// LocalSystem, so a sentinel file in the administrator-only configuration
// directory is used instead of an environment variable that an SCM-started
// process would never inherit.
#ifdef XFSWIN_TEST_HOOKS
std::filesystem::path TestHookSentinel(const wchar_t* name) {
  wchar_t program_data[MAX_PATH + 1] = {};
  const DWORD length =
      GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH + 1);
  std::filesystem::path directory;
  if (length > 0 && length <= MAX_PATH) {
    directory = std::filesystem::path(program_data) / L"xfswin" / L"test-hooks";
  } else {
    directory = std::filesystem::path(L"C:\\ProgramData") / L"xfswin" /
                L"test-hooks";
  }
  return directory / name;
}
#endif

// One-shot variant: consumes the sentinel when it fires.
bool ConsumeTestHook(const wchar_t* name) {
#ifdef XFSWIN_TEST_HOOKS
  std::error_code ec;
  const std::filesystem::path sentinel = TestHookSentinel(name);
  if (!std::filesystem::exists(sentinel, ec)) return false;
  std::filesystem::remove(sentinel, ec);
  return true;
#else
  (void)name;
  return false;
#endif
}

// Persistent variant: every attempt fires while the sentinel exists.  The
// rollback test uses this so a device or reconciliation event cannot turn a
// failed-mount assertion into a successful mount behind the test's back.
bool TestHookPresent(const wchar_t* name) {
#ifdef XFSWIN_TEST_HOOKS
  std::error_code ec;
  return std::filesystem::exists(TestHookSentinel(name), ec);
#else
  (void)name;
  return false;
#endif
}

// ---------------------------------------------------------------------------
// Drive-letter takeover against live Windows state
// ---------------------------------------------------------------------------

class WindowsTakeoverOps final : public TakeoverOps {
 public:
  Status VerifyAndCapture(const DiscoveredFilesystem& filesystem,
                          std::string_view drive,
                          std::string_view release_drive,
                          std::string_view release_volume_device_path,
                          DriveLetterTakeover* takeover) override {
    if (!takeover) {
      return XFS_ERR(XfsError::kInvalidArgument, "missing takeover record");
    }
    auto normalized = NormalizeDriveLetter(release_drive);
    if (!normalized.ok()) return normalized.error();
    const std::string release = normalized.value();
    // The engine may pass the volume GUID it has just verified live for
    // `release_drive`.  A discovery pass that runs right after a restore can
    // see the Mount Manager's path list before it lists the letter again; the
    // snapshot correlation is then stale, but the volume itself is verified
    // below against live state either way.
    const std::string volume_path =
        release_volume_device_path.empty()
            ? filesystem.volume_device_path
            : std::string(release_volume_device_path);
    if (release_volume_device_path.empty() &&
        !filesystem.volume_identity_exact) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "the Windows volume is not an exact extent match")
          .Metadata("drive-letter takeover");
    }
    if (filesystem.probe.state != XfsProbeState::kValid ||
        IsNullUuid(filesystem.probe.superblock.uuid)) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "the XFS filesystem did not verify")
          .Metadata("drive-letter takeover");
    }

    std::string error;
    auto state = QueryVolumePathState(volume_path, &error);
    if (!state.ok()) {
      return XFS_ERR(state.error().code,
                     "the Windows volume disappeared before the takeover: " +
                         state.error().Format())
          .Metadata("drive-letter takeover");
    }
    const VolumePathState& volume = state.value();

    // Fail closed: only ERROR_UNRECOGNIZED_VOLUME made this volume a
    // candidate.  A recognized filesystem can appear between discovery and
    // takeover only if the volume was reformatted or replaced; never detach
    // its letter in either case.
    if (volume.has_filesystem || !volume.unrecognized_filesystem) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "the Windows volume no longer reports an unrecognized "
                     "filesystem")
          .Metadata("drive-letter takeover");
    }
    if (volume.extents.size() != 1) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "the Windows volume does not have exactly one disk extent")
          .Metadata("drive-letter takeover");
    }
    const VolumeExtent& extent = volume.extents.front();
    if (extent.disk_number != filesystem.location.disk_number ||
        extent.starting_offset != filesystem.location.offset_bytes ||
        extent.length_bytes != filesystem.location.length_bytes) {
      return XFS_ERR(XfsError::kBadValue,
                     "the Windows volume's disk extent no longer matches the "
                     "XFS candidate")
          .Metadata("drive-letter takeover");
    }

    std::vector<std::string> drive_roots;
    std::vector<std::string> directory_mounts;
    for (const auto& mount : volume.mount_points) {
      if (IsDriveLetterRoot(mount)) {
        drive_roots.push_back(mount);
      } else {
        directory_mounts.push_back(mount);
      }
    }
    if (drive_roots.size() != 1) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "the Windows volume does not have exactly one "
                     "drive-letter mount point")
          .Metadata("drive-letter takeover");
    }
    const std::string only = SingleDriveLetterMountPoint(drive_roots);
    if (only != release) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "drive " + release +
                         " is not the Windows volume's drive letter")
          .Metadata("drive-letter takeover");
    }
    auto reverse = QueryVolumeNameForMountPoint(release);
    if (!reverse.ok() ||
        !SameTextIgnoreCase(NormalizedVolumePath(reverse.value()),
                            NormalizedVolumePath(volume.volume_name))) {
      return XFS_ERR(XfsError::kBadValue,
                     "drive " + release +
                         " does not resolve back to the recorded volume GUID")
          .Metadata("drive-letter takeover");
    }

    takeover->drive = std::string(drive);
    takeover->mount_point = release + "\\";
    takeover->original_drive = release;
    takeover->volume_name = volume.volume_name;
    takeover->volume_device_path = volume.device_path;
    takeover->fs_uuid = FormatUuid(filesystem.probe.superblock.uuid);
    takeover->disk_number = extent.disk_number;
    takeover->partition_number = filesystem.location.partition_number;
    takeover->offset_bytes = extent.starting_offset;
    takeover->length_bytes = extent.length_bytes;
    takeover->volume_present = true;
    takeover->other_mount_points = std::move(directory_mounts);
    return OkStatus();
  }

  Status RemoveMountPoint(DriveLetterTakeover* takeover) override {
    if (!takeover) {
      return XFS_ERR(XfsError::kInvalidArgument, "missing takeover record");
    }
    std::string error;
    Status removed = RemoveDriveLetterMountPoint(takeover->mount_point, &error);
    if (!removed.ok()) {
      if (!error.empty()) takeover->error = error;
      return removed;
    }
    // DeleteVolumeMountPointW returns before every observer has refreshed its
    // DOS-device snapshot.  WinFsp refuses an occupied letter, so wait (bounded)
    // for the letter to disappear before the mount is attempted.
    if (!WaitForDriveLetterRelease(takeover->original_drive, 5000)) {
      return XFS_ERR(XfsError::kIoError,
                     "the Windows drive letter " + takeover->original_drive +
                         " is still listed after the mount point was removed")
          .Metadata("drive-letter takeover");
    }
    return OkStatus();
  }

  Status VerifyRemoved(const DriveLetterTakeover& takeover) override {
    std::string error;
    auto state = QueryVolumePathState(takeover.volume_device_path, &error);
    if (!state.ok()) {
      // The device was removed in the meantime.  The mount point cannot exist
      // and the later restore will skip re-creating it.
      return OkStatus();
    }
    for (const auto& mount : state.value().mount_points) {
      if (SameTextIgnoreCase(mount, takeover.mount_point)) {
        return XFS_ERR(XfsError::kAlreadyExists,
                       "the Windows mount point " + takeover.mount_point +
                           " is still attached to the volume")
            .Metadata("drive-letter takeover");
      }
    }
    if (!WaitForDriveLetterRelease(takeover.original_drive, 0)) {
      return XFS_ERR(XfsError::kAlreadyExists,
                     "the Windows drive letter " + takeover.original_drive +
                         " is still listed")
          .Metadata("drive-letter takeover");
    }
    return OkStatus();
  }

  bool VolumePresent(const DriveLetterTakeover& takeover) override {
    return IsVolumePresent(takeover.volume_device_path);
  }

  Status RestoreMountPoint(DriveLetterTakeover* takeover) override {
    if (!takeover) {
      return XFS_ERR(XfsError::kInvalidArgument, "missing takeover record");
    }
    std::string error;
    Status restored = RestoreDriveLetterMountPoint(
        takeover->mount_point, takeover->volume_name, &error);
    if (!restored.ok()) {
      if (!error.empty()) takeover->error = error;
      return restored;
    }
    return OkStatus();
  }

  Status VerifyRestored(const DriveLetterTakeover& takeover) override {
    std::string error;
    auto state = QueryVolumePathState(takeover.volume_device_path, &error);
    if (!state.ok()) {
      // The volume vanished between the restore and this check.  The restore
      // itself succeeded; there is nothing further to verify.
      return OkStatus();
    }
    for (const auto& mount : state.value().mount_points) {
      if (SameTextIgnoreCase(mount, takeover.mount_point)) return OkStatus();
    }
    return XFS_ERR(XfsError::kNotFound,
                   "the Windows mount point " + takeover.mount_point +
                       " is not present after the restore")
        .Metadata("drive-letter takeover");
  }
};

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

  bool DriveLetterResolvesToVolume(
      std::string_view drive,
      std::string_view volume_device_path) override {
    return xfs::DriveLetterResolvesToVolume(drive, volume_device_path);
  }

  TakeoverResult TakeOverDriveLetter(
      const DiscoveredFilesystem& filesystem,
      const DriveLetterChoice& choice) override {
    WindowsTakeoverOps ops;
    return PrepareDriveLetterTakeover(ops, filesystem, choice.drive,
                                      choice.release_drive,
                                      choice.release_volume_device_path);
  }

  Status RestoreDriveLetterTakeover(DriveLetterTakeover* takeover) override {
    WindowsTakeoverOps ops;
    return xfs::RestoreDriveLetterTakeover(ops, takeover);
  }

  AutomountMountStart StartMount(const DiscoveredFilesystem& filesystem,
                                 const AutomountEntry& entry,
                                 AutomountMode mode,
                                 const std::string& drive) override {
    AutomountMountStart result;
    if (filesystem.location.disk_path.empty()) {
      result.status = XFS_ERR(XfsError::kInvalidArgument,
                              "the discovered filesystem has no raw device path");
      return result;
    }
    const bool read_write = mode == AutomountMode::kReadWrite;
    if (read_write &&
        !UuidEqual(entry.fs_uuid, filesystem.probe.superblock.uuid)) {
      result.status = XFS_ERR(XfsError::kPermissionDenied,
                              "the enrolled filesystem UUID does not match the "
                              "discovered filesystem; refusing read-write");
      return result;
    }
    if (read_write && entry.expected_device_class != DeviceClass::kUnknown &&
        filesystem.location.device_class != entry.expected_device_class) {
      result.status = XFS_ERR(XfsError::kPermissionDenied,
                              "the attached device class does not match the "
                              "enrolled class; refusing read-write");
      return result;
    }
    if (read_write && filesystem.disk_has_other_filesystems) {
      result.status = XFS_ERR(XfsError::kPermissionDenied,
                              "another volume on the same physical disk has a "
                              "Windows filesystem; a read-write mount would "
                              "dismount it");
      return result;
    }

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
    // The identity pins are the DEV-1 read-write authorization: they bind the
    // enrolled device to the exclusive writable open.  A read-only mount is
    // the safe fallback when those pins no longer match, so it must not be
    // refused by them; it still goes through the read-only safety policy.
    if (read_write) {
      policy.expected_serial = entry.expected_serial;
      policy.expected_stable_id = entry.expected_stable_id;
      policy.expected_size_bytes = entry.expected_size_bytes;
      // Entries from an older config may not carry a class pin.  Bind the
      // live open to the class discovered for this insertion instead of
      // leaving the class unchecked.
      policy.expected_device_class =
          entry.expected_device_class != DeviceClass::kUnknown
              ? entry.expected_device_class
              : filesystem.location.device_class;
    }
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
    // The device was identified by the discovery pass; make the mount itself
    // verify that same filesystem before recovery or an automount RW session
    // can write to it.
    options.expected_uuid = filesystem.probe.superblock.uuid;
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

    // The RAW mount point was detached before this call by the takeover
    // transaction.  A CI-only sentinel exercises the rollback path after the
    // detachment and before WinFsp owns the letter.  The persistent variant
    // keeps failing until the test removes it, so a later device event cannot
    // mask the rollback by mounting successfully.
    if (ConsumeTestHook(L"fail-next-winfsp-start") ||
        TestHookPresent(L"fail-winfsp-start-until-cleared")) {
      result.status = XFS_ERR(XfsError::kInternal,
                              "injected WinFsp mount failure (test hook)");
      return result;
    }

    auto session = winfsp::MountSession::Start(std::move(mounted.value()),
                                               config);
    if (!session.ok()) {
      result.status = session.error();
      return result;
    }

    // Verify the WinFsp volume really is the XFS filesystem, not a stale RAW
    // view of the old volume or another mount that raced us for the letter.
    const uint32_t expected_serial = XfsVolumeSerial(filesystem.probe.superblock);
    std::string mounted_filesystem;
    uint32_t mounted_serial = 0;
    std::string drive_error;
    bool verified = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
      if (QueryDriveVolumeInformation(drive, &mounted_filesystem,
                                      &mounted_serial, &drive_error) &&
          EqualsAsciiCaseInsensitive(mounted_filesystem, "XFS") &&
          mounted_serial == expected_serial) {
        verified = true;
        break;
      }
      Sleep(100);
    }
    if (!verified) {
      session.value()->Stop();
      result.status =
          XFS_ERR(XfsError::kIoError,
                  "the WinFsp mount at " + drive +
                      " could not be verified as the expected XFS filesystem" +
                      (drive_error.empty() ? "" : ": " + drive_error))
              .Metadata("automount mount");
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

  // Publishes the small session snapshot the CLI uses for `xfswin status`.
  // Written only when the effective state changes, and cleared on service
  // stop so a stale file cannot claim a live mount.
  void PublishRuntimeStatus() {
    if (!platform || !engine) return;
    std::vector<AutomountRuntimeMount> mounts;
    std::string fingerprint;
    if (engine->running()) {
      for (const auto& mount : engine->status().mounts) {
        AutomountRuntimeMount item;
        item.fs_uuid = mount.fs_uuid;
        item.label = mount.label;
        item.drive = mount.drive;
        item.mode = ToString(mount.mode);
        item.configured_mode = ToString(mount.configured_mode);
        item.drive_taken_over = mount.drive_taken_over;
        item.original_windows_drive = mount.original_windows_drive;
        item.detail = mount.mode_note;
        fingerprint += FormatUuid(item.fs_uuid) + '\t' + item.drive + '\t' +
                       item.mode + '\t' + item.configured_mode + '\t' +
                       (item.drive_taken_over ? "1" : "0") + '\t' +
                       item.original_windows_drive + '\t' + item.label + '\t' +
                       item.detail + '\n';
        mounts.push_back(std::move(item));
      }
    }
    if (fingerprint == last_runtime_fingerprint) return;
    const Status saved = SaveAutomountRuntimeStatus(
        paths.directory / L"sessions.ini", mounts);
    if (!saved.ok()) {
      AutomountLogRecord record;
      record.level = AutomountLogLevel::kWarn;
      record.event = "status-publish";
      record.result = "failed";
      record.detail = saved.error().Format();
      platform->EmitLog(record);
    }
    last_runtime_fingerprint = std::move(fingerprint);
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
  std::string last_runtime_fingerprint = "<unpublished>";
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
    runtime.PublishRuntimeStatus();
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
  runtime.PublishRuntimeStatus();
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

std::filesystem::path DefaultAutomountRuntimeStatusPath() {
  const AutomountServicePaths paths = DefaultAutomountServicePaths();
  return paths.directory / L"sessions.ini";
}

namespace {

std::string SanitizeRuntimeField(std::string text) {
  for (char& c : text) {
    if (c == '\t' || c == '\r' || c == '\n') c = ' ';
  }
  return text;
}

}  // namespace

Status SaveAutomountRuntimeStatus(
    const std::filesystem::path& path,
    const std::vector<AutomountRuntimeMount>& mounts) {
  std::ostringstream out;
  out << "xfswin-sessions 1\n";
  for (const auto& mount : mounts) {
    out << FormatUuid(mount.fs_uuid) << '\t'
        << SanitizeRuntimeField(mount.drive) << '\t'
        << SanitizeRuntimeField(mount.mode) << '\t'
        << SanitizeRuntimeField(mount.configured_mode) << '\t'
        << (mount.drive_taken_over ? '1' : '0') << '\t'
        << SanitizeRuntimeField(mount.original_windows_drive) << '\t'
        << SanitizeRuntimeField(mount.label) << '\t'
        << SanitizeRuntimeField(mount.detail) << '\n';
  }
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return XFS_ERR(XfsError::kIoError,
                     "cannot create the runtime status directory: " +
                         ec.message());
    }
  }
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      return XFS_ERR(XfsError::kIoError,
                     "cannot write the runtime status: " + temporary.string());
    }
    file << out.str();
    file.flush();
    if (!file) {
      std::filesystem::remove(temporary, ec);
      return XFS_ERR(XfsError::kIoError,
                     "cannot flush the runtime status: " + temporary.string());
    }
  }
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return XFS_ERR(XfsError::kIoError,
                   "cannot replace the runtime status: " + ec.message());
  }
  return OkStatus();
}

Result<AutomountRuntimeStatus> LoadAutomountRuntimeStatus(
    const std::filesystem::path& path) {
  AutomountRuntimeStatus status;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (ec) {
      return XFS_ERR(XfsError::kIoError,
                     "cannot inspect the runtime status: " + ec.message());
    }
    return status;  // loaded=false: no snapshot
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return XFS_ERR(XfsError::kIoError,
                   "cannot open the runtime status: " + path.string());
  }
  std::string header;
  if (!std::getline(file, header) || header != "xfswin-sessions 1") {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "the runtime status has an unknown version");
  }
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    std::vector<std::string> fields;
    size_t begin = 0;
    for (;;) {
      const size_t tab = line.find('\t', begin);
      fields.push_back(line.substr(begin, tab == std::string::npos
                                               ? std::string::npos
                                               : tab - begin));
      if (tab == std::string::npos) break;
      begin = tab + 1;
    }
    if (fields.size() != 8) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "the runtime status has a malformed entry");
    }
    AutomountRuntimeMount mount;
    if (!ParseUuid(fields[0], &mount.fs_uuid)) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "the runtime status has an invalid filesystem UUID");
    }
    mount.drive = fields[1];
    mount.mode = fields[2];
    mount.configured_mode = fields[3];
    mount.drive_taken_over = fields[4] == "1";
    mount.original_windows_drive = fields[5];
    mount.label = fields[6];
    mount.detail = fields[7];
    status.mounts.push_back(std::move(mount));
  }
  status.loaded = true;
  return status;
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