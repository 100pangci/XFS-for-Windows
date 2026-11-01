// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/windows_volume.h"

#ifdef _WIN32

#include "xfs/log.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
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

}  // namespace xfs

#endif  // _WIN32