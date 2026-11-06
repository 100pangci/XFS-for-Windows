// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - REL-1 installer / deployment implementation.
//
// This file is the single place that implements the maintenance transaction:
// the fence, the per-session lease registry, the SCM operations used by the
// installer, the captured service snapshot and the six operations the MSI
// calls.  It never touches XFS on-disk state and never reads a block device.

#ifndef WINVER
#define WINVER 0x0602
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "xfs/deployment.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <winver.h>
#include <winsvc.h>

#ifndef IMAGE_FILE_MACHINE_ARM64
#define IMAGE_FILE_MACHINE_ARM64 0xAA64
#endif
// winnt.h defines IMAGE_FILE_MACHINE_ARM64EC (0xA641) in current SDKs; keep a
// correct fallback for older ones.  The value is a 16-bit machine type, not a
// concatenation of the ARM64/AMD64 constants.
#ifndef IMAGE_FILE_MACHINE_ARM64EC
#define IMAGE_FILE_MACHINE_ARM64EC 0xA641
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

// The generated header is available to the product targets.  The fallback
// keeps a source-only build compiling; product verification requires the
// generated values (the CLI and the MSI both build through CMake).
#if defined(__has_include)
#if __has_include("xfswin_version.h")
#include "xfswin_version.h"
#endif
#endif
#ifndef XFSWIN_VERSION
#define XFSWIN_VERSION "0.0.0"
#endif
#ifndef XFSWIN_GIT_SHA
#define XFSWIN_GIT_SHA "unknown"
#endif
#ifndef XFSWIN_BUILD_ARCH
#define XFSWIN_BUILD_ARCH ""
#endif

namespace xfs {
namespace {

constexpr wchar_t kServiceName[] = L"XFSWinAutomount";
constexpr wchar_t kServiceDisplayName[] = L"XFS for Windows automount";
constexpr wchar_t kProgramDataSubdirectory[] = L"xfswin";
// The byte-range lock lives past the metadata so another process can read the
// lease record through a separate handle while the owner still holds the
// session.  Windows enforces byte-range locks for ReadFile on other handles,
// so the locked range must not overlap the metadata at offset 0.
constexpr uint32_t kLeaseLockBytes = 64;
constexpr uint64_t kLeaseLockOffset = 4096;

// ---------------------------------------------------------------------------
// Basic conversions and formatting
// ---------------------------------------------------------------------------

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
  if (length <= 0) return {};
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
  if (length <= 0) return {};
  std::wstring out(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), length);
  return out;
}

std::string Win32ErrorText(DWORD code) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::string text = "error " + std::to_string(code);
  if (length != 0 && buffer) {
    text += ": ";
    text += WideToUtf8(std::wstring(buffer, length));
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' ||
                             text.back() == ' ')) {
      text.pop_back();
    }
  }
  if (buffer) LocalFree(buffer);
  return text;
}

Status Win32Failure(const char* operation, DWORD code,
                    const std::wstring& context = {}) {
  std::string message = operation;
  message += " failed";
  if (!context.empty()) message += " for " + WideToUtf8(context);
  message += ": " + Win32ErrorText(code);
  XfsError kind = XfsError::kIoError;
  if (code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD) {
    kind = XfsError::kPermissionDenied;
  } else if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
    kind = XfsError::kNotFound;
  }
  return XFS_ERR(kind, std::move(message)).Metadata("installer");
}

std::string SanitizeField(std::string text) {
  for (char& c : text) {
    if (c == '\t' || c == '\r' || c == '\n') c = ' ';
  }
  return text;
}

std::string Hex32(uint32_t value) {
  char text[16] = {};
  std::snprintf(text, sizeof(text), "%08lX",
                static_cast<unsigned long>(value));
  return text;
}

std::string NowUtc() {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER value;
  value.LowPart = now.dwLowDateTime;
  value.HighPart = now.dwHighDateTime;
  constexpr uint64_t kEpoch = 116444736000000000ull;
  const uint64_t unix_seconds =
      value.QuadPart > kEpoch ? (value.QuadPart - kEpoch) / 10000000ull : 0;
  const std::time_t seconds = static_cast<std::time_t>(unix_seconds);
  std::tm tm{};
  gmtime_s(&tm, &seconds);
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec);
  return buffer;
}

std::string Trim(std::string text) {
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n' ||
                           text.back() == ' ' || text.back() == '\t')) {
    text.pop_back();
  }
  const size_t start = text.find_first_not_of(" \t");
  if (start == std::string::npos) return {};
  return text.substr(start);
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) lines.push_back(Trim(line));
  return lines;
}

std::string ProductVersion() { return XFSWIN_VERSION; }

std::string BuildArchitecture() {
  const char* arch = XFSWIN_BUILD_ARCH;
  if (arch[0] != '\0') return arch;
  return RuntimeArchitecture();
}

// ---------------------------------------------------------------------------
// Path comparison
// ---------------------------------------------------------------------------

std::wstring NormalizeForCompare(const std::wstring& path) {
  std::wstring text = path;
  std::replace(text.begin(), text.end(), L'/', L'\\');
  if (text.rfind(L"\\\\?\\", 0) == 0) text.erase(0, 4);
  if (text.rfind(L"\\??\\", 0) == 0) text.erase(0, 4);
  while (text.size() > 3 && text.back() == L'\\') text.pop_back();
  for (wchar_t& c : text) c = static_cast<wchar_t>(towlower(c));
  return text;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
  if (a.empty() || b.empty()) return false;
  return NormalizeForCompare(a) == NormalizeForCompare(b);
}

bool PathExists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

// ---------------------------------------------------------------------------
// Protected state directory
// ---------------------------------------------------------------------------

bool ProcessElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
                                      sizeof(elevation), &size) != FALSE;
  CloseHandle(token);
  return ok && elevation.TokenIsElevated != 0;
}

std::wstring CurrentUserSidString() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (size == 0) {
    CloseHandle(token);
    return {};
  }
  std::vector<uint8_t> buffer(size);
  std::wstring result;
  if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
    auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    wchar_t* text = nullptr;
    if (ConvertSidToStringSidW(user->User.Sid, &text) && text) {
      result = text;
      LocalFree(text);
    }
  }
  CloseHandle(token);
  return result;
}

// LocalSystem and the local Administrators group always own the state.
// A non-elevated developer/test process additionally keeps access to its own
// isolated root (the elevated installer and the LocalSystem service are the
// only production writers).
std::wstring ProtectedSddl() {
  std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
  if (!ProcessElevated()) {
    const std::wstring sid = CurrentUserSidString();
    if (!sid.empty()) sddl += L"(A;OICI;FA;;;" + sid + L")";
  }
  return sddl;
}

bool ApplySddlToHandle(HANDLE object, const std::wstring& sddl) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    return false;
  }
  auto* sd = reinterpret_cast<SECURITY_DESCRIPTOR*>(descriptor);
  PACL dacl = nullptr;
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present ||
      dacl == nullptr) {
    LocalFree(descriptor);
    return false;
  }
  // The SDDL starts with D:P; the protected flag must be requested separately
  // when the DACL is applied through SetSecurityInfo, otherwise inherited ACEs
  // from a weaker parent would stay in effect.
  const DWORD result = SetSecurityInfo(
      object, SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
      nullptr, dacl, nullptr);
  LocalFree(descriptor);
  return result == ERROR_SUCCESS;
}

Status SecureDirectory(const std::filesystem::path& path) {
  const std::wstring sddl = ProtectedSddl();
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    return Win32Failure("build the deployment directory security descriptor",
                        GetLastError(), path.wstring());
  }
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = descriptor;
  attributes.bInheritHandle = FALSE;
  const BOOL created = CreateDirectoryW(path.wstring().c_str(), &attributes);
  const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
  LocalFree(descriptor);
  if (create_error != ERROR_SUCCESS && create_error != ERROR_ALREADY_EXISTS) {
    return Win32Failure("create the deployment directory", create_error,
                        path.wstring());
  }
  HANDLE handle = CreateFileW(
      path.wstring().c_str(), WRITE_DAC | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Win32Failure("open the deployment directory for ACL update",
                        GetLastError(), path.wstring());
  }
  const bool secured = ApplySddlToHandle(handle, sddl);
  const DWORD secure_error = secured ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if (!secured) {
    return Win32Failure("protect the deployment directory", secure_error,
                        path.wstring());
  }
  return OkStatus();
}

Status SecureFile(const std::filesystem::path& path) {
  const std::wstring sddl = ProtectedSddl();
  HANDLE handle = CreateFileW(
      path.wstring().c_str(), WRITE_DAC | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Win32Failure("open a deployment file for ACL update",
                        GetLastError(), path.wstring());
  }
  const bool secured = ApplySddlToHandle(handle, sddl);
  const DWORD secure_error = secured ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if (!secured) {
    return Win32Failure("protect a deployment file", secure_error,
                        path.wstring());
  }
  return OkStatus();
}

Status WriteTextFileAtomic(const std::filesystem::path& path,
                           const std::string& content) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return XFS_ERR(XfsError::kIoError,
                     "cannot create directory " +
                         WideToUtf8(path.parent_path().wstring()) + ": " +
                         ec.message())
          .Metadata("installer");
    }
  }
  const std::filesystem::path temporary = path.wstring() + L".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      return XFS_ERR(XfsError::kIoError,
                     "cannot write " + WideToUtf8(temporary.wstring()))
          .Metadata("installer");
    }
    file << content;
    file.flush();
    if (!file) {
      std::filesystem::remove(temporary, ec);
      return XFS_ERR(XfsError::kIoError,
                     "cannot flush " + WideToUtf8(temporary.wstring()))
          .Metadata("installer");
    }
  }
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return XFS_ERR(XfsError::kIoError,
                   "cannot replace " + WideToUtf8(path.wstring()) + ": " +
                       ec.message())
        .Metadata("installer");
  }
  return SecureFile(path);
}

Result<std::string> ReadTextFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return XFS_ERR(XfsError::kNotFound,
                   "cannot read " + WideToUtf8(path.wstring()));
  }
  std::ostringstream out;
  out << file.rdbuf();
  if (!file.good() && !file.eof()) {
    return XFS_ERR(XfsError::kIoError,
                   "cannot read " + WideToUtf8(path.wstring()));
  }
  return out.str();
}

// ---------------------------------------------------------------------------
// Named mutexes
// ---------------------------------------------------------------------------

struct MutexGuard {
  HANDLE handle = nullptr;
  MutexGuard() = default;
  MutexGuard(const MutexGuard&) = delete;
  MutexGuard& operator=(const MutexGuard&) = delete;
  MutexGuard(MutexGuard&& other) noexcept : handle(other.handle) {
    other.handle = nullptr;
  }
  MutexGuard& operator=(MutexGuard&& other) noexcept {
    if (this != &other) {
      Reset();
      handle = other.handle;
      other.handle = nullptr;
    }
    return *this;
  }
  ~MutexGuard() { Reset(); }
  void Reset() {
    if (handle) {
      ReleaseMutex(handle);
      CloseHandle(handle);
      handle = nullptr;
    }
  }
};

constexpr wchar_t kStartupMutexName[] = L"Global\\XFSWin.MountStart";
constexpr wchar_t kStartupMutexFallback[] = L"Local\\XFSWin.MountStart";
constexpr wchar_t kInstallerMutexName[] = L"Global\\XFSWin.Installer";
constexpr wchar_t kInstallerMutexFallback[] = L"Local\\XFSWin.Installer";

Result<MutexGuard> OpenNamedMutex(const wchar_t* name, const wchar_t* fallback,
                                  uint32_t timeout_ms) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const wchar_t* sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100001;;;WD)";
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
    attributes.lpSecurityDescriptor = descriptor;
  }
  HANDLE handle = CreateMutexW(&attributes, FALSE, name);
  DWORD create_error = GetLastError();
  if (!handle) {
    handle = CreateMutexW(&attributes, FALSE, fallback);
    create_error = GetLastError();
  }
  if (descriptor) LocalFree(descriptor);
  if (!handle) {
    return Win32Failure("create a deployment mutex", create_error,
                        std::wstring(name) + L" / " + fallback)
        .error();
  }
  // CreateMutex returns an unowned handle whether or not the object already
  // existed, so every caller must wait before treating the guard as held.
  const DWORD wait = WaitForSingleObject(handle, timeout_ms);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
    CloseHandle(handle);
    if (wait == WAIT_TIMEOUT) {
      return XFS_ERR(XfsError::kIoError,
                     "timed out waiting for the deployment mutex")
          .Metadata("installer");
    }
    return Win32Failure("wait for a deployment mutex", GetLastError())
        .error();
  }
  MutexGuard guard;
  guard.handle = handle;
  return guard;
}

Result<MutexGuard> OpenStartupMutex(uint32_t timeout_ms) {
  return OpenNamedMutex(kStartupMutexName, kStartupMutexFallback, timeout_ms);
}

Result<MutexGuard> OpenInstallerMutex(uint32_t timeout_ms) {
  return OpenNamedMutex(kInstallerMutexName, kInstallerMutexFallback,
                        timeout_ms);
}

// ---------------------------------------------------------------------------
// Lease registry
// ---------------------------------------------------------------------------

constexpr const char* kLeasePrefix = "mount-";
constexpr const char* kLeaseSuffix = ".lease";

bool IsLeaseName(const std::string& name) {
  const size_t prefix = std::strlen(kLeasePrefix);
  const size_t suffix = std::strlen(kLeaseSuffix);
  return name.size() > prefix + suffix &&
         name.compare(0, prefix, kLeasePrefix) == 0 &&
         name.compare(name.size() - suffix, suffix, kLeaseSuffix) == 0;
}

struct LeaseMetadata {
  bool valid = false;
  uint32_t pid = 0;
  std::string mount_point;
  std::string executable;
  std::string started;
};

LeaseMetadata ParseLease(const std::string& content) {
  LeaseMetadata meta;
  for (const std::string& line : SplitLines(content)) {
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (key == "pid") {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
      if (end != value.c_str() && parsed <= 0xFFFFFFFFul) {
        meta.pid = static_cast<uint32_t>(parsed);
        meta.valid = true;
      }
    } else if (key == "mount_point") {
      meta.mount_point = value;
    } else if (key == "executable") {
      meta.executable = value;
    } else if (key == "started") {
      meta.started = value;
    }
  }
  return meta;
}

std::string FormatLease(uint32_t pid, const std::string& mount_point,
                        const std::string& executable) {
  std::ostringstream out;
  out << "xfswin-mount-lease 1\n"
      << "pid=" << pid << "\n"
      << "mount_point=" << SanitizeField(mount_point) << "\n"
      << "executable=" << SanitizeField(executable) << "\n"
      << "started=" << NowUtc() << "\n";
  return out.str();
}

enum class LeaseOpenResult : uint8_t { kLocked, kBusy, kMissing, kError };

LeaseOpenResult OpenLeaseLocked(const std::wstring& path, HANDLE* handle,
                                DWORD* error) {
  HANDLE file = CreateFileW(
      path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD first_error = GetLastError();
    if (first_error == ERROR_FILE_NOT_FOUND ||
        first_error == ERROR_PATH_NOT_FOUND) {
      if (error) *error = first_error;
      return LeaseOpenResult::kMissing;
    }
    file = CreateFileW(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  if (file == INVALID_HANDLE_VALUE) {
    if (error) *error = GetLastError();
    return LeaseOpenResult::kError;
  }
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(kLeaseLockOffset);
  if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                  kLeaseLockBytes, 0, &overlapped)) {
    const DWORD lock_error = GetLastError();
    CloseHandle(file);
    if (lock_error == ERROR_LOCK_VIOLATION) return LeaseOpenResult::kBusy;
    if (error) *error = lock_error;
    return LeaseOpenResult::kError;
  }
  *handle = file;
  return LeaseOpenResult::kLocked;
}

std::string ReadLeaseContent(HANDLE handle) {
  LARGE_INTEGER zero{};
  SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN);
  char buffer[1024] = {};
  DWORD read = 0;
  if (!ReadFile(handle, buffer, sizeof(buffer) - 1, &read, nullptr)) {
    return {};
  }
  return std::string(buffer, read);
}

bool WriteLeaseContent(HANDLE handle, const std::string& content) {
  LARGE_INTEGER zero{};
  SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN);
  DWORD written = 0;
  if (!WriteFile(handle, content.data(), static_cast<DWORD>(content.size()),
                 &written, nullptr) ||
      written != static_cast<DWORD>(content.size())) {
    return false;
  }
  SetEndOfFile(handle);
  return FlushFileBuffers(handle) != FALSE;
}

bool IsValidMountPointText(const std::string& text) {
  if (text.size() == 2 && text[1] == ':' &&
      ((text[0] >= 'A' && text[0] <= 'Z') ||
       (text[0] >= 'a' && text[0] <= 'z'))) {
    return true;
  }
  return text.size() >= 3 &&
         ((text[0] >= 'A' && text[0] <= 'Z') ||
          (text[0] >= 'a' && text[0] <= 'z')) &&
         text[1] == ':' && (text[2] == '\\' || text[2] == '/');
}

bool DriveLetterInUse(const std::string& mount_point) {
  if (mount_point.size() != 2 || mount_point[1] != ':') return false;
  const wchar_t letter = static_cast<wchar_t>(mount_point[0]);
  const DWORD drives = GetLogicalDrives();
  const int index = towupper(letter) - L'A';
  return index >= 0 && index < 26 && (drives & (1u << index)) != 0;
}

bool ProcessImagePath(uint32_t pid, std::wstring* path) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return false;
  wchar_t buffer[32768] = {};
  DWORD size = 32768;
  const bool ok =
      QueryFullProcessImageNameW(process, 0, buffer, &size) != FALSE;
  CloseHandle(process);
  if (ok) *path = std::wstring(buffer, size);
  return ok;
}

// The SCM reports the service process id only while it is running.  Used to
// distinguish the service's own leases from a manual mount during prepare.
bool RunningServiceProcessId(DWORD* pid) {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) return false;
  SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
  if (!service) {
    CloseServiceHandle(scm);
    return false;
  }
  SERVICE_STATUS_PROCESS status{};
  DWORD needed = 0;
  const bool ok = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                       reinterpret_cast<LPBYTE>(&status),
                                       sizeof(status), &needed) != FALSE;
  CloseServiceHandle(service);
  CloseServiceHandle(scm);
  if (!ok || status.dwCurrentState != SERVICE_RUNNING ||
      status.dwProcessId == 0) {
    return false;
  }
  *pid = status.dwProcessId;
  return true;
}

Result<ActiveSessionScan> ScanActiveSessionsLocked(
    const DeploymentPaths& paths, const std::wstring& expected_exe,
    bool remove_stale) {
  ActiveSessionScan scan;
  std::error_code ec;
  if (!std::filesystem::exists(paths.sessions, ec)) {
    return scan;  // nothing has mounted yet
  }
  DWORD service_pid = 0;
  const bool have_service_pid = RunningServiceProcessId(&service_pid);
  std::vector<std::filesystem::path> leases;
  for (const auto& entry : std::filesystem::directory_iterator(
           paths.sessions,
           std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) {
      return XFS_ERR(XfsError::kIoError,
                     "cannot enumerate session leases: " + ec.message())
          .Metadata("installer");
    }
    std::error_code type_error;
    if (!entry.is_regular_file(type_error)) continue;
    const std::string name = WideToUtf8(entry.path().filename().wstring());
    if (!IsLeaseName(name)) continue;
    leases.push_back(entry.path());
  }
  for (const auto& lease : leases) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD open_error = 0;
    const LeaseOpenResult opened =
        OpenLeaseLocked(lease.wstring(), &handle, &open_error);
    if (opened == LeaseOpenResult::kMissing) continue;
    if (opened == LeaseOpenResult::kError) {
      MountSessionRecord record;
      record.session_file = WideToUtf8(lease.filename().wstring());
      record.mount_point = "(unreadable lease)";
      record.executable = Win32ErrorText(open_error);
      scan.sessions.push_back(std::move(record));
      scan.warnings.push_back("could not inspect session lease " +
                              WideToUtf8(lease.filename().wstring()) + ": " +
                              Win32ErrorText(open_error));
      continue;
    }
    if (opened == LeaseOpenResult::kBusy) {
      // The owner still holds its byte-range lock.  Read the record through a
      // separate read-only handle; the owner never takes a lock there.
      HANDLE reader = CreateFileW(
          lease.wstring().c_str(), GENERIC_READ,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      LeaseMetadata meta;
      if (reader != INVALID_HANDLE_VALUE) {
        meta = ParseLease(ReadLeaseContent(reader));
        CloseHandle(reader);
      }
      MountSessionRecord record;
      record.session_file = WideToUtf8(lease.filename().wstring());
      record.pid = meta.pid;
      record.mount_point = meta.mount_point;
      record.started = meta.started;
      record.executable = meta.executable;
      // Only the SCM-reported PID identifies the service process.  The lease
      // records an image path, and a manual mount runs the same xfswin.exe, so
      // path equality is not ownership.
      record.owned_by_service = have_service_pid && meta.pid == service_pid;
      std::wstring image;
      if (meta.pid != 0 && ProcessImagePath(meta.pid, &image)) {
        if (!expected_exe.empty() && !SamePath(image, expected_exe) &&
            !(have_service_pid && meta.pid == service_pid)) {
          scan.warnings.push_back(
              "lease " + record.session_file + " belongs to pid " +
              std::to_string(meta.pid) + " (" + WideToUtf8(image) +
              "), not the expected xfswin image");
        }
      }
      scan.sessions.push_back(std::move(record));
      continue;
    }
    // Lock acquired: the owner is gone.  Remove the stale lease.
    if (remove_stale) {
      std::error_code remove_error;
      std::filesystem::remove(lease, remove_error);
      if (remove_error) {
        scan.warnings.push_back("cannot remove stale session lease " +
                                WideToUtf8(lease.filename().wstring()) + ": " +
                                remove_error.message());
      } else {
        scan.warnings.push_back("removed stale session lease " +
                                WideToUtf8(lease.filename().wstring()));
      }
    }
    if (handle != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlapped{};
      overlapped.Offset = static_cast<DWORD>(kLeaseLockOffset);
      UnlockFileEx(handle, 0, kLeaseLockBytes, 0, &overlapped);
      CloseHandle(handle);
    }
  }
  return scan;
}

// Dynamic signal into the WinFsp adapter.  The adapter owns the named stop
// event; the deployment layer only stores a function pointer so it can be
// linked without WinFsp support.
using UnmountHook = Status (*)(const std::wstring& mount_point);
UnmountHook g_unmount_hook = nullptr;

}  // namespace

// ---------------------------------------------------------------------------
// Runtime WinFsp loader
// ---------------------------------------------------------------------------

const char* RuntimeArchitecture() {
#if defined(_M_ARM64EC)
  return "arm64ec";
#elif defined(_M_ARM64)
  return "arm64";
#elif defined(_M_IX86)
  return "x86";
#else
  return "x64";
#endif
}

namespace {

std::wstring ExpectedWinFspDllName() {
  const std::string arch = RuntimeArchitecture();
  if (arch == "arm64") return L"winfsp-a64.dll";
  if (arch == "x86") return L"winfsp-x86.dll";
  return L"winfsp-x64.dll";
}

std::wstring RegistryWinFspInstallDir() {
  const wchar_t* keys[] = {L"SOFTWARE\\WOW6432Node\\WinFsp",
                           L"SOFTWARE\\WinFsp"};
  const REGSAM flags[] = {KEY_READ | KEY_WOW64_64KEY, KEY_READ};
  for (const wchar_t* key_name : keys) {
    for (REGSAM flag : flags) {
      HKEY key = nullptr;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_name, 0, flag, &key) !=
          ERROR_SUCCESS) {
        continue;
      }
      wchar_t buffer[1024] = {};
      DWORD size = sizeof(buffer) - sizeof(wchar_t);
      DWORD type = 0;
      const LONG result = RegQueryValueExW(
          key, L"InstallDir", nullptr, &type,
          reinterpret_cast<LPBYTE>(buffer), &size);
      RegCloseKey(key);
      if (result == ERROR_SUCCESS && type == REG_SZ && buffer[0] != L'\0') {
        return buffer;
      }
    }
  }
  return {};
}

std::wstring ModulePath(HMODULE module) {
  wchar_t buffer[32768] = {};
  const DWORD length = GetModuleFileNameW(module, buffer, 32768);
  if (length == 0 || length >= 32768) return {};
  return std::wstring(buffer, length);
}

// GetProcAddress returns FARPROC.  Casting a function pointer to void* and
// back trips MSVC C4054 (level 1) and a direct cast trips C4191 (level 3), so
// copy the pointer value instead: FARPROC and the typed pointer have the same
// representation on every Windows target this product ships.
template <typename Fn>
Fn ResolveFunction(HMODULE module, const char* name) {
  FARPROC proc = GetProcAddress(module, name);
  static_assert(sizeof(Fn) == sizeof(proc),
                "function pointer sizes must match");
  Fn fn = nullptr;
  if (proc) std::memcpy(&fn, &proc, sizeof(fn));
  return fn;
}

struct FileVersionInfo {
  bool valid = false;
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t build = 0;
  uint32_t revision = 0;
  std::string text;
};

// Reads the fixed file version without adding a version.lib link dependency.
FileVersionInfo ReadFileVersionResource(const std::wstring& path) {
  FileVersionInfo info;
  HMODULE version = LoadLibraryExW(L"version.dll", nullptr,
                                  LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!version) return info;
  using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
  using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
  using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
  auto size_fn = ResolveFunction<SizeFn>(version, "GetFileVersionInfoSizeW");
  auto info_fn = ResolveFunction<InfoFn>(version, "GetFileVersionInfoW");
  auto query_fn = ResolveFunction<QueryFn>(version, "VerQueryValueW");
  if (!size_fn || !info_fn || !query_fn) {
    FreeLibrary(version);
    return info;
  }
  DWORD handle = 0;
  const DWORD size = size_fn(path.c_str(), &handle);
  if (size == 0) {
    FreeLibrary(version);
    return info;
  }
  std::vector<uint8_t> buffer(size);
  if (!info_fn(path.c_str(), handle, size, buffer.data())) {
    FreeLibrary(version);
    return info;
  }
  VS_FIXEDFILEINFO* fixed = nullptr;
  UINT length = 0;
  if (!query_fn(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed),
                &length) ||
      fixed == nullptr || length < sizeof(VS_FIXEDFILEINFO) ||
      fixed->dwSignature != 0xFEEF04BD) {
    FreeLibrary(version);
    return info;
  }
  info.valid = true;
  info.major = HIWORD(fixed->dwFileVersionMS);
  info.minor = LOWORD(fixed->dwFileVersionMS);
  info.build = HIWORD(fixed->dwFileVersionLS);
  info.revision = LOWORD(fixed->dwFileVersionLS);
  std::ostringstream text;
  text << info.major << '.' << info.minor << '.' << info.build;
  if (info.revision != 0) text << '.' << info.revision;
  info.text = text.str();
  FreeLibrary(version);
  return info;
}

struct DecodedPackedVersion {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t build = 0;
};

// WinFsp packs its version into one UINT32.  The historical layout is
// major<<24 | minor<<16 | build; tolerate the 16-bit-major reading as well so
// an encoding detail cannot turn a genuine runtime into a false negative.
DecodedPackedVersion DecodePackedVersion(uint32_t packed, bool high_major) {
  DecodedPackedVersion version;
  if (high_major) {
    version.major = (packed >> 24) & 0xFF;
    version.minor = (packed >> 16) & 0xFF;
    version.build = packed & 0xFFFF;
  } else {
    version.major = (packed >> 16) & 0xFFFF;
    version.minor = (packed >> 8) & 0xFF;
    version.build = packed & 0xFF;
  }
  return version;
}

const char* const kRequiredWinFspExports[] = {
    "FspFileSystemCreate",
    "FspFileSystemDelete",
    "FspFileSystemSetMountPoint",
    "FspFileSystemRemoveMountPoint",
    "FspFileSystemStartDispatcher",
    "FspFileSystemStopDispatcher",
    "FspFileSystemStopServiceIfNecessary",
    "FspServiceCreate",
    "FspServiceLoop",
    "FspServiceStop",
};

std::mutex g_winfsp_mutex;
WinFspRuntimeInfo g_winfsp_cached;
bool g_winfsp_probed = false;
HMODULE g_winfsp_module = nullptr;

WinFspRuntimeInfo ProbeWinFspLocked() {
  WinFspRuntimeInfo info;
  info.expected_dll = ExpectedWinFspDllName();
  const std::wstring name = info.expected_dll;

  // Load only the absolute path recorded by the official WinFsp installer.
  // A bare module name would let the application directory or the current
  // directory shadow the shared runtime through the DLL search order, so
  // there is deliberately no fallback.  The restricted search resolves the
  // runtime's own dependencies from its bin directory and System32 only.
  const std::wstring install_dir = RegistryWinFspInstallDir();
  if (install_dir.empty() ||
      !std::filesystem::path(install_dir).is_absolute()) {
    info.diagnostic =
        "the WinFsp runtime " + WideToUtf8(name) +
        " is not installed; install the supported WinFsp 2.x runtime";
    return info;
  }
  const std::wstring path =
      (std::filesystem::path(install_dir) / L"bin" / name).wstring();
  HMODULE module = LoadLibraryExW(
      path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module) {
    info.diagnostic = "the WinFsp runtime " + WideToUtf8(name) +
                      " is not installed at " + WideToUtf8(path) +
                      "; install the supported WinFsp 2.x runtime";
    return info;
  }
  info.dll_path = ModulePath(module);

  using FspVersionFn = LONG(WINAPI*)(UINT32*);
  auto version_fn = ResolveFunction<FspVersionFn>(module, "FspVersion");
  if (!version_fn) {
    info.diagnostic =
        "the loaded module does not export FspVersion and is not the official "
        "WinFsp runtime";
    FreeLibrary(module);
    return info;
  }
  UINT32 packed = 0;
  const LONG status = version_fn(&packed);
  if (status < 0) {
    info.diagnostic = "FspVersion failed with NTSTATUS " +
                      std::to_string(static_cast<long long>(status));
    FreeLibrary(module);
    return info;
  }

  const FileVersionInfo file_version = ReadFileVersionResource(info.dll_path);
  const DecodedPackedVersion high = DecodePackedVersion(packed, true);
  const DecodedPackedVersion low = DecodePackedVersion(packed, false);
  if (file_version.valid) {
    info.major = file_version.major;
    info.minor = file_version.minor;
    info.build = file_version.build;
    info.version = file_version.text;
    const bool high_match = high.major == file_version.major &&
                            high.minor == file_version.minor;
    const bool low_match = low.major == file_version.major &&
                           low.minor == file_version.minor;
    if (!high_match && !low_match) {
      info.diagnostic = "FspVersion 0x" + Hex32(packed) +
                        " does not match the file version " +
                        file_version.text;
    }
  } else {
    const DecodedPackedVersion decoded = high.major != 0 ? high : low;
    info.major = decoded.major;
    info.minor = decoded.minor;
    info.build = decoded.build;
    info.version = std::to_string(decoded.major) + "." +
                   std::to_string(decoded.minor) + "." +
                   std::to_string(decoded.build);
    info.diagnostic =
        "the WinFsp runtime has no readable file version resource; using "
        "FspVersion";
  }

  if (info.major != 2) {
    const std::string found = info.version.empty() ? "unknown" : info.version;
    info.available = false;
    info.diagnostic = "WinFsp " + found +
                      " is not supported; XFS for Windows requires the WinFsp "
                      "2.x runtime";
    info.major = info.minor = info.build = 0;
    info.version = found;
    FreeLibrary(module);
    return info;
  }

  for (const char* export_name : kRequiredWinFspExports) {
    if (!GetProcAddress(module, export_name)) {
      info.diagnostic = std::string("the WinFsp runtime is missing the ") +
                        export_name + " export";
      FreeLibrary(module);
      return info;
    }
  }

  info.available = true;
  g_winfsp_module = module;  // stays loaded for the process lifetime
  g_winfsp_cached = info;
  g_winfsp_probed = true;
  return info;
}

}  // namespace

WinFspRuntimeInfo ProbeWinFspRuntime() {
  std::lock_guard<std::mutex> lock(g_winfsp_mutex);
  if (g_winfsp_probed) return g_winfsp_cached;
  return ProbeWinFspLocked();
}

Status PreloadWinFspRuntime(WinFspRuntimeInfo* info) {
  const WinFspRuntimeInfo probed = ProbeWinFspRuntime();
  if (info) *info = probed;
  if (probed.available) return OkStatus();
  const XfsError kind = probed.dll_path.empty()
                            ? XfsError::kNotFound
                            : XfsError::kUnsupportedFeature;
  return XFS_ERR(kind, probed.diagnostic.empty()
                           ? std::string("the WinFsp runtime is not usable")
                           : probed.diagnostic)
      .Metadata("winfsp");
}

// ---------------------------------------------------------------------------
// PE architecture validation
// ---------------------------------------------------------------------------

bool ValidateExecutableArchitecture(const std::filesystem::path& path,
                                    std::string* error) {
  auto fail = [error](const std::string& text) {
    if (error) *error = text;
    return false;
  };
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return fail("cannot open " + WideToUtf8(path.wstring()));
  }
  IMAGE_DOS_HEADER dos{};
  file.read(reinterpret_cast<char*>(&dos), sizeof(dos));
  if (!file || dos.e_magic != IMAGE_DOS_SIGNATURE) {
    return fail("not a PE image (missing MZ header)");
  }
  if (dos.e_lfanew < 0 || dos.e_lfanew > 64 * 1024 * 1024) {
    return fail("not a PE image (invalid e_lfanew)");
  }
  file.seekg(dos.e_lfanew, std::ios::beg);
  DWORD signature = 0;
  WORD machine = 0;
  file.read(reinterpret_cast<char*>(&signature), sizeof(signature));
  file.read(reinterpret_cast<char*>(&machine), sizeof(machine));
  if (!file || signature != IMAGE_NT_SIGNATURE) {
    return fail("not a PE image (missing PE signature)");
  }

  const std::string native = RuntimeArchitecture();
  WORD expected = 0;
  if (native == "arm64") {
    expected = IMAGE_FILE_MACHINE_ARM64;
  } else if (native == "x86") {
    expected = IMAGE_FILE_MACHINE_I386;
  } else if (native == "x64") {
    expected = IMAGE_FILE_MACHINE_AMD64;
  } else {
    return fail(
        "this build targets ARM64EC/x64 emulation, which the service does not "
        "support; use a native x64 or ARM64 package");
  }
  if (machine == IMAGE_FILE_MACHINE_ARM64EC) {
    return fail(
        "the image is ARM64EC, not native ARM64; release packages must be "
        "native x64 or native ARM64");
  }
  if (machine != expected) {
    std::string actual;
    switch (machine) {
      case IMAGE_FILE_MACHINE_AMD64: actual = "x64"; break;
      case IMAGE_FILE_MACHINE_ARM64: actual = "arm64"; break;
      case IMAGE_FILE_MACHINE_I386: actual = "x86"; break;
      case IMAGE_FILE_MACHINE_ARM:
      case IMAGE_FILE_MACHINE_THUMB: actual = "arm"; break;
      default: {
        char text[16] = {};
        std::snprintf(text, sizeof(text), "machine 0x%04X",
                      static_cast<unsigned>(machine));
        actual = text;
        break;
      }
    }
    return fail("the image is " + actual + " but this package is " + native);
  }
  if (error) error->clear();
  return true;
}

// ---------------------------------------------------------------------------
// Paths and fence
// ---------------------------------------------------------------------------

DeploymentPaths ResolveDeploymentPaths(const std::filesystem::path& root) {
  DeploymentPaths paths;
  paths.root = root;
  paths.sessions = root / L"sessions";
  paths.installer = root / L"installer";
  paths.fence = root / L"maintenance.fence";
  return paths;
}

DeploymentPaths DefaultDeploymentPaths() {
  // Deliberately no environment override: the fence and lease directory must
  // be the same machine-wide location for the elevated installer, the
  // LocalSystem service and manual mounts.  A process-controlled redirect
  // would silently remove the maintenance fence and let a new mount race an
  // upgrade.  Isolated tests pass an explicit root to ResolveDeploymentPaths.
  wchar_t buffer[MAX_PATH + 1] = {};
  const DWORD length =
      GetEnvironmentVariableW(L"ProgramData", buffer, MAX_PATH + 1);
  std::wstring program_data;
  if (length > 0 && length <= MAX_PATH) {
    program_data.assign(buffer, length);
  } else {
    program_data = L"C:\\ProgramData";
  }
  return ResolveDeploymentPaths(std::filesystem::path(program_data) /
                                kProgramDataSubdirectory);
}

Status EnsureDeploymentPaths(const DeploymentPaths& paths) {
  if (paths.root.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "the deployment state directory is empty")
        .Metadata("installer");
  }
  Status root = SecureDirectory(paths.root);
  if (!root.ok()) return root;
  Status sessions = SecureDirectory(paths.sessions);
  if (!sessions.ok()) return sessions;
  return SecureDirectory(paths.installer);
}

Status ReadMaintenanceFence(const DeploymentPaths& paths,
                            MaintenanceFenceInfo* info) {
  if (!info) {
    return XFS_ERR(XfsError::kInvalidArgument, "missing fence result");
  }
  *info = MaintenanceFenceInfo{};
  std::error_code ec;
  const bool exists = std::filesystem::exists(paths.fence, ec);
  if (ec) {
    info->readable = false;
    return XFS_ERR(XfsError::kIoError,
                   "cannot inspect the maintenance fence: " + ec.message())
        .Metadata("installer");
  }
  if (!exists) return OkStatus();
  info->exists = true;
  auto content = ReadTextFile(paths.fence);
  if (!content.ok()) {
    info->readable = false;
    return content.error();
  }
  for (const std::string& line : SplitLines(content.value())) {
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (key == "operation") info->operation = value;
    if (key == "install_path") info->install_path = value;
    if (key == "pid") {
      info->pid =
          static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    }
  }
  return OkStatus();
}

bool MaintenanceFenceActive(const DeploymentPaths& paths) {
  MaintenanceFenceInfo info;
  if (!ReadMaintenanceFence(paths, &info).ok()) return true;
  return info.exists;
}

Result<ActiveSessionScan> ScanActiveSessions(
    const DeploymentPaths& paths, const std::wstring& expected_exe) {
  if (paths.root.empty() || paths.sessions.empty()) {
    return ActiveSessionScan{};
  }
  auto mutex = OpenStartupMutex(10000);
  if (!mutex.ok()) return mutex.error();
  return ScanActiveSessionsLocked(paths, expected_exe, true);
}

void SetMountUnmountHook(Status (*hook)(const std::wstring& mount_point)) {
  g_unmount_hook = hook;
}

Status StopMountSessions(const DeploymentPaths& paths,
                         const std::wstring& expected_exe,
                         const std::vector<MountSessionRecord>& records,
                         uint32_t timeout_ms, SessionStopResult* result,
                         std::vector<std::string>* recoverable_detail) {
  SessionStopResult local;
  if (result) *result = local;
  std::vector<std::string> signalled;
  for (const auto& record : records) {
    if (!record.owned_by_service) {
      local.all_stopped = false;
      local.remaining.push_back(record.mount_point + " (pid " +
                                std::to_string(record.pid) +
                                "): manual mount");
      continue;
    }
    if (!IsValidMountPointText(record.mount_point)) {
      local.all_stopped = false;
      local.remaining.push_back(record.session_file +
                                ": lease has no valid mount point");
      continue;
    }
    if (!g_unmount_hook ||
        !g_unmount_hook(Utf8ToWide(record.mount_point)).ok()) {
      local.all_stopped = false;
      local.remaining.push_back(record.mount_point +
                                ": could not signal the mount session");
      continue;
    }
    signalled.push_back(record.mount_point);
  }
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    auto scan = ScanActiveSessions(paths, expected_exe);
    if (!scan.ok()) {
      local.all_stopped = false;
      local.remaining.push_back("cannot rescan sessions: " +
                                scan.error().Format());
      break;
    }
    if (scan.value().sessions.empty()) break;
    if (GetTickCount64() >= deadline) {
      local.all_stopped = false;
      for (const auto& session : scan.value().sessions) {
        local.remaining.push_back(session.mount_point + " (pid " +
                                  std::to_string(session.pid) + ")");
        if (recoverable_detail) {
          recoverable_detail->push_back(
              "session " + session.mount_point + " pid " +
              std::to_string(session.pid) + " image " + session.executable +
              " still holds a lease");
        }
      }
      for (const std::string& mount : signalled) {
        const bool still = std::any_of(
            scan.value().sessions.begin(), scan.value().sessions.end(),
            [&mount](const MountSessionRecord& session) {
              return session.mount_point == mount;
            });
        if (still && DriveLetterInUse(mount)) local.mounted.push_back(mount);
      }
      break;
    }
    Sleep(200);
  }
  if (result) *result = local;
  return local.all_stopped
             ? OkStatus()
             : XFS_ERR(XfsError::kIoError,
                       "one or more mount sessions did not stop")
                   .Metadata("installer");
}

namespace {

bool WaitForSessionsGone(const DeploymentPaths& paths,
                         const std::wstring& expected_exe, uint32_t timeout_ms,
                         ActiveSessionScan* last_scan) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    auto scan = ScanActiveSessions(paths, expected_exe);
    if (!scan.ok()) {
      if (last_scan) *last_scan = ActiveSessionScan{};
      return false;
    }
    if (scan.value().sessions.empty()) {
      if (last_scan) *last_scan = std::move(scan.value());
      return true;
    }
    if (GetTickCount64() >= deadline) {
      if (last_scan) *last_scan = std::move(scan.value());
      return false;
    }
    Sleep(200);
  }
}

// Writes the fence while holding the startup mutex.  Every mount start takes
// the same mutex, so either the mount observes the fence or the installer
// observes the new lease.
Status WriteFenceLocked(const DeploymentPaths& paths,
                        const InstallerOptions& options,
                        const std::string& install_path) {
  auto mutex = OpenStartupMutex(10000);
  if (!mutex.ok()) return mutex.error();
  std::ostringstream out;
  out << "xfswin-maintenance-fence 1\n"
      << "operation=" << ToString(options.action) << "\n"
      << "install_path=" << SanitizeField(install_path) << "\n"
      << "pid=" << GetCurrentProcessId() << "\n"
      << "created_utc=" << NowUtc() << "\n";
  return WriteTextFileAtomic(paths.fence, out.str());
}

// apply.state records that the transaction reached the service-mutating step
// (apply or remove).  Prepare writes the fence and capture before it classifies
// live sessions, so a refused upgrade leaves both behind while the original
// service and its mounts keep running.  The MSI's rollback stop action is
// scheduled after InstallFiles for the active package (its sequence condition
// only excludes the old package during RemoveExistingProducts), so it must
// consult this marker: only a transaction that actually changed the service
// may stop it during rollback.
std::filesystem::path ApplyMarkerFilePath(const DeploymentPaths& paths) {
  return paths.installer / L"apply.state";
}

Status WriteApplyMarker(const DeploymentPaths& paths, InstallerAction action) {
  std::ostringstream out;
  out << "xfswin-installer-apply 1\n"
      << "operation=" << ToString(action) << "\n"
      << "pid=" << GetCurrentProcessId() << "\n"
      << "created_utc=" << NowUtc() << "\n";
  return WriteTextFileAtomic(ApplyMarkerFilePath(paths), out.str());
}

bool ApplyMarkerExists(const DeploymentPaths& paths) {
  std::error_code ec;
  const bool exists =
      std::filesystem::exists(ApplyMarkerFilePath(paths), ec);
  return !ec && exists;
}

void RemoveApplyMarker(const DeploymentPaths& paths) {
  std::error_code ec;
  std::filesystem::remove(ApplyMarkerFilePath(paths), ec);
}

Status ClearMaintenanceState(const DeploymentPaths& paths) {
  {
    auto mutex = OpenStartupMutex(10000);
    if (!mutex.ok()) return mutex.error();
    std::error_code ec;
    std::filesystem::remove(paths.fence, ec);
    if (ec) {
      return XFS_ERR(XfsError::kIoError,
                     "cannot remove the maintenance fence: " + ec.message())
          .Metadata("installer");
    }
  }
  std::error_code ec;
  std::filesystem::remove(paths.installer / L"rollback.ini", ec);
  RemoveApplyMarker(paths);
  return OkStatus();
}

std::filesystem::path StateFilePath(const DeploymentPaths& paths) {
  return paths.installer / L"rollback.ini";
}

// ---------------------------------------------------------------------------
// SCM helpers
// ---------------------------------------------------------------------------

struct ScmHandle {
  SC_HANDLE handle = nullptr;
  ScmHandle() = default;
  ScmHandle(const ScmHandle&) = delete;
  ScmHandle& operator=(const ScmHandle&) = delete;
  ~ScmHandle() {
    if (handle) CloseServiceHandle(handle);
  }
};

struct ServiceHandle {
  SC_HANDLE handle = nullptr;
  ServiceHandle() = default;
  explicit ServiceHandle(SC_HANDLE value) : handle(value) {}
  ServiceHandle(const ServiceHandle&) = delete;
  ServiceHandle& operator=(const ServiceHandle&) = delete;
  ~ServiceHandle() {
    if (handle) CloseServiceHandle(handle);
  }
};

InstallerServiceState MapServiceState(DWORD state) {
  switch (state) {
    case SERVICE_STOPPED: return InstallerServiceState::kStopped;
    case SERVICE_START_PENDING: return InstallerServiceState::kStartPending;
    case SERVICE_STOP_PENDING: return InstallerServiceState::kStopPending;
    case SERVICE_RUNNING: return InstallerServiceState::kRunning;
    case SERVICE_PAUSED: return InstallerServiceState::kPaused;
    default: return InstallerServiceState::kUnknown;
  }
}

bool QueryStatus(SC_HANDLE service, SERVICE_STATUS_PROCESS* status) {
  DWORD needed = 0;
  return QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(status),
                              sizeof(*status), &needed) != FALSE;
}

bool WaitForServiceState(SC_HANDLE service, DWORD desired, uint32_t timeout_ms,
                         DWORD* exit_code) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    SERVICE_STATUS_PROCESS status{};
    if (!QueryStatus(service, &status)) return false;
    if (status.dwCurrentState == desired) {
      if (exit_code) *exit_code = status.dwWin32ExitCode;
      return true;
    }
    if (status.dwCurrentState == SERVICE_STOPPED && desired != SERVICE_STOPPED) {
      if (exit_code) *exit_code = status.dwWin32ExitCode;
      return false;
    }
    if (GetTickCount64() >= deadline) return false;
    Sleep(100);
  }
}

std::wstring BuildMultiSz(const std::vector<std::wstring>& items) {
  std::wstring out;
  for (const auto& item : items) {
    out += item;
    out.push_back(L'\0');
  }
  out.push_back(L'\0');
  if (out.size() < 2) out.push_back(L'\0');
  return out;
}

}  // namespace

const char* ToString(InstallerServiceState state) {
  switch (state) {
    case InstallerServiceState::kNotInstalled: return "not-installed";
    case InstallerServiceState::kStopped: return "stopped";
    case InstallerServiceState::kStartPending: return "start-pending";
    case InstallerServiceState::kStopPending: return "stop-pending";
    case InstallerServiceState::kRunning: return "running";
    case InstallerServiceState::kPaused: return "paused";
    case InstallerServiceState::kUnknown: return "unknown";
  }
  return "unknown";
}

const char* ToString(InstallerAction action) {
  switch (action) {
    case InstallerAction::kInstall: return "install";
    case InstallerAction::kUpgrade: return "upgrade";
    case InstallerAction::kUninstall: return "uninstall";
  }
  return "install";
}

bool ParseInstallerAction(const std::string& text, InstallerAction* action) {
  if (!action) return false;
  if (text == "install") {
    *action = InstallerAction::kInstall;
    return true;
  }
  if (text == "upgrade") {
    *action = InstallerAction::kUpgrade;
    return true;
  }
  if (text == "uninstall") {
    *action = InstallerAction::kUninstall;
    return true;
  }
  return false;
}

std::wstring ParseServiceImagePath(const std::wstring& image_path) {
  std::wstring text = image_path;
  while (!text.empty() && text.front() == L' ') text.erase(text.begin());
  if (text.empty()) return {};
  if (text.front() == L'"') {
    const size_t closing = text.find(L'"', 1);
    if (closing == std::wstring::npos) return {};
    return text.substr(1, closing - 1);
  }
  const size_t space = text.find(L' ');
  return space == std::wstring::npos ? text : text.substr(0, space);
}

Status QueryInstallerService(InstallerServiceConfig* config) {
  if (!config) {
    return XFS_ERR(XfsError::kInvalidArgument, "missing service config");
  }
  *config = InstallerServiceConfig{};
  config->name = kServiceName;
  config->display_name = kServiceDisplayName;
  ScmHandle scm;
  scm.handle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm.handle) {
    return Win32Failure("open the service manager", GetLastError());
  }
  ServiceHandle service(
      OpenServiceW(scm.handle, kServiceName,
                   SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
  if (!service.handle) {
    const DWORD code = GetLastError();
    if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
      config->installed = false;
      config->state = InstallerServiceState::kNotInstalled;
      return OkStatus();
    }
    if (code == ERROR_SERVICE_MARKED_FOR_DELETE) {
      // The SCM still remembers the service until every handle is closed.
      // Report it as installed so the removal path can wait for the SCM to
      // forget it instead of failing on an error it cannot act on.
      config->installed = true;
      config->state = InstallerServiceState::kUnknown;
      config->detail = "the service is marked for deletion";
      return OkStatus();
    }
    return Win32Failure("open the automount service", code, kServiceName);
  }
  config->installed = true;

  SERVICE_STATUS_PROCESS status{};
  if (!QueryStatus(service.handle, &status)) {
    return Win32Failure("query the automount service status", GetLastError(),
                        kServiceName);
  }
  config->state = MapServiceState(status.dwCurrentState);
  config->running = status.dwCurrentState == SERVICE_RUNNING;
  config->stop_pending = status.dwCurrentState == SERVICE_STOP_PENDING;
  config->start_pending = status.dwCurrentState == SERVICE_START_PENDING;
  if (status.dwCurrentState == SERVICE_STOPPED &&
      status.dwWin32ExitCode != ERROR_SUCCESS) {
    config->detail =
        "last exit code " + std::to_string(status.dwWin32ExitCode);
  }

  DWORD needed = 0;
  if (!QueryServiceConfigW(service.handle, nullptr, 0, &needed) &&
      GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return Win32Failure("size the automount service configuration",
                        GetLastError(), kServiceName);
  }
  if (needed == 0) {
    return XFS_ERR(XfsError::kInternal,
                   "the service manager returned an empty configuration size")
        .Metadata("installer");
  }
  std::vector<uint8_t> buffer(needed);
  auto* query = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
  if (!QueryServiceConfigW(service.handle, query, needed, &needed)) {
    return Win32Failure("query the automount service configuration",
                        GetLastError(), kServiceName);
  }
  config->raw_image_path =
      query->lpBinaryPathName ? query->lpBinaryPathName : L"";
  config->binary_path = ParseServiceImagePath(config->raw_image_path);
  config->start_type = query->dwStartType;
  if (query->lpServiceStartName) config->account = query->lpServiceStartName;
  if (query->lpDependencies) {
    const wchar_t* cursor = query->lpDependencies;
    while (*cursor != L'\0') {
      config->dependencies.emplace_back(cursor);
      cursor += config->dependencies.back().size() + 1;
    }
  }
  SERVICE_DELAYED_AUTO_START_INFO delayed{};
  if (QueryServiceConfig2W(service.handle,
                           SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                           reinterpret_cast<LPBYTE>(&delayed),
                           sizeof(delayed), &needed)) {
    config->delayed_auto_start = delayed.fDelayedAutostart != FALSE;
  }
  return OkStatus();
}

namespace {

Status ConfigureExistingService(SC_HANDLE service,
                                const std::wstring& binary_path) {
  DWORD needed = 0;
  if (!QueryServiceConfigW(service, nullptr, 0, &needed) &&
      GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return Win32Failure("size the automount service configuration",
                        GetLastError());
  }
  std::vector<uint8_t> buffer(needed);
  auto* query = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
  if (!QueryServiceConfigW(service, query, needed, &needed)) {
    return Win32Failure("query the automount service configuration",
                        GetLastError());
  }
  const std::wstring current =
      query->lpBinaryPathName ? query->lpBinaryPathName : L"";
  const std::wstring command = L"\"" + binary_path + L"\" automount run";
  const bool path_changed = current != command;
  const bool start_changed = query->dwStartType != SERVICE_AUTO_START;
  if (!path_changed && !start_changed) return OkStatus();
  if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                            SERVICE_NO_CHANGE, command.c_str(), nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr)) {
    return Win32Failure("update the automount service executable",
                        GetLastError());
  }
  return OkStatus();
}

Status EnsureServiceWithConfig(const InstallerServiceSnapshot& original,
                               const std::wstring& binary_path,
                               std::wstring* detail) {
  const std::wstring command = L"\"" + binary_path + L"\" automount run";
  const std::wstring account =
      original.account.empty() ? L"LocalSystem" : original.account;
  const std::wstring dependencies = BuildMultiSz(original.dependencies);
  const DWORD start_type =
      original.start_type == 0 ? SERVICE_AUTO_START : original.start_type;
  ScmHandle scm;
  scm.handle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (!scm.handle) {
    return Win32Failure("open the service manager", GetLastError());
  }
  InstallerServiceConfig current;
  Status queried = QueryInstallerService(&current);
  if (!queried.ok()) return queried;
  if (!current.installed) {
    ServiceHandle service(CreateServiceW(
        scm.handle, kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, start_type, SERVICE_ERROR_NORMAL,
        command.c_str(), nullptr, nullptr, dependencies.c_str(),
        account.c_str(), nullptr));
    if (!service.handle) {
      return Win32Failure("recreate the automount service", GetLastError(),
                          kServiceName);
    }
    SERVICE_DELAYED_AUTO_START_INFO delayed{};
    delayed.fDelayedAutostart = original.delayed_auto_start ? TRUE : FALSE;
    ChangeServiceConfig2W(service.handle,
                          SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
    if (detail) *detail = L"service recreated from the captured snapshot";
    return OkStatus();
  }
  ServiceHandle service(OpenServiceW(
      scm.handle, kServiceName,
      SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
  if (!service.handle) {
    return Win32Failure("open the automount service for rollback",
                        GetLastError(), kServiceName);
  }
  if (!ChangeServiceConfigW(service.handle, SERVICE_NO_CHANGE, start_type,
                            SERVICE_NO_CHANGE, command.c_str(), nullptr,
                            nullptr, dependencies.c_str(), account.c_str(),
                            nullptr, nullptr)) {
    return Win32Failure("restore the automount service configuration",
                        GetLastError(), kServiceName);
  }
  SERVICE_DELAYED_AUTO_START_INFO delayed{};
  delayed.fDelayedAutostart = original.delayed_auto_start ? TRUE : FALSE;
  ChangeServiceConfig2W(service.handle,
                        SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
  if (detail) *detail = L"service configuration restored";
  return OkStatus();
}

}  // namespace

Status EnsureInstallerService(const std::wstring& binary_path,
                              const std::wstring& display_name,
                              InstallerServiceConfig* config) {
  if (binary_path.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "the automount service executable path is empty")
        .Metadata("installer");
  }
  InstallerServiceConfig current;
  Status queried = QueryInstallerService(&current);
  if (!queried.ok()) return queried;
  ScmHandle scm;
  scm.handle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (!scm.handle) {
    return Win32Failure("open the service manager", GetLastError());
  }
  if (!current.installed) {
    const std::wstring command = L"\"" + binary_path + L"\" automount run";
    ServiceHandle service(CreateServiceW(
        scm.handle, kServiceName,
        display_name.empty() ? kServiceDisplayName : display_name.c_str(),
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
        nullptr, nullptr));
    if (!service.handle) {
      const DWORD code = GetLastError();
      if (code == ERROR_SERVICE_EXISTS ||
          code == ERROR_SERVICE_MARKED_FOR_DELETE) {
        return XFS_ERR(XfsError::kAlreadyExists,
                       "the automount service exists but cannot be opened: " +
                           Win32ErrorText(code))
            .Metadata("installer");
      }
      return Win32Failure("create the automount service", code);
    }
    SERVICE_DESCRIPTIONW description{};
    const std::wstring text =
        L"Discovers attached XFS filesystems and mounts configured "
        L"filesystems under a free drive letter.";
    description.lpDescription = const_cast<LPWSTR>(text.c_str());
    ChangeServiceConfig2W(service.handle, SERVICE_CONFIG_DESCRIPTION,
                          &description);
    if (config) {
      Status refreshed = QueryInstallerService(config);
      if (!refreshed.ok()) return refreshed;
    }
    return OkStatus();
  }
  ServiceHandle service(OpenServiceW(
      scm.handle, kServiceName,
      SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
  if (!service.handle) {
    return Win32Failure("open the automount service for update",
                        GetLastError(), kServiceName);
  }
  Status updated = ConfigureExistingService(service.handle, binary_path);
  if (!updated.ok()) return updated;
  if (config) {
    Status refreshed = QueryInstallerService(config);
    if (!refreshed.ok()) return refreshed;
  }
  return OkStatus();
}

Status StartInstallerService(std::string* message) {
  InstallerServiceConfig config;
  Status queried = QueryInstallerService(&config);
  if (!queried.ok()) return queried;
  if (!config.installed) {
    return XFS_ERR(XfsError::kNotFound,
                   "the automount service is not installed")
        .Metadata("installer");
  }
  ScmHandle scm;
  scm.handle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm.handle) {
    return Win32Failure("open the service manager", GetLastError());
  }
  ServiceHandle service(OpenServiceW(
      scm.handle, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS));
  if (!service.handle) {
    return Win32Failure("open the automount service", GetLastError(),
                        kServiceName);
  }
  if (config.running) {
    if (message) *message = "the automount service is already running";
    return OkStatus();
  }
  if (!StartServiceW(service.handle, 0, nullptr)) {
    const DWORD code = GetLastError();
    if (code != ERROR_SERVICE_ALREADY_RUNNING) {
      return Win32Failure("start the automount service", code);
    }
  }
  DWORD exit_code = 0;
  if (!WaitForServiceState(service.handle, SERVICE_RUNNING, 30000,
                           &exit_code)) {
    std::string text =
        "the automount service did not reach the running state";
    if (exit_code != 0) {
      text += " (service exit code " + std::to_string(exit_code) + ")";
    }
    return XFS_ERR(XfsError::kIoError, text).Metadata("installer");
  }
  if (message) *message = "the automount service is running";
  return OkStatus();
}

Status StopInstallerService(std::string* message) {
  InstallerServiceConfig config;
  Status queried = QueryInstallerService(&config);
  if (!queried.ok()) return queried;
  if (!config.installed) {
    if (message) *message = "the automount service is not installed";
    return OkStatus();
  }
  ScmHandle scm;
  scm.handle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm.handle) {
    return Win32Failure("open the service manager", GetLastError());
  }
  ServiceHandle service(OpenServiceW(
      scm.handle, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS));
  if (!service.handle) {
    const DWORD code = GetLastError();
    if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
      if (message) *message = "the automount service is not installed";
      return OkStatus();
    }
    return Win32Failure("open the automount service", code, kServiceName);
  }
  if (config.state == InstallerServiceState::kStopped) {
    if (message) {
      *message = "the automount service is not running";
      if (!config.detail.empty()) *message += " (" + config.detail + ")";
    }
    return OkStatus();
  }
  if (config.state != InstallerServiceState::kStopPending) {
    SERVICE_STATUS ignored{};
    if (!ControlService(service.handle, SERVICE_CONTROL_STOP, &ignored)) {
      const DWORD code = GetLastError();
      if (code != ERROR_SERVICE_NOT_ACTIVE) {
        return Win32Failure("stop the automount service", code, kServiceName);
      }
    }
  }
  DWORD exit_code = 0;
  if (!WaitForServiceState(service.handle, SERVICE_STOPPED, 30000,
                           &exit_code)) {
    SERVICE_STATUS_PROCESS current{};
    const std::string state =
        QueryStatus(service.handle, &current)
            ? ToString(MapServiceState(current.dwCurrentState))
            : std::string("unknown");
    return XFS_ERR(XfsError::kIoError,
                   "the automount service did not stop (state " + state + ")")
        .Metadata("installer");
  }
  if (exit_code != ERROR_SUCCESS) {
    // The service stopped but reported a clean-stop failure (durability or
    // drive-letter restoration).  The installer must not pretend otherwise;
    // the details are in the shutdown record.  A stale exit code from an
    // earlier, already-stopped run does not block the transaction: it was
    // handled by the early return above.
    return XFS_ERR(XfsError::kIoError,
                   "the automount service stopped with exit code " +
                       std::to_string(exit_code) +
                       "; the shutdown record has the details")
        .Metadata("installer");
  }
  if (message) *message = "the automount service is stopped";
  return OkStatus();
}

Status DeleteInstallerService(bool* pending) {
  if (pending) *pending = false;
  InstallerServiceConfig config;
  Status queried = QueryInstallerService(&config);
  if (!queried.ok()) return queried;
  if (!config.installed) return OkStatus();
  ScmHandle scm;
  scm.handle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm.handle) {
    return Win32Failure("open the service manager", GetLastError());
  }
  ServiceHandle service(OpenServiceW(
      scm.handle, kServiceName, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS));
  if (!service.handle) {
    const DWORD code = GetLastError();
    if (code == ERROR_SERVICE_DOES_NOT_EXIST) return OkStatus();
    if (code == ERROR_SERVICE_MARKED_FOR_DELETE) {
      if (pending) *pending = true;
      return OkStatus();
    }
    return Win32Failure("open the automount service for removal", code,
                        kServiceName);
  }
  if (config.state != InstallerServiceState::kStopped) {
    if (config.state != InstallerServiceState::kStopPending) {
      SERVICE_STATUS ignored{};
      if (!ControlService(service.handle, SERVICE_CONTROL_STOP, &ignored)) {
        const DWORD code = GetLastError();
        if (code != ERROR_SERVICE_NOT_ACTIVE) {
          return Win32Failure("stop the automount service", code, kServiceName);
        }
      }
    }
    DWORD exit_code = 0;
    if (!WaitForServiceState(service.handle, SERVICE_STOPPED, 30000,
                             &exit_code)) {
      return XFS_ERR(XfsError::kIoError,
                     "the automount service did not stop before removal")
          .Metadata("installer");
    }
  }
  if (!DeleteService(service.handle)) {
    const DWORD code = GetLastError();
    if (code == ERROR_SERVICE_MARKED_FOR_DELETE) {
      if (pending) *pending = true;
      return OkStatus();
    }
    return Win32Failure("delete the automount service", code, kServiceName);
  }
  return OkStatus();
}

Status WaitInstallerServiceDeleted(uint32_t timeout_ms) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    InstallerServiceConfig config;
    Status queried = QueryInstallerService(&config);
    if (!queried.ok()) return queried;
    if (!config.installed) return OkStatus();
    if (GetTickCount64() >= deadline) {
      return XFS_ERR(XfsError::kIoError,
                     "the automount service is still registered as pending "
                     "deletion; reboot or retry")
          .Metadata("installer");
    }
    Sleep(200);
  }
}

Status SaveShutdownFailureRecord(const DeploymentPaths& paths,
                                 const std::vector<std::string>& failures) {
  if (paths.installer.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "the installer state directory is not configured")
        .Metadata("installer");
  }
  std::ostringstream out;
  out << "xfswin-shutdown 1\n"
      << "recorded_utc=" << NowUtc() << "\n";
  for (const std::string& failure : failures) {
    out << "failure=" << SanitizeField(failure) << "\n";
  }
  return WriteTextFileAtomic(paths.installer / L"shutdown.ini", out.str());
}

namespace {

std::vector<std::string> ReadShutdownFailures(const DeploymentPaths& paths) {
  std::vector<std::string> failures;
  auto content = ReadTextFile(paths.installer / L"shutdown.ini");
  if (!content.ok()) return failures;
  for (const std::string& line : SplitLines(content.value())) {
    if (line.rfind("failure=", 0) == 0) {
      failures.push_back(line.substr(std::strlen("failure=")));
    }
  }
  return failures;
}

}  // namespace

// ---------------------------------------------------------------------------
// Mount start gate
// ---------------------------------------------------------------------------

MountStartGate::MountStartGate() = default;
MountStartGate::~MountStartGate() { Release(); }

Status MountStartGate::Acquire(const DeploymentPaths& paths,
                               const std::wstring& mount_point,
                               const std::wstring& owner_executable) {
  Release();
  if (paths.root.empty() || paths.sessions.empty()) return OkStatus();
  std::error_code exists_error;
  if (!std::filesystem::exists(paths.sessions, exists_error)) {
    Status ensured = EnsureDeploymentPaths(paths);
    if (!ensured.ok()) return ensured;
  }
  auto mutex = OpenStartupMutex(10000);
  if (!mutex.ok()) return mutex.error();

  MaintenanceFenceInfo fence;
  Status fence_status = ReadMaintenanceFence(paths, &fence);
  if (!fence_status.ok() || fence.exists) {
    const std::string detail =
        fence_status.ok()
            ? "maintenance fence is active (" + fence.operation + ")"
            : "maintenance fence cannot be read (" +
                  fence_status.error().Format() + ")";
    return XFS_ERR(XfsError::kPermissionDenied,
                   "new XFS mounts are refused: " + detail +
                       "; finish or roll back the installation first")
        .Metadata("maintenance-fence");
  }

  const std::string mount_text = WideToUtf8(mount_point);
  std::wstring lease_name;
  HANDLE lease = INVALID_HANDLE_VALUE;
  DWORD create_error = ERROR_SUCCESS;
  static std::atomic<uint64_t> sequence{0};
  for (int attempt = 0; attempt < 16; ++attempt) {
    const uint64_t serial = ++sequence;
    const std::string name =
        std::string(kLeasePrefix) + std::to_string(GetCurrentProcessId()) +
        "-" + std::to_string(static_cast<unsigned long long>(serial)) + "-" +
        std::to_string(attempt) + kLeaseSuffix;
    const std::wstring full =
        (paths.sessions / std::filesystem::path(Utf8ToWide(name))).wstring();
    lease = CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lease != INVALID_HANDLE_VALUE) {
      lease_name = full;
      break;
    }
    create_error = GetLastError();
    if (create_error != ERROR_FILE_EXISTS &&
        create_error != ERROR_ALREADY_EXISTS) {
      break;
    }
  }
  if (lease == INVALID_HANDLE_VALUE) {
    return Win32Failure("create the mount session lease", create_error);
  }
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(kLeaseLockOffset);
  if (!LockFileEx(lease, LOCKFILE_EXCLUSIVE_LOCK, 0, kLeaseLockBytes, 0,
                  &overlapped)) {
    const DWORD code = GetLastError();
    CloseHandle(lease);
    DeleteFileW(lease_name.c_str());
    return Win32Failure("lock the mount session lease", code);
  }
  if (!WriteLeaseContent(lease,
                         FormatLease(GetCurrentProcessId(), mount_text,
                                     WideToUtf8(owner_executable)))) {
    const DWORD code = GetLastError();
    CloseHandle(lease);
    DeleteFileW(lease_name.c_str());
    return Win32Failure("write the mount session lease", code);
  }
  // Re-check under the same mutex: if the fence appeared, remove the lease and
  // refuse.  The mutex guarantees the installer did not create the fence
  // between the first check and the lease creation.
  MaintenanceFenceInfo second;
  const Status second_status = ReadMaintenanceFence(paths, &second);
  if (!second_status.ok() || second.exists) {
    CloseHandle(lease);
    DeleteFileW(lease_name.c_str());
    return XFS_ERR(XfsError::kPermissionDenied,
                   second.exists
                       ? "new XFS mounts are refused: maintenance fence is "
                         "active"
                       : "new XFS mounts are refused: the maintenance fence "
                         "cannot be read")
        .Metadata("maintenance-fence");
  }
  lease_handle_ = lease;
  session_file_ = WideToUtf8(std::filesystem::path(lease_name).filename());
  paths_ = paths;
  return OkStatus();
}

void MountStartGate::Release() {
  if (lease_handle_) {
    HANDLE lease = static_cast<HANDLE>(lease_handle_);
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(kLeaseLockOffset);
    UnlockFileEx(lease, 0, kLeaseLockBytes, 0, &overlapped);
    CloseHandle(lease);
    lease_handle_ = nullptr;
  }
  if (!session_file_.empty()) {
    std::error_code ec;
    std::filesystem::remove(
        paths_.sessions / std::filesystem::path(Utf8ToWide(session_file_)),
        ec);
    session_file_.clear();
  }
  mutex_handle_ = nullptr;
}

// ---------------------------------------------------------------------------
// Install transaction state file
// ---------------------------------------------------------------------------

namespace {

std::string SerializeCapture(const InstallerCapture& capture) {
  std::ostringstream out;
  out << "xfswin-installer-state 1\n"
      << "state_version=" << capture.format << "\n"
      << "action=" << ToString(capture.action) << "\n"
      << "install_path=" << SanitizeField(capture.install_path) << "\n"
      << "product_version=" << SanitizeField(capture.product_version) << "\n"
      << "architecture=" << SanitizeField(capture.architecture) << "\n"
      << "created_utc=" << SanitizeField(capture.created_utc) << "\n"
      << "service_context_captured="
      << (capture.service_context_captured ? "1" : "0") << "\n"
      << "original_service_installed="
      << (capture.original_service.installed ? "1" : "0") << "\n"
      << "original_service_running="
      << (capture.original_service.running ? "1" : "0") << "\n"
      << "original_service_binary="
      << SanitizeField(WideToUtf8(capture.original_service.binary_path))
      << "\n"
      << "original_service_start_type=" << capture.original_service.start_type
      << "\n"
      << "original_service_delayed="
      << (capture.original_service.delayed_auto_start ? "1" : "0") << "\n"
      << "original_service_account="
      << SanitizeField(WideToUtf8(capture.original_service.account)) << "\n";
  for (const auto& dependency : capture.original_service.dependencies) {
    out << "original_service_dependency="
        << SanitizeField(WideToUtf8(dependency)) << "\n";
  }
  for (const auto& warning : capture.warnings) {
    out << "warning=" << SanitizeField(warning) << "\n";
  }
  return out.str();
}

Result<InstallerCapture> ParseCapture(const std::string& content) {
  InstallerCapture capture;
  bool header = false;
  for (const std::string& line : SplitLines(content)) {
    if (line.empty()) continue;
    if (line == "xfswin-installer-state 1") {
      header = true;
      continue;
    }
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (key == "state_version") {
      capture.format =
          static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (key == "action") {
      InstallerAction action;
      if (ParseInstallerAction(value, &action)) capture.action = action;
    } else if (key == "install_path") {
      capture.install_path = value;
    } else if (key == "product_version") {
      capture.product_version = value;
    } else if (key == "architecture") {
      capture.architecture = value;
    } else if (key == "created_utc") {
      capture.created_utc = value;
    } else if (key == "service_context_captured") {
      capture.service_context_captured = value == "1";
    } else if (key == "original_service_installed") {
      capture.original_service.installed = value == "1";
    } else if (key == "original_service_running") {
      capture.original_service.running = value == "1";
    } else if (key == "original_service_binary") {
      capture.original_service.binary_path = Utf8ToWide(value);
    } else if (key == "original_service_start_type") {
      capture.original_service.start_type =
          static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (key == "original_service_delayed") {
      capture.original_service.delayed_auto_start = value == "1";
    } else if (key == "original_service_account") {
      capture.original_service.account = Utf8ToWide(value);
    } else if (key == "original_service_dependency") {
      capture.original_service.dependencies.push_back(Utf8ToWide(value));
    } else if (key == "warning") {
      capture.warnings.push_back(value);
    }
  }
  if (!header) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "the installer rollback state has an unknown format")
        .Metadata("installer");
  }
  return capture;
}

Status ReadStateFile(const DeploymentPaths& paths, InstallerCapture* capture) {
  auto content = ReadTextFile(StateFilePath(paths));
  if (!content.ok()) {
    return XFS_ERR(XfsError::kNotFound,
                   "no installer rollback state exists for this image")
        .Metadata("installer");
  }
  auto parsed = ParseCapture(content.value());
  if (!parsed.ok()) return parsed.error();
  if (capture) *capture = std::move(parsed.value());
  return OkStatus();
}

Status WriteStateFile(const DeploymentPaths& paths,
                      const InstallerCapture& capture) {
  return WriteTextFileAtomic(StateFilePath(paths), SerializeCapture(capture));
}

Status RequireActiveTransaction(const DeploymentPaths& paths,
                                const InstallerOptions& options,
                                InstallerCapture* capture) {
  MaintenanceFenceInfo fence;
  Status fence_status = ReadMaintenanceFence(paths, &fence);
  if (!fence_status.ok()) return fence_status;
  if (!fence.exists) {
    return XFS_ERR(XfsError::kNotMounted,
                   "no maintenance fence exists; run `xfswin installer "
                   "prepare` first")
        .Metadata("installer");
  }
  if (!fence.operation.empty() &&
      fence.operation != ToString(options.action)) {
    return XFS_ERR(XfsError::kAlreadyExists,
                   "the outstanding maintenance operation is '" +
                       fence.operation + "', not '" +
                       ToString(options.action) + "'")
        .Metadata("installer");
  }
  InstallerCapture state;
  Status read = ReadStateFile(paths, &state);
  if (!read.ok()) return read;
  if (state.action != options.action) {
    return XFS_ERR(XfsError::kBadValue,
                   "the captured rollback state belongs to a " +
                       std::string(ToString(state.action)) + " operation")
        .Metadata("installer");
  }
  if (!state.install_path.empty() && !options.install_path.empty() &&
      !SamePath(Utf8ToWide(state.install_path),
                options.install_path.wstring())) {
    return XFS_ERR(XfsError::kBadValue,
                   "the captured rollback state belongs to " +
                       state.install_path)
        .Metadata("installer");
  }
  if (capture) *capture = std::move(state);
  return OkStatus();
}

InstallerResult BaseResult(const InstallerOptions& options,
                           const std::string& install_path) {
  InstallerResult result;
  result.action = options.action;
  result.operation = ToString(options.action);
  result.install_path = install_path;
  result.product_version = ProductVersion();
  result.architecture = BuildArchitecture();
  result.service_absent_allowed = options.allow_service_absent;
  return result;
}

InstallerResult FailResult(InstallerResult result, const Status& status,
                           int64_t exit_code = 1) {
  result.ok = false;
  result.exit_code = exit_code;
  result.error = status;
  result.messages.push_back(status.error().Format());
  return result;
}

DeploymentPaths ResolveStateRoot(const InstallerOptions& options) {
  const std::filesystem::path root =
      options.state_root.empty() ? DefaultDeploymentPaths().root
                                 : options.state_root;
  return ResolveDeploymentPaths(root);
}

void AttachPaths(InstallerResult* result, const DeploymentPaths& paths) {
  result->state_file = StateFilePath(paths);
  result->fence_file = paths.fence;
}

bool RuntimeAcceptable(const WinFspRuntimeInfo& runtime) {
  return runtime.available && runtime.major == 2;
}

Status RequireRuntime(const WinFspRuntimeInfo& runtime, bool allow_missing) {
  if (RuntimeAcceptable(runtime)) return OkStatus();
  if (allow_missing && runtime.dll_path.empty()) return OkStatus();
  const std::string message =
      runtime.diagnostic.empty() ? "the WinFsp 2.x runtime is not usable"
                                 : runtime.diagnostic;
  const XfsError kind = runtime.dll_path.empty()
                            ? XfsError::kNotFound
                            : XfsError::kUnsupportedFeature;
  return XFS_ERR(kind, message).Metadata("winfsp");
}

Status VerifyServiceMatches(const InstallerServiceConfig& service,
                            const std::filesystem::path& exe,
                            bool require_running) {
  if (!service.installed) {
    return XFS_ERR(XfsError::kNotFound,
                   "the automount service is not installed")
        .Metadata("installer");
  }
  if (!SamePath(service.binary_path, exe.wstring())) {
    return XFS_ERR(XfsError::kBadValue,
                   "the automount service runs " +
                       WideToUtf8(service.binary_path) + ", not " +
                       WideToUtf8(exe.wstring()))
        .Metadata("installer");
  }
  if (service.start_type != SERVICE_AUTO_START) {
    return XFS_ERR(XfsError::kBadValue,
                   "the automount service is not configured for automatic "
                   "start")
        .Metadata("installer");
  }
  if (service.stop_pending || service.start_pending) {
    return XFS_ERR(XfsError::kIoError,
                   std::string("the automount service is ") +
                       ToString(service.state))
        .Metadata("installer");
  }
  if (require_running && !service.running) {
    return XFS_ERR(XfsError::kNotMounted,
                   "the automount service is not running")
        .Metadata("installer");
  }
  return OkStatus();
}

InstallerServiceSnapshot SnapshotService(const InstallerServiceConfig& config) {
  InstallerServiceSnapshot snapshot;
  snapshot.installed = config.installed;
  snapshot.running = config.running;
  snapshot.binary_path = config.binary_path;
  snapshot.start_type = config.start_type;
  snapshot.delayed_auto_start = config.delayed_auto_start;
  snapshot.account = config.account;
  snapshot.dependencies = config.dependencies;
  return snapshot;
}

}  // namespace

// ---------------------------------------------------------------------------
// Installed payload inspection
// ---------------------------------------------------------------------------

namespace {

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
  if (a.size() != b.size()) return false;
  for (size_t index = 0; index < a.size(); ++index) {
    if (towlower(a[index]) != towlower(b[index])) return false;
  }
  return true;
}

// Reads one string field from the flat version.json the build produces.  This
// is deliberately not a general JSON parser: the manifest is generated by the
// build, and a field we cannot read is reported as empty (the caller then
// refuses the package instead of trusting it).
std::string JsonStringField(const std::string& text, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t position = text.find(needle);
  if (position == std::string::npos) return {};
  position = text.find(':', position + needle.size());
  if (position == std::string::npos) return {};
  ++position;
  while (position < text.size() &&
         (text[position] == ' ' || text[position] == '\t' ||
          text[position] == '\r' || text[position] == '\n')) {
    ++position;
  }
  if (position >= text.size() || text[position] != '"') return {};
  ++position;
  std::string value;
  for (; position < text.size(); ++position) {
    const char c = text[position];
    if (c == '"') break;
    if (c == '\\' && position + 1 < text.size()) {
      const char escaped = text[++position];
      switch (escaped) {
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: value.push_back(escaped); break;
      }
      continue;
    }
    value.push_back(c);
  }
  return value;
}

}  // namespace

Result<std::filesystem::path> NormalizeInstallPath(
    const std::filesystem::path& path) {
  if (path.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "an install path is required")
        .Metadata("installer");
  }
  if (!path.is_absolute()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "the install path must be absolute: " +
                       WideToUtf8(path.wstring()))
        .Metadata("installer");
  }
  const std::filesystem::path normalized = path.lexically_normal();
  if (!EqualsIgnoreCase(normalized.filename().wstring(), L"xfswin.exe")) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "the install path must name xfswin.exe: " +
                       WideToUtf8(path.wstring()))
        .Metadata("installer");
  }
  return normalized;
}

InstalledVersionInfo ReadInstalledVersion(const std::filesystem::path& exe) {
  InstalledVersionInfo info;
  if (exe.empty()) return info;
  auto content = ReadTextFile(exe.parent_path() / L"version.json");
  if (!content.ok()) return info;
  info.present = true;
  info.version = JsonStringField(content.value(), "version");
  info.git_sha = JsonStringField(content.value(), "git_sha");
  info.architecture = JsonStringField(content.value(), "architecture");
  return info;
}

// ---------------------------------------------------------------------------
// prepare / apply / remove / verify / commit / rollback
// ---------------------------------------------------------------------------

InstallerResult PrepareInstaller(const InstallerOptions& options) {
  auto normalized = NormalizeInstallPath(options.install_path);
  if (!normalized.ok()) {
    return FailResult(BaseResult(options, options.install_path.string()),
                     normalized.error());
  }
  const std::filesystem::path exe = normalized.value();
  InstallerResult result = BaseResult(options, WideToUtf8(exe.wstring()));
  const DeploymentPaths paths = ResolveStateRoot(options);
  AttachPaths(&result, paths);

  Status secured = EnsureDeploymentPaths(paths);
  if (!secured.ok()) return FailResult(result, secured);

  auto lock = OpenInstallerMutex(60000);
  if (!lock.ok()) return FailResult(result, lock.error());

  // Capture (or adopt) the original service context.  The capture is written
  // before the service is touched so a failed stop can still be rolled back.
  InstallerServiceConfig service;
  Status queried = QueryInstallerService(&service);
  if (!queried.ok()) return FailResult(result, queried);
  if (service.installed && !SamePath(service.binary_path, exe.wstring())) {
    result.blockers.push_back(
        "the automount service already belongs to " +
        WideToUtf8(service.binary_path) + ", not " + result.install_path);
    return FailResult(
        result,
        XFS_ERR(XfsError::kAlreadyExists,
                "refusing to take over another installation's automount "
                "service")
            .Metadata("installer"));
  }
  result.service = service;

  result.winfsp = ProbeWinFspRuntime();
  if (options.action == InstallerAction::kUninstall) {
    // Removing the product must not depend on the shared runtime still being
    // healthy: without WinFsp no new session can start, so only the service
    // and the files remain to be removed.
    if (!result.winfsp.available) {
      result.warnings.push_back(
          "WinFsp is not installed; the uninstall continues without it");
    }
  } else {
    Status runtime = RequireRuntime(result.winfsp, false);
    if (!runtime.ok()) return FailResult(result, runtime);
  }

  if (!PathExists(exe)) {
    if (options.action == InstallerAction::kInstall) {
      result.warnings.push_back(
          "the target xfswin.exe is not installed yet; prepare runs before "
          "InstallFiles");
    } else {
      return FailResult(
          result,
          XFS_ERR(XfsError::kNotFound,
                  "the installed xfswin.exe does not exist at " +
                      result.install_path)
              .Metadata("installer"));
    }
  } else {
    std::string arch_error;
    if (!ValidateExecutableArchitecture(exe, &arch_error)) {
      return FailResult(result,
                        XFS_ERR(XfsError::kInvalidArgument,
                                "the installed xfswin.exe is not usable: " +
                                    arch_error)
                            .Metadata("installer"));
    }
    const InstalledVersionInfo installed = ReadInstalledVersion(exe);
    if (installed.present && !installed.version.empty() &&
        installed.version != ProductVersion()) {
      result.warnings.push_back(
          "the existing installation reports version " + installed.version +
          " (this package is " + ProductVersion() + ")");
    }
  }

  MaintenanceFenceInfo fence;
  Status fence_status = ReadMaintenanceFence(paths, &fence);
  if (!fence_status.ok()) return FailResult(result, fence_status);
  bool state_matches = false;
  InstallerCapture capture;
  if (fence.exists) {
    if (fence.operation != ToString(options.action) ||
        (!fence.install_path.empty() &&
         !SamePath(Utf8ToWide(fence.install_path), exe.wstring()))) {
      result.blockers.push_back(
          "another maintenance operation is in progress: " + fence.operation +
          " " + fence.install_path);
      return FailResult(
          result,
          XFS_ERR(XfsError::kAlreadyExists,
                  "a different installer transaction is already in progress")
              .Metadata("installer"));
    }
    auto existing = ReadTextFile(StateFilePath(paths));
    if (existing.ok()) {
      auto parsed = ParseCapture(existing.value());
      if (!parsed.ok()) return FailResult(result, parsed.error());
      capture = std::move(parsed.value());
      if (capture.action != options.action ||
          (!capture.install_path.empty() &&
           !SamePath(Utf8ToWide(capture.install_path), exe.wstring()))) {
        return FailResult(
            result,
            XFS_ERR(XfsError::kBadValue,
                    "the captured rollback state belongs to a different "
                    "operation")
                .Metadata("installer"));
      }
      state_matches = true;
      result.warnings.push_back(
          "resuming an interrupted maintenance operation; the original "
          "service snapshot is preserved");
    }
  }
  if (!state_matches) {
    capture.format = 1;
    capture.action = options.action;
    capture.install_path = result.install_path;
    capture.product_version = ProductVersion();
    capture.architecture = BuildArchitecture();
    capture.created_utc = NowUtc();
    capture.service_context_captured = true;
    capture.original_service = SnapshotService(service);
    for (const std::string& warning : result.warnings) {
      capture.warnings.push_back(warning);
    }
    Status written = WriteStateFile(paths, capture);
    if (!written.ok()) return FailResult(result, written);
  }

  if (!fence.exists) {
    // A committed or rolled-back transaction clears the apply marker; this
    // only drops one left behind by an interrupted attempt so it cannot make
    // this transaction's rollback stop an untouched service.  A resumed
    // transaction (fence exists) keeps its marker.
    RemoveApplyMarker(paths);
    Status fenced = WriteFenceLocked(paths, options, result.install_path);
    if (!fenced.ok()) return FailResult(result, fenced);
  }

  // Before stopping the service, classify live sessions.  Ownership is the
  // SCM-reported service PID, never a matching image path.
  //
  // Upgrade refuses while ANY lease is live, service-owned included: it
  // replaces the image the running service and every manual mount execute, so
  // it cannot rely on a clean-stop contract it does not control.  Install
  // refuses sessions it does not own.  Uninstall may clean-stop the owned
  // service (its sessions end with it) and refuses whatever remains.
  auto scan = ScanActiveSessions(paths, exe.wstring());
  if (!scan.ok()) return FailResult(result, scan.error());
  for (const std::string& warning : scan.value().warnings) {
    result.warnings.push_back(warning);
  }
  DWORD service_pid = 0;
  const bool service_running = RunningServiceProcessId(&service_pid);
  std::vector<std::string> active_sessions;
  std::vector<std::string> manual_sessions;
  for (const auto& session : scan.value().sessions) {
    const bool is_service_session =
        service_running && session.pid == service_pid;
    std::string text = session.mount_point + " (pid " +
                       std::to_string(session.pid) + ", " + session.executable;
    if (is_service_session) text += ", automount service";
    text += ")";
    active_sessions.push_back(text);
    if (!is_service_session) manual_sessions.push_back(std::move(text));
  }
  if (options.action == InstallerAction::kUpgrade && !active_sessions.empty()) {
    result.remaining_sessions = active_sessions;
    result.blockers.push_back(
        "all active mount sessions must be unmounted before an upgrade, "
        "including sessions owned by the automount service");
    return FailResult(
        result,
        XFS_ERR(XfsError::kPermissionDenied,
                "active mount sessions are still live: " +
                    active_sessions.front())
            .Metadata("installer"));
  }
  if (options.action == InstallerAction::kInstall && !manual_sessions.empty()) {
    result.remaining_sessions = manual_sessions;
    result.blockers.push_back(
        "active mount sessions must be unmounted before installing");
    return FailResult(
        result,
        XFS_ERR(XfsError::kPermissionDenied,
                "manual mount sessions are still active: " +
                    manual_sessions.front())
            .Metadata("installer"));
  }
  if (options.action == InstallerAction::kUninstall &&
      !manual_sessions.empty()) {
    result.warnings.push_back(
        "manual mount sessions will remain after the service stops: " +
        manual_sessions.front());
  }
  if (options.action == InstallerAction::kUninstall &&
      active_sessions.size() > manual_sessions.size()) {
    result.messages.push_back(
        std::to_string(active_sessions.size() - manual_sessions.size()) +
        " service-owned mount session(s) will be clean-stopped with the "
        "automount service");
  }

  std::string stop_message;
  Status stopped = StopInstallerService(&stop_message);
  if (!stop_message.empty()) result.messages.push_back(stop_message);
  if (!stopped.ok()) {
    const std::vector<std::string> failures = ReadShutdownFailures(paths);
    for (const std::string& failure : failures) {
      result.messages.push_back("shutdown record: " + failure);
    }
    result.restore_state = "restoration-pending";
    result.recoverable_record = paths.installer / L"shutdown.ini";
    return FailResult(result, stopped);
  }

  ActiveSessionScan remaining;
  if (!WaitForSessionsGone(paths, exe.wstring(), options.stop_timeout_ms,
                           &remaining)) {
    for (const auto& session : remaining.sessions) {
      result.remaining_sessions.push_back(
          session.mount_point + " (pid " + std::to_string(session.pid) + ")");
    }
    for (const std::string& warning : remaining.warnings) {
      result.warnings.push_back(warning);
    }
    result.blockers.push_back(
        "mount sessions did not end after the service stopped");
    return FailResult(
        result,
        XFS_ERR(XfsError::kIoError,
                "mount sessions are still active after the service stop")
            .Metadata("installer"));
  }
  for (const std::string& warning : remaining.warnings) {
    result.warnings.push_back(warning);
  }

  Status refreshed = QueryInstallerService(&result.service);
  if (!refreshed.ok()) return FailResult(result, refreshed);
  const std::vector<std::string> failures = ReadShutdownFailures(paths);
  for (const std::string& failure : failures) {
    result.messages.push_back("shutdown record: " + failure);
    result.recoverable_record = paths.installer / L"shutdown.ini";
  }
  result.ok = true;
  result.exit_code = 0;
  result.restore_state = "maintenance-active";
  result.messages.push_back(
      "maintenance fence active; the automount service is " +
      std::string(ToString(result.service.state)));
  return result;
}

InstallerResult ApplyInstaller(const InstallerOptions& options) {
  auto normalized = NormalizeInstallPath(options.install_path);
  if (!normalized.ok()) {
    return FailResult(BaseResult(options, options.install_path.string()),
                     normalized.error());
  }
  const std::filesystem::path exe = normalized.value();
  InstallerResult result = BaseResult(options, WideToUtf8(exe.wstring()));
  const DeploymentPaths paths = ResolveStateRoot(options);
  AttachPaths(&result, paths);

  Status secured = EnsureDeploymentPaths(paths);
  if (!secured.ok()) return FailResult(result, secured);
  auto lock = OpenInstallerMutex(60000);
  if (!lock.ok()) return FailResult(result, lock.error());

  InstallerOptions scoped = options;
  scoped.install_path = exe;
  InstallerCapture capture;
  Status active = RequireActiveTransaction(paths, scoped, &capture);
  if (!active.ok()) return FailResult(result, active);
  if (options.action == InstallerAction::kUninstall) {
    return FailResult(
        result,
        XFS_ERR(XfsError::kInvalidArgument,
                "apply does not handle uninstall; use remove")
            .Metadata("installer"));
  }

  if (!PathExists(exe)) {
    return FailResult(
        result,
        XFS_ERR(XfsError::kNotFound,
                "the installed xfswin.exe does not exist at " +
                    result.install_path)
            .Metadata("installer"));
  }
  std::string arch_error;
  if (!ValidateExecutableArchitecture(exe, &arch_error)) {
    return FailResult(result,
                      XFS_ERR(XfsError::kInvalidArgument,
                              "the installed xfswin.exe is not usable: " +
                                  arch_error)
                          .Metadata("installer"));
  }
  result.winfsp = ProbeWinFspRuntime();
  Status runtime = RequireRuntime(result.winfsp, false);
  if (!runtime.ok()) return FailResult(result, runtime);

  const std::string failure = options.injected_failure;
  if (failure == "before-service" || failure == "after-files") {
    return FailResult(
        result,
        XFS_ERR(XfsError::kIoError, "injected installer failure: " + failure)
            .Metadata("installer"));
  }

  InstallerServiceConfig before;
  Status queried = QueryInstallerService(&before);
  if (!queried.ok()) return FailResult(result, queried);
  if (before.installed && !SamePath(before.binary_path, exe.wstring())) {
    return FailResult(
        result,
        XFS_ERR(XfsError::kAlreadyExists,
                "the automount service belongs to " +
                    WideToUtf8(before.binary_path))
            .Metadata("installer"));
  }
  const bool existed = before.installed;

  // From here on this transaction mutates the service (create, reconfigure or
  // start it), so a rollback must stop it before MSI restores the files.  The
  // marker is written before the first SCM change and after the failure
  // injections, which means a transaction that never reached the service step
  // leaves it absent and rollback-quiesce stays a no-op.
  Status marked = WriteApplyMarker(paths, options.action);
  if (!marked.ok()) return FailResult(result, marked);

  Status ensured = EnsureInstallerService(exe.wstring(), kServiceDisplayName,
                                          &result.service);
  if (!ensured.ok()) return FailResult(result, ensured);
  result.service_created = !existed;
  result.service_updated =
      existed && (before.raw_image_path != result.service.raw_image_path ||
                  before.start_type != result.service.start_type);

  std::string start_message;
  Status started = StartInstallerService(&start_message);
  if (!start_message.empty()) result.messages.push_back(start_message);
  if (!started.ok()) return FailResult(result, started);

  // A failure after the service started is followed by MSI rollback, which
  // must be able to replace the files.  Stop the service before returning so
  // the rollback action restores the captured state (or deletes a service
  // this transaction created) instead of failing on a running image.
  auto stop_after_failure = [&result]() {
    std::string stop_message;
    const Status stopped = StopInstallerService(&stop_message);
    if (!stop_message.empty()) result.messages.push_back(stop_message);
    if (!stopped.ok()) {
      result.messages.push_back(
          "the started automount service could not be stopped: " +
          stopped.error().Format());
    }
  };

  Status refreshed = QueryInstallerService(&result.service);
  if (!refreshed.ok()) {
    stop_after_failure();
    return FailResult(result, refreshed);
  }

  Status matches = VerifyServiceMatches(result.service, exe, true);
  if (!matches.ok()) {
    stop_after_failure();
    return FailResult(result, matches);
  }

  if (failure == "service-start") {
    result.messages.push_back(
        "injected installer failure: service-start (service was started)");
    stop_after_failure();
    return FailResult(
        result,
        XFS_ERR(XfsError::kIoError,
                "injected installer failure: service-start")
            .Metadata("installer"));
  }

  const InstalledVersionInfo installed = ReadInstalledVersion(exe);
  if (!installed.present) {
    stop_after_failure();
    return FailResult(
        result,
        XFS_ERR(XfsError::kNotFound,
                "version.json is missing next to xfswin.exe; refusing to "
                "verify a stale or unrelated package")
            .Metadata("installer"));
  }
  if (installed.version != ProductVersion()) {
    stop_after_failure();
    return FailResult(
        result,
        XFS_ERR(XfsError::kBadValue,
                "the installed payload reports version " + installed.version +
                    ", expected " + ProductVersion())
            .Metadata("installer"));
  }
  if (!installed.architecture.empty() &&
      !SamePath(Utf8ToWide(installed.architecture),
                Utf8ToWide(BuildArchitecture()))) {
    stop_after_failure();
    return FailResult(
        result,
        XFS_ERR(XfsError::kBadValue,
                "the installed payload is " + installed.architecture +
                    ", expected " + BuildArchitecture())
            .Metadata("installer"));
  }

  result.ok = true;
  result.exit_code = 0;
  result.restore_state = "maintenance-active";
  result.messages.push_back("the automount service is installed and running");
  return result;
}

InstallerResult RemoveInstaller(const InstallerOptions& options) {
  auto normalized = NormalizeInstallPath(options.install_path);
  if (!normalized.ok()) {
    return FailResult(BaseResult(options, options.install_path.string()),
                     normalized.error());
  }
  const std::filesystem::path exe = normalized.value();
  InstallerResult result = BaseResult(options, WideToUtf8(exe.wstring()));
  const DeploymentPaths paths = ResolveStateRoot(options);
  AttachPaths(&result, paths);

  Status secured = EnsureDeploymentPaths(paths);
  if (!secured.ok()) return FailResult(result, secured);
  auto lock = OpenInstallerMutex(60000);
  if (!lock.ok()) return FailResult(result, lock.error());

  InstallerOptions scoped = options;
  scoped.install_path = exe;
  InstallerCapture capture;
  Status active = RequireActiveTransaction(paths, scoped, &capture);
  if (!active.ok()) return FailResult(result, active);
  if (options.action != InstallerAction::kUninstall) {
    return FailResult(
        result,
        XFS_ERR(XfsError::kInvalidArgument,
                "remove handles only the uninstall action")
            .Metadata("installer"));
  }

  if (options.injected_failure == "before-service") {
    return FailResult(
        result,
        XFS_ERR(XfsError::kIoError,
                "injected installer failure: before-service")
            .Metadata("installer"));
  }

  InstallerServiceConfig service;
  Status queried = QueryInstallerService(&service);
  if (!queried.ok()) return FailResult(result, queried);
  result.service = service;
  if (service.installed && !SamePath(service.binary_path, exe.wstring())) {
    return FailResult(
        result,
        XFS_ERR(XfsError::kAlreadyExists,
                "the automount service belongs to " +
                    WideToUtf8(service.binary_path) +
                    "; refusing to delete it")
            .Metadata("installer"));
  }

  // This transaction is about to stop and delete the service; rollback may
  // therefore stop it.  A failure injected before this point leaves the
  // marker absent, so rollback-quiesce does not touch the running service.
  Status marked = WriteApplyMarker(paths, options.action);
  if (!marked.ok()) return FailResult(result, marked);

  std::string stop_message;
  Status stopped = StopInstallerService(&stop_message);
  if (!stop_message.empty()) result.messages.push_back(stop_message);
  if (!stopped.ok()) {
    const std::vector<std::string> failures = ReadShutdownFailures(paths);
    for (const std::string& failure : failures) {
      result.messages.push_back("shutdown record: " + failure);
    }
    result.restore_state = "restoration-pending";
    result.recoverable_record = paths.installer / L"shutdown.ini";
    return FailResult(result, stopped);
  }

  bool pending = false;
  Status deleted = DeleteInstallerService(&pending);
  if (!deleted.ok()) return FailResult(result, deleted);
  // DeleteService only marks the service for deletion; the SCM keeps the
  // registration until the last handle closes.  Wait for it to forget the
  // service so the final absent check (and the MSI transaction) cannot race
  // that window.  This also covers pending-deletion from an earlier attempt.
  Status waited = WaitInstallerServiceDeleted(30000);
  if (!waited.ok()) return FailResult(result, waited);

  ActiveSessionScan remaining;
  if (!WaitForSessionsGone(paths, exe.wstring(), options.stop_timeout_ms,
                           &remaining)) {
    for (const auto& session : remaining.sessions) {
      result.remaining_sessions.push_back(
          session.mount_point + " (pid " + std::to_string(session.pid) + ")");
    }
    result.blockers.push_back(
        "manual mount sessions must be unmounted before uninstalling");
    return FailResult(
        result,
        XFS_ERR(XfsError::kIoError,
                "mount sessions are still active after the service stop")
            .Metadata("installer"));
  }

  Status refreshed = QueryInstallerService(&result.service);
  if (!refreshed.ok()) return FailResult(result, refreshed);
  if (result.service.installed) {
    return FailResult(
        result,
        XFS_ERR(XfsError::kIoError,
                "the automount service is still registered after removal")
            .Metadata("installer"));
  }
  result.ok = true;
  result.exit_code = 0;
  result.restore_state = "maintenance-active";
  result.messages.push_back("the automount service was removed");
  return result;
}

InstallerResult VerifyInstaller(const InstallerOptions& options) {
  auto normalized = NormalizeInstallPath(options.install_path);
  if (!normalized.ok()) {
    return FailResult(BaseResult(options, options.install_path.string()),
                     normalized.error());
  }
  const std::filesystem::path exe = normalized.value();
  InstallerResult result = BaseResult(options, WideToUtf8(exe.wstring()));
  const DeploymentPaths paths = ResolveStateRoot(options);
  AttachPaths(&result, paths);

  if (options.action != InstallerAction::kUninstall) {
    if (!PathExists(exe)) {
      return FailResult(
          result,
          XFS_ERR(XfsError::kNotFound,
                  "the installed xfswin.exe does not exist at " +
                      result.install_path)
              .Metadata("installer"));
    }
    std::string arch_error;
    if (!ValidateExecutableArchitecture(exe, &arch_error)) {
      return FailResult(result,
                        XFS_ERR(XfsError::kInvalidArgument,
                                "the installed xfswin.exe is not usable: " +
                                    arch_error)
                            .Metadata("installer"));
    }
    const InstalledVersionInfo installed = ReadInstalledVersion(exe);
    if (!installed.present) {
      return FailResult(
          result,
          XFS_ERR(XfsError::kNotFound,
                  "version.json is missing next to xfswin.exe; refusing to "
                  "verify a stale or unrelated package")
              .Metadata("installer"));
    }
    if (installed.version != ProductVersion()) {
      return FailResult(
          result,
          XFS_ERR(XfsError::kBadValue,
                  "the installed payload reports version " +
                      installed.version + ", expected " + ProductVersion())
              .Metadata("installer"));
    }
    if (!installed.architecture.empty() &&
        !SamePath(Utf8ToWide(installed.architecture),
                  Utf8ToWide(BuildArchitecture()))) {
      return FailResult(
          result,
          XFS_ERR(XfsError::kBadValue,
                  "the installed payload is " + installed.architecture +
                      ", expected " + BuildArchitecture())
              .Metadata("installer"));
    }
    result.winfsp = ProbeWinFspRuntime();
    Status runtime = RequireRuntime(result.winfsp, false);
    if (!runtime.ok()) return FailResult(result, runtime);
  }

  InstallerServiceConfig service;
  Status queried = QueryInstallerService(&service);
  if (!queried.ok()) return FailResult(result, queried);
  result.service = service;

  if (options.action == InstallerAction::kUninstall) {
    if (service.installed) {
      return FailResult(
          result,
          XFS_ERR(XfsError::kAlreadyExists,
                  "the automount service is still installed")
              .Metadata("installer"));
    }
    result.ok = true;
    result.exit_code = 0;
    result.messages.push_back(
        "the automount service is absent; uninstall verification passed");
    return result;
  }

  if (!service.installed && options.allow_service_absent) {
    result.warnings.push_back(
        "the automount service is absent; verification allowed by request");
    result.ok = true;
    result.exit_code = 0;
    return result;
  }
  Status matches = VerifyServiceMatches(service, exe, true);
  if (!matches.ok()) return FailResult(result, matches);

  // Installer-only failure injection for the standalone verify action.  It
  // runs after the real verification passed so the MSI rollback path sees a
  // running, fully applied product and must clean-stop it before file restore.
  if (options.injected_failure == "verify") {
    return FailResult(
        result,
        XFS_ERR(XfsError::kIoError, "injected installer failure: verify")
            .Metadata("installer"));
  }

  result.ok = true;
  result.exit_code = 0;
  result.messages.push_back(
      "the automount service matches the installed xfswin.exe and is running");
  return result;
}

InstallerResult CommitInstaller(const InstallerOptions& options) {
  auto normalized = NormalizeInstallPath(options.install_path);
  if (!normalized.ok()) {
    return FailResult(BaseResult(options, options.install_path.string()),
                     normalized.error());
  }
  const std::filesystem::path exe = normalized.value();
  InstallerResult result = BaseResult(options, WideToUtf8(exe.wstring()));
  const DeploymentPaths paths = ResolveStateRoot(options);
  AttachPaths(&result, paths);

  Status secured = EnsureDeploymentPaths(paths);
  if (!secured.ok()) return FailResult(result, secured);
  auto lock = OpenInstallerMutex(60000);
  if (!lock.ok()) return FailResult(result, lock.error());

  MaintenanceFenceInfo fence;
  Status fence_status = ReadMaintenanceFence(paths, &fence);
  if (!fence_status.ok()) return FailResult(result, fence_status);
  if (!fence.exists) {
    std::error_code ignored;
    std::filesystem::remove(StateFilePath(paths), ignored);
    RemoveApplyMarker(paths);
    result.ok = true;
    result.exit_code = 0;
    result.restore_state = "stable";
    result.messages.push_back("no maintenance state to commit");
    return result;
  }
  InstallerAction action = options.action;
  if (options.use_captured_action) {
    // The MSI commit action runs for every branch of the active package and
    // passes --action auto, so recover the operation from the fence/capture
    // prepare wrote.  Its sequence condition only excludes the old package's
    // nested uninstall during RemoveExistingProducts.
    InstallerAction parsed;
    if (!fence.operation.empty() &&
        ParseInstallerAction(fence.operation, &parsed)) {
      action = parsed;
    } else {
      InstallerCapture capture;
      Status read = ReadStateFile(paths, &capture);
      if (!read.ok()) return FailResult(result, read);
      action = capture.action;
    }
    result.action = action;
    result.operation = ToString(action);
  } else if (!fence.operation.empty() &&
             fence.operation != ToString(action)) {
    return FailResult(
        result,
        XFS_ERR(XfsError::kBadValue,
                "the outstanding maintenance operation is " + fence.operation +
                    ", not " + ToString(action))
            .Metadata("installer"));
  }
  if (!options.use_captured_action && action != InstallerAction::kUninstall) {
    InstallerCapture capture;
    Status read = ReadStateFile(paths, &capture);
    if (!read.ok()) return FailResult(result, read);
    if (capture.action != action) {
      return FailResult(
          result,
          XFS_ERR(XfsError::kBadValue,
                  "the captured rollback state belongs to a " +
                      std::string(ToString(capture.action)) + " operation")
              .Metadata("installer"));
    }
  }
  Status cleared = ClearMaintenanceState(paths);
  if (!cleared.ok()) return FailResult(result, cleared);
  result.ok = true;
  result.exit_code = 0;
  result.restore_state = "stable";
  result.messages.push_back("maintenance fence cleared");
  return result;
}

InstallerResult RollbackInstaller(const InstallerOptions& options) {
  auto normalized = NormalizeInstallPath(options.install_path);
  if (!normalized.ok()) {
    return FailResult(BaseResult(options, options.install_path.string()),
                     normalized.error());
  }
  const std::filesystem::path exe = normalized.value();
  InstallerResult result = BaseResult(options, WideToUtf8(exe.wstring()));
  const DeploymentPaths paths = ResolveStateRoot(options);
  AttachPaths(&result, paths);

  Status secured = EnsureDeploymentPaths(paths);
  if (!secured.ok()) return FailResult(result, secured);
  auto lock = OpenInstallerMutex(60000);
  if (!lock.ok()) return FailResult(result, lock.error());

  MaintenanceFenceInfo fence;
  Status fence_status = ReadMaintenanceFence(paths, &fence);
  if (!fence_status.ok()) return FailResult(result, fence_status);
  if (!fence.exists) {
    std::error_code ignored;
    std::filesystem::remove(StateFilePath(paths), ignored);
    RemoveApplyMarker(paths);
    result.ok = true;
    result.exit_code = 0;
    result.restore_state = "stable";
    result.messages.push_back("no maintenance state to roll back");
    return result;
  }
  InstallerCapture capture;
  Status read = ReadStateFile(paths, &capture);
  if (!read.ok()) {
    result.restore_state = "restoration-pending";
    result.recoverable_record = StateFilePath(paths);
    return FailResult(result, read);
  }
  if (options.use_captured_action) {
    // The MSI rollback action runs for every branch of the active package and
    // passes --action auto; the operation it must undo is the one prepare
    // captured.  Its sequence condition only excludes the old package's
    // nested uninstall during RemoveExistingProducts.
    result.action = capture.action;
    result.operation = ToString(capture.action);
  } else if (capture.action != options.action) {
    return FailResult(
        result,
        XFS_ERR(XfsError::kBadValue,
                "the captured rollback state belongs to a " +
                    std::string(ToString(capture.action)) + " operation")
            .Metadata("installer"));
  }
  if (!capture.service_context_captured) {
    result.restore_state = "restoration-pending";
    result.recoverable_record = StateFilePath(paths);
    return FailResult(
        result,
        XFS_ERR(XfsError::kUnsupportedFeature,
                "the original service context was not captured; restore the "
                "service manually and then run commit")
            .Metadata("installer"));
  }
  result.winfsp = ProbeWinFspRuntime();

  InstallerServiceConfig current;
  Status queried = QueryInstallerService(&current);
  if (!queried.ok()) return FailResult(result, queried);

  if (capture.original_service.installed) {
    if (!PathExists(exe)) {
      result.restore_state = "restoration-pending";
      result.recoverable_record = StateFilePath(paths);
      return FailResult(
          result,
          XFS_ERR(XfsError::kNotFound,
                  "the original xfswin.exe is not restored yet; retry rollback "
                  "after the files are back")
              .Metadata("installer"));
    }
    if (current.installed && !SamePath(current.binary_path, exe.wstring()) &&
        !SamePath(current.binary_path,
                  capture.original_service.binary_path)) {
      return FailResult(
          result,
          XFS_ERR(XfsError::kAlreadyExists,
                  "the automount service now belongs to " +
                      WideToUtf8(current.binary_path))
              .Metadata("installer"));
    }
    // The rollback-quiesce action runs before Windows Installer restores the
    // files and stops the service the transaction started or updated.  This
    // action must not stop an untouched service: a rollback that only clears a
    // refused prepare leaves the original installation and its live mounts
    // running.  Restoring the captured configuration (and then starting or
    // stopping per the snapshot) is enough here.
    std::wstring detail;
    Status restored =
        EnsureServiceWithConfig(capture.original_service, exe.wstring(), &detail);
    if (!detail.empty()) result.messages.push_back(WideToUtf8(detail));
    if (!restored.ok()) {
      result.restore_state = "restoration-pending";
      result.recoverable_record = StateFilePath(paths);
      return FailResult(result, restored);
    }
    if (capture.original_service.running) {
      std::string start_message;
      Status started = StartInstallerService(&start_message);
      if (!start_message.empty()) result.messages.push_back(start_message);
      if (!started.ok()) {
        result.restore_state = "restoration-pending";
        result.recoverable_record = StateFilePath(paths);
        return FailResult(result, started);
      }
    } else {
      std::string stop_message;
      Status stopped = StopInstallerService(&stop_message);
      if (!stop_message.empty()) result.messages.push_back(stop_message);
      if (!stopped.ok()) {
        result.restore_state = "restoration-pending";
        result.recoverable_record = StateFilePath(paths);
        return FailResult(result, stopped);
      }
    }
  } else {
    if (current.installed) {
      if (!SamePath(current.binary_path, exe.wstring())) {
        return FailResult(
            result,
            XFS_ERR(XfsError::kAlreadyExists,
                    "refusing to delete a service that belongs to " +
                        WideToUtf8(current.binary_path))
                .Metadata("installer"));
      }
      bool pending = false;
      Status deleted = DeleteInstallerService(&pending);
      if (!deleted.ok()) {
        result.restore_state = "restoration-pending";
        result.recoverable_record = StateFilePath(paths);
        return FailResult(result, deleted);
      }
      if (pending) {
        Status waited = WaitInstallerServiceDeleted(30000);
        if (!waited.ok()) {
          result.restore_state = "restoration-pending";
          result.recoverable_record = StateFilePath(paths);
          return FailResult(result, waited);
        }
      }
      result.messages.push_back(
          "the service created by this transaction was removed");
    }
  }

  Status refreshed = QueryInstallerService(&result.service);
  if (!refreshed.ok()) return FailResult(result, refreshed);
  Status cleared = ClearMaintenanceState(paths);
  if (!cleared.ok()) {
    result.restore_state = "restoration-pending";
    result.recoverable_record = StateFilePath(paths);
    return FailResult(result, cleared);
  }
  result.ok = true;
  result.exit_code = 0;
  result.restore_state = "stable-after-rollback";
  result.messages.push_back("the pre-install service state was restored");
  return result;
}

InstallerResult QuiesceInstaller(const InstallerOptions& options) {
  auto normalized = NormalizeInstallPath(options.install_path);
  if (!normalized.ok()) {
    return FailResult(BaseResult(options, options.install_path.string()),
                     normalized.error());
  }
  const std::filesystem::path exe = normalized.value();
  InstallerResult result = BaseResult(options, WideToUtf8(exe.wstring()));
  const DeploymentPaths paths = ResolveStateRoot(options);
  AttachPaths(&result, paths);

  Status secured = EnsureDeploymentPaths(paths);
  if (!secured.ok()) return FailResult(result, secured);
  auto lock = OpenInstallerMutex(60000);
  if (!lock.ok()) return FailResult(result, lock.error());

  // Without a fence no apply could have run, so there is nothing to stop.
  // Leaving the state untouched keeps this verb safe to call from every
  // rollback branch, including failures that happened before prepare.
  MaintenanceFenceInfo fence;
  Status fence_status = ReadMaintenanceFence(paths, &fence);
  if (!fence_status.ok()) return FailResult(result, fence_status);
  if (!fence.exists) {
    result.ok = true;
    result.exit_code = 0;
    result.restore_state = "stable";
    result.messages.push_back(
        "no maintenance transaction is active; no service to stop");
    return result;
  }
  // Only a transaction that actually reached the service-mutating apply or
  // remove step may stop a service during rollback.  Prepare writes the fence
  // and capture before it classifies live sessions, so an upgrade refused for
  // an active mount leaves a fence behind while the original service and its
  // mounts keep running; stopping it here would kill the very session the
  // operator was told to keep.  apply/remove write the marker immediately
  // before their first SCM change.
  if (!ApplyMarkerExists(paths)) {
    result.ok = true;
    result.exit_code = 0;
    result.restore_state = "maintenance-active";
    result.messages.push_back(
        "the transaction never reached the service step; no service to stop");
    return result;
  }

  // Quiesce takes no operation argument: the MSI schedules it for every
  // transaction branch of the active package and excludes only the old
  // package's nested uninstall, so the fence/capture written by prepare is the
  // authoritative operation for reporting.
  InstallerAction action;
  if (ParseInstallerAction(fence.operation, &action)) {
    result.action = action;
    result.operation = ToString(action);
  }
  InstallerCapture capture;
  const bool have_capture = ReadStateFile(paths, &capture).ok();

  InstallerServiceConfig service;
  Status queried = QueryInstallerService(&service);
  if (!queried.ok()) return FailResult(result, queried);
  result.service = service;
  if (!service.installed) {
    result.ok = true;
    result.exit_code = 0;
    result.restore_state = "maintenance-active";
    result.messages.push_back(
        "the automount service is not installed; nothing to stop");
    return result;
  }
  // Stop only the service this transaction owns.  A different image is
  // another installation (the original image is accepted because an upgrade
  // applies the same install path).
  const bool owned = SamePath(service.binary_path, exe.wstring()) ||
                     (have_capture && capture.original_service.installed &&
                      SamePath(service.binary_path,
                               capture.original_service.binary_path));
  if (!owned) {
    return FailResult(
        result,
        XFS_ERR(XfsError::kAlreadyExists,
                "the automount service belongs to " +
                    WideToUtf8(service.binary_path) + "; refusing to stop it")
            .Metadata("installer"));
  }
  std::string stop_message;
  Status stopped = StopInstallerService(&stop_message);
  if (!stop_message.empty()) result.messages.push_back(stop_message);
  if (!stopped.ok()) {
    const std::vector<std::string> failures = ReadShutdownFailures(paths);
    for (const std::string& failure : failures) {
      result.messages.push_back("shutdown record: " + failure);
    }
    result.restore_state = "restoration-pending";
    result.recoverable_record = paths.installer / L"shutdown.ini";
    return FailResult(result, stopped);
  }
  result.ok = true;
  result.exit_code = 0;
  result.restore_state = "maintenance-active";
  result.messages.push_back(
      "the automount service is stopped; rollback may restore the files");
  return result;
}

// ---------------------------------------------------------------------------
// Result rendering
// ---------------------------------------------------------------------------

namespace {

std::string JsonEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buffer[8] = {};
          std::snprintf(buffer, sizeof(buffer), "\\u%04X",
                        static_cast<unsigned>(c));
          out += buffer;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

std::string JsonString(const std::string& text) {
  return "\"" + JsonEscape(text) + "\"";
}

std::string JsonStrings(const std::vector<std::string>& values) {
  std::string out = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += ",";
    out += JsonString(values[i]);
  }
  out += "]";
  return out;
}

std::string JsonBool(bool value) { return value ? "true" : "false"; }

std::string PathText(const std::filesystem::path& path) {
  return path.empty() ? std::string() : WideToUtf8(path.wstring());
}

std::string ServiceStateText(const InstallerResult& result) {
  std::string text = ToString(result.service.state);
  if (result.service.installed && result.service.running) text += " (running)";
  return text;
}

}  // namespace

std::string FormatInstallerResult(const InstallerResult& result,
                                  const std::string& verb, bool json) {
  if (!json) {
    std::ostringstream text;
    text << "verb=" << verb << "\n";
    text << "result=" << (result.ok ? "ok" : "failed") << "\n";
    text << "operation=" << result.operation << "\n";
    text << "install-path=" << result.install_path << "\n";
    text << "product-version=" << result.product_version << "\n";
    text << "architecture=" << result.architecture << "\n";
    text << "exit-code=" << result.exit_code << "\n";
    text << "service=" << ServiceStateText(result) << "\n";
    if (result.service.installed) {
      text << "service-binary=" << WideToUtf8(result.service.binary_path)
           << "\n";
    }
    if (result.winfsp.available) {
      text << "winfsp=" << result.winfsp.version << " ("
           << WideToUtf8(result.winfsp.dll_path) << ")\n";
    } else if (!result.winfsp.diagnostic.empty()) {
      text << "winfsp=unavailable: " << result.winfsp.diagnostic << "\n";
    }
    if (!result.state_file.empty()) {
      text << "state-file=" << PathText(result.state_file) << "\n";
    }
    if (!result.fence_file.empty()) {
      text << "fence-file=" << PathText(result.fence_file) << "\n";
    }
    if (!result.recoverable_record.empty()) {
      text << "recoverable-record=" << PathText(result.recoverable_record)
           << "\n";
    }
    if (!result.restore_state.empty()) {
      text << "restore-state=" << result.restore_state << "\n";
    }
    for (const auto& message : result.messages) {
      text << "message=" << message << "\n";
    }
    for (const auto& warning : result.warnings) {
      text << "warning=" << warning << "\n";
    }
    for (const auto& blocker : result.blockers) {
      text << "blocker=" << blocker << "\n";
    }
    for (const auto& session : result.remaining_sessions) {
      text << "remaining-session=" << session << "\n";
    }
    if (!result.ok) text << "error=" << result.error.error().Format() << "\n";
    return text.str();
  }

  std::ostringstream out;
  out << "{";
  out << "\"verb\":" << JsonString(verb) << ",";
  out << "\"ok\":" << JsonBool(result.ok) << ",";
  out << "\"operation\":" << JsonString(result.operation) << ",";
  out << "\"action\":" << JsonString(ToString(result.action)) << ",";
  out << "\"install_path\":" << JsonString(result.install_path) << ",";
  out << "\"product_version\":" << JsonString(result.product_version) << ",";
  out << "\"architecture\":" << JsonString(result.architecture) << ",";
  out << "\"exit_code\":" << result.exit_code << ",";
  out << "\"restore_state\":" << JsonString(result.restore_state) << ",";
  out << "\"service\":{"
      << "\"installed\":" << JsonBool(result.service.installed) << ","
      << "\"state\":" << JsonString(ToString(result.service.state)) << ","
      << "\"running\":" << JsonBool(result.service.running) << ","
      << "\"stop_pending\":" << JsonBool(result.service.stop_pending) << ","
      << "\"start_pending\":" << JsonBool(result.service.start_pending) << ","
      << "\"start_type\":" << result.service.start_type << ","
      << "\"binary_path\":" << JsonString(WideToUtf8(result.service.binary_path))
      << "},";
  out << "\"winfsp\":{"
      << "\"available\":" << JsonBool(result.winfsp.available) << ","
      << "\"version\":" << JsonString(result.winfsp.version) << ","
      << "\"dll_path\":" << JsonString(WideToUtf8(result.winfsp.dll_path))
      << ","
      << "\"major\":" << result.winfsp.major << ","
      << "\"minor\":" << result.winfsp.minor << ","
      << "\"build\":" << result.winfsp.build << ","
      << "\"diagnostic\":" << JsonString(result.winfsp.diagnostic) << "},";
  out << "\"service_created\":" << JsonBool(result.service_created) << ",";
  out << "\"service_updated\":" << JsonBool(result.service_updated) << ",";
  out << "\"service_absent_allowed\":"
      << JsonBool(result.service_absent_allowed) << ",";
  out << "\"messages\":" << JsonStrings(result.messages) << ",";
  out << "\"warnings\":" << JsonStrings(result.warnings) << ",";
  out << "\"blockers\":" << JsonStrings(result.blockers) << ",";
  out << "\"remaining_sessions\":"
      << JsonStrings(result.remaining_sessions) << ",";
  out << "\"state_file\":" << JsonString(PathText(result.state_file)) << ",";
  out << "\"fence_file\":" << JsonString(PathText(result.fence_file)) << ",";
  out << "\"recoverable_record\":"
      << JsonString(PathText(result.recoverable_record));
  if (!result.ok) {
    out << ",\"error\":{"
        << "\"code\":" << JsonString(ToString(result.error.error().code))
        << ","
        << "\"message\":" << JsonString(result.error.error().message) << ","
        << "\"metadata\":" << JsonString(result.error.error().metadata) << "}";
  }
  out << "}";
  return out.str();
}

}  // namespace xfs
