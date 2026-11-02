// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/windows_volume.h"

#ifdef _WIN32

#include "xfs/log.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace xfs {
namespace {

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                      out.data(), len, nullptr, nullptr);
  return out;
}

std::string WinErrorSuffix(DWORD code) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), " (Windows error %lu)",
                static_cast<unsigned long>(code));
  return buf;
}

// Reads IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS with a growing buffer.
bool QueryExtents(HANDLE volume, std::vector<VolumeExtent>* out,
                  std::string* error) {
  constexpr size_t kInitialExtents = 8;
  std::vector<uint8_t> buffer(sizeof(VOLUME_DISK_EXTENTS) +
                              (kInitialExtents - 1) * sizeof(DISK_EXTENT));
  for (int attempt = 0; attempt < 5; ++attempt) {
    DWORD bytes = 0;
    if (DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr,
                        0, buffer.data(), static_cast<DWORD>(buffer.size()),
                        &bytes, nullptr)) {
      if (bytes < sizeof(VOLUME_DISK_EXTENTS)) {
        if (error) *error = "short volume extent buffer";
        return false;
      }
      const auto* extents =
          reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
      const size_t count = extents->NumberOfDiskExtents;
      const size_t needed =
          sizeof(VOLUME_DISK_EXTENTS) +
          (count > 0 ? (count - 1) * sizeof(DISK_EXTENT) : 0);
      if (needed > bytes) {
        if (error) *error = "truncated volume extent buffer";
        return false;
      }
      out->clear();
      for (size_t i = 0; i < count; ++i) {
        VolumeExtent e;
        e.disk_number = extents->Extents[i].DiskNumber;
        e.starting_offset =
            static_cast<uint64_t>(extents->Extents[i].StartingOffset.QuadPart);
        e.length_bytes =
            static_cast<uint64_t>(extents->Extents[i].ExtentLength.QuadPart);
        out->push_back(e);
      }
      return true;
    }
    const DWORD code = GetLastError();
    if (code != ERROR_MORE_DATA) {
      if (error) *error = "cannot query volume extents" + WinErrorSuffix(code);
      return false;
    }
    size_t next = kInitialExtents << (attempt + 1);
    if (bytes >= sizeof(VOLUME_DISK_EXTENTS)) {
      const auto* extents =
          reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
      next = std::max<size_t>(extents->NumberOfDiskExtents, kInitialExtents);
    }
    if (next > 4096) {
      if (error) *error = "volume has too many disk extents";
      return false;
    }
    const size_t needed =
        sizeof(VOLUME_DISK_EXTENTS) + (next - 1) * sizeof(DISK_EXTENT);
    if (needed <= buffer.size()) {
      if (error) *error = "volume extent query made no progress";
      return false;
    }
    buffer.resize(needed);
  }
  if (error) *error = "cannot query volume extents";
  return false;
}

std::vector<std::string> QueryMountPoints(const std::wstring& volume_name) {
  std::vector<std::string> out;
  std::vector<wchar_t> buffer(512);
  DWORD returned = 0;
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (GetVolumePathNamesForVolumeNameW(volume_name.c_str(), buffer.data(),
                                         static_cast<DWORD>(buffer.size()),
                                         &returned)) {
      break;
    }
    if (GetLastError() != ERROR_MORE_DATA || returned <= buffer.size()) {
      return out;
    }
    buffer.resize(static_cast<size_t>(returned) + 1);
  }
  const wchar_t* p = buffer.data();
  while (*p) {
    out.push_back(WideToUtf8(p));
    p += std::wcslen(p) + 1;
  }
  return out;
}

DriveLetterKind ClassifyDriveTarget(DWORD drive_type,
                                    const std::string& target) {
  if (drive_type == DRIVE_REMOTE) return DriveLetterKind::kNetwork;
  if (drive_type == DRIVE_CDROM) return DriveLetterKind::kCdrom;
  if (drive_type == DRIVE_RAMDISK) return DriveLetterKind::kRamdisk;
  // QueryDosDeviceW for SUBST returns "\??\C:\target".  A volume mapping
  // returns "\Device\HarddiskVolumeN", and a network mapping returns a
  // \Device\Mup... path.
  if (target.size() >= 6 && target[0] == '\\' && target[1] == '?' &&
      target[2] == '?' && target[3] == '\\' && target[5] == ':') {
    return DriveLetterKind::kSubst;
  }
  if (drive_type == DRIVE_REMOVABLE) return DriveLetterKind::kRemovable;
  if (drive_type == DRIVE_FIXED) return DriveLetterKind::kFixed;
  return DriveLetterKind::kUnknown;
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), len);
  return out;
}

std::string NormalizedVolumePath(std::string_view device_path) {
  std::string path(device_path);
  while (!path.empty() && path.back() == '\\') path.pop_back();
  std::transform(path.begin(), path.end(), path.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return path;
}

bool SamePathIgnoreCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const auto ca = static_cast<unsigned char>(a[i]);
    const auto cb = static_cast<unsigned char>(b[i]);
    if (std::tolower(ca) != std::tolower(cb)) return false;
  }
  return true;
}

// Normalizes "f:" / "F:\\" to "F:\\" for the mount-point APIs.  Returns an
// empty string for anything that is not a normal drive-letter root.
std::wstring DriveLetterMountPointW(std::string_view mount_point) {
  if (mount_point.empty()) return {};
  char letter = mount_point[0];
  if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 32);
  if (letter < 'A' || letter > 'Z') return {};
  if (mount_point.size() > 1 && mount_point[1] != ':') return {};
  if (mount_point.size() > 2 && mount_point[2] != '\\') return {};
  if (mount_point.size() > 3) return {};
  std::wstring out;
  out.push_back(static_cast<wchar_t>(letter));
  out.push_back(L':');
  out.push_back(L'\\');
  return out;
}

std::string MountPointText(std::string_view mount_point) {
  const std::wstring wide = DriveLetterMountPointW(mount_point);
  return wide.empty() ? std::string() : WideToUtf8(wide);
}

bool VolumeHasPath(const std::wstring& volume_name, const std::string& path) {
  for (const auto& current : QueryMountPoints(volume_name)) {
    if (SamePathIgnoreCase(current, path)) return true;
  }
  return false;
}

// Both directions: the volume lists the mount point, and the mount point
// resolves back to the same volume GUID.  A match in one direction only is not
// proof that the original association is intact.
bool VolumeOwnsMountPoint(const std::wstring& volume_name,
                          const std::string& mount_text) {
  if (!VolumeHasPath(volume_name, mount_text)) return false;
  auto reverse = QueryVolumeNameForMountPoint(mount_text);
  if (!reverse.ok()) return false;
  return SamePathIgnoreCase(
      NormalizedVolumePath(reverse.value()),
      NormalizedVolumePath(WideToUtf8(volume_name)));
}

bool DriveLetterInUse(std::string_view drive) {
  if (drive.size() < 1) return false;
  char letter = drive[0];
  if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 32);
  if (letter < 'A' || letter > 'Z') return false;
  SetLastError(ERROR_SUCCESS);
  const DWORD mask = GetLogicalDrives();
  if (mask == 0 && GetLastError() != ERROR_SUCCESS) {
    // Cannot prove the letter is free; fail closed rather than let a restore
    // or takeover overwrite an existing association.
    return true;
  }
  return (mask & (1UL << (letter - 'A'))) != 0;
}

}  // namespace

std::vector<RawVolumeInfo> EnumerateVolumes(std::string* error) {
  std::vector<RawVolumeInfo> volumes;
  wchar_t volume_name[MAX_PATH + 1] = {};
  HANDLE find = FindFirstVolumeW(volume_name, MAX_PATH);
  if (find == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (error) {
      *error = "cannot enumerate volumes" + WinErrorSuffix(code);
    }
    return volumes;
  }
  do {
    RawVolumeInfo info;
    info.volume_name = WideToUtf8(volume_name);
    std::wstring open_path(volume_name);
    if (!open_path.empty() && open_path.back() == L'\\') open_path.pop_back();
    info.device_path = WideToUtf8(open_path);
    XFS_DEBUG("discovery: volume " << info.volume_name
                                   << ": querying mount points");
    info.mount_points = QueryMountPoints(volume_name);

    // The volume properties are optional: a RAW volume has no label or
    // filesystem name, and an inaccessible volume still contributes its
    // identity when possible.
    wchar_t label[261] = {};
    wchar_t fs_name[261] = {};
    DWORD serial = 0;
    XFS_DEBUG("discovery: volume " << info.volume_name
                                   << ": querying volume information");
    if (GetVolumeInformationW(volume_name, label, 260, &serial, nullptr,
                              nullptr, fs_name, 260)) {
      info.label = WideToUtf8(label);
      info.filesystem = WideToUtf8(fs_name);
      info.serial_number = static_cast<uint32_t>(serial);
      info.has_filesystem = true;
    } else {
      info.volume_info_error = GetLastError();
      // Only this error means "a volume with no filesystem Windows can use".
      // Access-denied / not-ready / I/O errors must never make a volume a
      // takeover candidate.
      info.unrecognized_filesystem =
          info.volume_info_error == ERROR_UNRECOGNIZED_VOLUME;
    }

    XFS_DEBUG("discovery: volume " << info.volume_name << ": opening");
    HANDLE volume = CreateFileW(open_path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) {
      info.error = "cannot open volume" + WinErrorSuffix(GetLastError());
    } else {
      info.readable = true;
      STORAGE_DEVICE_NUMBER number{};
      DWORD bytes = 0;
      if (DeviceIoControl(volume, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                          &number, sizeof(number), &bytes, nullptr)) {
        info.disk_number = number.DeviceNumber;
        info.partition_number = number.PartitionNumber;
      }
      std::string extent_error;
      if (QueryExtents(volume, &info.extents, &extent_error)) {
        if (!info.extents.empty()) {
          info.disk_number = info.extents.front().disk_number;
        }
      } else if (!extent_error.empty()) {
        info.error = extent_error;
      }
      CloseHandle(volume);
    }
    volumes.push_back(std::move(info));
  } while (FindNextVolumeW(find, volume_name, MAX_PATH));
  const DWORD end = GetLastError();
  FindVolumeClose(find);
  if (end != ERROR_NO_MORE_FILES && end != ERROR_NO_MORE_ITEMS) {
    if (error) {
      *error = "volume enumeration stopped early" + WinErrorSuffix(end);
    }
  }
  return volumes;
}

std::vector<DriveLetterInfo> EnumerateDriveLetters(std::string* error) {
  std::vector<DriveLetterInfo> letters;
  DWORD needed = GetLogicalDriveStringsW(0, nullptr);
  if (needed == 0) {
    if (error) {
      *error = "cannot enumerate drive letters" + WinErrorSuffix(GetLastError());
    }
    return letters;
  }
  std::vector<wchar_t> buffer(static_cast<size_t>(needed) + 1, L'\0');
  DWORD written =
      GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()), buffer.data());
  if (written == 0 || written > buffer.size()) {
    if (error) {
      *error = "cannot read drive letters" + WinErrorSuffix(GetLastError());
    }
    return letters;
  }
  const wchar_t* p = buffer.data();
  while (*p) {
    std::wstring root(p);
    p += std::wcslen(p) + 1;
    if (root.size() < 2 || root[1] != L':') continue;
    wchar_t letter = root[0];
    if (letter >= L'a' && letter <= L'z') letter = static_cast<wchar_t>(letter - 32);
    if (letter < L'A' || letter > L'Z') continue;

    DriveLetterInfo info;
    info.letter = static_cast<char>(letter);
    XFS_DEBUG("discovery: drive letter " << info.letter << ": querying type");
    const DWORD type = GetDriveTypeW(root.c_str());
    wchar_t target[MAX_PATH + 1] = {};
    const std::wstring device = std::wstring(1, letter) + L":";
    if (QueryDosDeviceW(device.c_str(), target, MAX_PATH) != 0) {
      info.target = WideToUtf8(target);
    }
    XFS_DEBUG("discovery: drive letter " << info.letter
                                         << ": type=" << type
                                         << " target=" << info.target);
    info.kind = ClassifyDriveTarget(type, info.target);
    letters.push_back(std::move(info));
  }
  return letters;
}

const RawVolumeInfo* FindVolumeByDevicePath(
    const std::vector<RawVolumeInfo>& volumes, std::string_view device_path) {
  const std::string wanted = NormalizedVolumePath(device_path);
  if (wanted.empty()) return nullptr;
  for (const auto& volume : volumes) {
    if (NormalizedVolumePath(volume.device_path) == wanted) return &volume;
  }
  return nullptr;
}

Result<VolumePathState> QueryVolumePathState(std::string_view device_path,
                                             std::string* error) {
  std::string enumeration_error;
  const auto volumes = EnumerateVolumes(&enumeration_error);
  if (!enumeration_error.empty() && error) *error = enumeration_error;
  const RawVolumeInfo* volume = FindVolumeByDevicePath(volumes, device_path);
  if (!volume) {
    if (error) {
      *error = "volume " + std::string(device_path) +
               " is no longer attached" +
               (enumeration_error.empty() ? std::string()
                                          : " (" + enumeration_error + ")");
    }
    return XFS_ERR(XfsError::kNotFound,
                   "volume is no longer attached")
        .Metadata("volume");
  }
  VolumePathState state;
  state.volume_name = volume->volume_name;
  state.device_path = volume->device_path;
  state.mount_points = volume->mount_points;
  state.has_filesystem = volume->has_filesystem;
  state.unrecognized_filesystem = volume->unrecognized_filesystem;
  state.volume_info_error = volume->volume_info_error;
  state.filesystem = volume->filesystem;
  state.readable = volume->readable;
  state.disk_number = volume->disk_number;
  state.partition_number = volume->partition_number;
  state.extents = volume->extents;
  return state;
}

Status RemoveDriveLetterMountPoint(std::string_view mount_point,
                                   std::string* error) {
  const std::wstring wide = DriveLetterMountPointW(mount_point);
  if (wide.empty()) {
    if (error) *error = "not a drive-letter mount point: " +
                        std::string(mount_point);
    return XFS_ERR(XfsError::kInvalidArgument,
                   "not a drive-letter mount point")
        .Metadata("volume mount point");
  }
  if (DeleteVolumeMountPointW(wide.c_str())) {
    return OkStatus();
  }
  const DWORD code = GetLastError();
  // These mean the association is already gone; that is the desired state.
  if (code == ERROR_PATH_NOT_FOUND || code == ERROR_FILE_NOT_FOUND ||
      code == ERROR_NOT_FOUND) {
    return OkStatus();
  }
  if (error) {
    *error = "cannot remove the mount point " + WideToUtf8(wide) +
             WinErrorSuffix(code);
  }
  return XFS_ERR(XfsError::kIoError,
                 "cannot remove the Windows drive-letter mount point")
      .Metadata("volume mount point")
      .Actual(code);
}

Status RestoreDriveLetterMountPoint(std::string_view mount_point,
                                    std::string_view volume_name,
                                    std::string* error) {
  const std::wstring mount_wide = DriveLetterMountPointW(mount_point);
  if (mount_wide.empty()) {
    if (error) *error = "not a drive-letter mount point";
    return XFS_ERR(XfsError::kInvalidArgument,
                   "not a drive-letter mount point")
        .Metadata("volume mount point");
  }
  std::wstring volume_wide = Utf8ToWide(std::string(volume_name));
  if (volume_wide.empty() || volume_wide.back() != L'\\') {
    volume_wide.push_back(L'\\');
  }
  const std::string mount_text = WideToUtf8(mount_wide);

  // Already restored: keep it, but verify both directions of the association.
  if (VolumeOwnsMountPoint(volume_wide, mount_text)) {
    return OkStatus();
  }
  if (VolumeHasPath(volume_wide, mount_text)) {
    if (error) {
      *error = "the volume lists " + mount_text +
               " but the letter resolves to a different volume";
    }
    return XFS_ERR(XfsError::kBadValue,
                   "the drive letter does not resolve to the recorded volume")
        .Metadata("volume mount point");
  }
  if (DriveLetterInUse(mount_text)) {
    if (error) {
      *error = "the drive letter " + mount_text +
               " is now in use by another volume; the original Windows mount "
               "point was not overwritten";
    }
    return XFS_ERR(XfsError::kAlreadyExists,
                   "the drive letter is now in use by another volume")
        .Metadata("volume mount point");
  }
  if (!SetVolumeMountPointW(mount_wide.c_str(), volume_wide.c_str())) {
    const DWORD code = GetLastError();
    if (error) {
      *error = "cannot restore the mount point " + mount_text +
               WinErrorSuffix(code);
    }
    return XFS_ERR(XfsError::kIoError,
                   "cannot restore the original Windows mount point")
        .Metadata("volume mount point")
        .Actual(code);
  }
  // SetVolumeMountPointW can succeed while the mount manager resolves the
  // association asynchronously; require both directions to be visible before
  // telling the caller the restoration is complete.
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (VolumeOwnsMountPoint(volume_wide, mount_text)) return OkStatus();
    Sleep(50);
  }
  if (error) {
    *error = "the mount point " + mount_text +
             " was accepted but the volume does not list it";
  }
  return XFS_ERR(XfsError::kIoError,
                 "the original Windows mount point was not restored")
      .Metadata("volume mount point");
}

bool IsVolumePresent(std::string_view device_path) {
  std::string error;
  const auto volumes = EnumerateVolumes(&error);
  return FindVolumeByDevicePath(volumes, device_path) != nullptr;
}

bool WaitForDriveLetterRelease(std::string_view drive, uint32_t timeout_ms) {
  if (drive.empty()) return false;
  char letter = drive[0];
  if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 32);
  if (letter < 'A' || letter > 'Z') return false;
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    SetLastError(ERROR_SUCCESS);
    const DWORD mask = GetLogicalDrives();
    if (mask == 0 && GetLastError() != ERROR_SUCCESS) {
      // The query failed; the letter's state is unknown, so it is not released.
      return false;
    }
    if ((mask & (1UL << (letter - 'A'))) == 0) return true;
    if (GetTickCount64() >= deadline) return false;
    Sleep(50);
  }
}

bool QueryDriveVolumeInformation(std::string_view drive,
                                 std::string* filesystem, uint32_t* serial,
                                 std::string* error) {
  if (drive.empty()) {
    if (error) *error = "empty drive letter";
    return false;
  }
  char letter = drive[0];
  if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 32);
  if (letter < 'A' || letter > 'Z') {
    if (error) *error = "invalid drive letter: " + std::string(drive);
    return false;
  }
  const std::wstring root = std::wstring(1, static_cast<wchar_t>(letter)) +
                            L":\\";
  wchar_t fs_name[261] = {};
  DWORD serial_number = 0;
  if (!GetVolumeInformationW(root.c_str(), nullptr, 0, &serial_number, nullptr,
                             nullptr, fs_name, 260)) {
    if (error) {
      *error = "cannot read drive " + WideToUtf8(root) + " volume information" +
               WinErrorSuffix(GetLastError());
    }
    return false;
  }
  if (filesystem) *filesystem = WideToUtf8(fs_name);
  if (serial) *serial = static_cast<uint32_t>(serial_number);
  return true;
}

Result<std::string> QueryVolumeNameForMountPoint(std::string_view mount_point) {
  const std::wstring wide = DriveLetterMountPointW(mount_point);
  if (wide.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "not a drive-letter mount point")
        .Metadata("volume mount point");
  }
  std::vector<wchar_t> buffer(MAX_PATH + 1, L'\0');
  if (!GetVolumeNameForVolumeMountPointW(wide.c_str(), buffer.data(),
                                         static_cast<DWORD>(buffer.size()))) {
    const DWORD code = GetLastError();
    return XFS_ERR(XfsError::kNotFound,
                   "cannot resolve the volume of " + WideToUtf8(wide) +
                       WinErrorSuffix(code))
        .Metadata("volume mount point")
        .Actual(code);
  }
  return WideToUtf8(buffer.data());
}

}  // namespace xfs

#else  // !_WIN32

namespace xfs {

std::vector<RawVolumeInfo> EnumerateVolumes(std::string* error) {
  if (error) *error = "volume enumeration is only supported on Windows";
  return {};
}

std::vector<DriveLetterInfo> EnumerateDriveLetters(std::string* error) {
  if (error) *error = "drive-letter enumeration is only supported on Windows";
  return {};
}

const RawVolumeInfo* FindVolumeByDevicePath(
    const std::vector<RawVolumeInfo>& volumes, std::string_view device_path) {
  (void)volumes;
  (void)device_path;
  return nullptr;
}

Result<VolumePathState> QueryVolumePathState(std::string_view device_path,
                                             std::string* error) {
  (void)device_path;
  if (error) *error = "volume queries are only supported on Windows";
  return XFS_ERR(XfsError::kUnsupportedFeature,
                 "volume queries are only supported on Windows");
}

Status RemoveDriveLetterMountPoint(std::string_view mount_point,
                                   std::string* error) {
  (void)mount_point;
  if (error) *error = "mount-point changes are only supported on Windows";
  return XFS_ERR(XfsError::kUnsupportedFeature,
                 "mount-point changes are only supported on Windows");
}

Status RestoreDriveLetterMountPoint(std::string_view mount_point,
                                    std::string_view volume_name,
                                    std::string* error) {
  (void)mount_point;
  (void)volume_name;
  if (error) *error = "mount-point changes are only supported on Windows";
  return XFS_ERR(XfsError::kUnsupportedFeature,
                 "mount-point changes are only supported on Windows");
}

bool IsVolumePresent(std::string_view device_path) {
  (void)device_path;
  return false;
}

bool WaitForDriveLetterRelease(std::string_view drive, uint32_t timeout_ms) {
  (void)drive;
  (void)timeout_ms;
  return false;
}

bool QueryDriveVolumeInformation(std::string_view drive,
                                 std::string* filesystem, uint32_t* serial,
                                 std::string* error) {
  (void)drive;
  (void)filesystem;
  (void)serial;
  if (error) *error = "volume queries are only supported on Windows";
  return false;
}

Result<std::string> QueryVolumeNameForMountPoint(
    std::string_view mount_point) {
  (void)mount_point;
  return XFS_ERR(XfsError::kUnsupportedFeature,
                 "volume queries are only supported on Windows");
}

}  // namespace xfs

#endif  // _WIN32