// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/windows_raw_device.h"

#include "xfs/log.h"

#ifdef _WIN32

#include <windows.h>
#include <winioctl.h>

#include <cstring>

#ifndef DEVICE_TYPE
#define DEVICE_TYPE DWORD
#endif

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

std::string WinErrorString(DWORD code) {
  LPSTR buffer = nullptr;
  DWORD len = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
  std::string message;
  if (len && buffer) {
    message.assign(buffer, len);
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r' ||
            message.back() == ' ')) {
      message.pop_back();
    }
  } else {
    message = "Windows error " + std::to_string(code);
  }
  if (buffer) LocalFree(buffer);
  return message;
}

// Queries the disk number a volume lives on (returns false if not a volume).
bool VolumeDiskNumber(HANDLE volume, uint32_t* disk_number,
                      uint64_t* first_offset) {
  STORAGE_DEVICE_NUMBER sdn{};
  DWORD bytes = 0;
  if (!DeviceIoControl(volume, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                       &sdn, sizeof(sdn), &bytes, nullptr)) {
    return false;
  }
  if (sdn.DeviceType != FILE_DEVICE_DISK &&
      sdn.DeviceType != FILE_DEVICE_VIRTUAL_DISK &&
      sdn.DeviceType != FILE_DEVICE_DISK_FILE_SYSTEM) {
    return false;
  }
  *disk_number = sdn.DeviceNumber;
  if (first_offset) {
    VOLUME_DISK_EXTENTS extents{};
    DWORD ext_bytes = 0;
    if (DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr,
                        0, &extents,
                        sizeof(VOLUME_DISK_EXTENTS), &ext_bytes, nullptr) &&
        extents.NumberOfDiskExtents > 0) {
      *first_offset = extents.Extents[0].StartingOffset.QuadPart;
    } else {
      *first_offset = 0;
    }
  }
  return true;
}

std::string QueryDeviceProperty(HANDLE h, STORAGE_PROPERTY_ID property,
                                STORAGE_QUERY_TYPE type, DWORD* out_size) {
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = property;
  query.QueryType = type;
  std::vector<uint8_t> buffer(4096);
  DWORD bytes = 0;
  if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                       buffer.data(), static_cast<DWORD>(buffer.size()),
                       &bytes, nullptr)) {
    return {};
  }
  if (out_size) *out_size = bytes;
  return std::string(reinterpret_cast<char*>(buffer.data()), bytes);
}

}  // namespace

struct WindowsRawBlockDevice::Impl {
  HANDLE handle = INVALID_HANDLE_VALUE;
  std::wstring path;
  std::vector<HANDLE> locked_volumes;
  bool volumes_locked = false;
};

WindowsRawBlockDevice::~WindowsRawBlockDevice() { Close(); }

std::unique_ptr<WindowsRawBlockDevice> WindowsRawBlockDevice::Open(
    const std::wstring& path, bool writable, bool exclusive,
    std::string* error) {
  // Raw volume/disk handles require sector-aligned offsets, lengths and
  // buffers, so every request goes through sector alignment (with a bounce
  // buffer when the caller's buffer is not aligned). Callers that want the
  // fast path allocate 4 KiB-aligned buffers (see AlignedBuffer).
  DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING;
  if (writable) flags |= FILE_FLAG_WRITE_THROUGH;
  DWORD share = exclusive ? 0 : (FILE_SHARE_READ | FILE_SHARE_WRITE);
  DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0);

  HANDLE handle = CreateFileW(path.c_str(), access, share, nullptr,
                              OPEN_EXISTING, flags, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = "cannot open " + WideToUtf8(path) + ": " +
               WinErrorString(GetLastError()) +
               (exclusive ? " (exclusive access requested)" : "");
    }
    return nullptr;
  }

  auto dev = std::unique_ptr<WindowsRawBlockDevice>(new WindowsRawBlockDevice());
  dev->impl_ = std::make_unique<Impl>();
  dev->impl_->handle = handle;
  dev->impl_->path = path;
  dev->writable_ = writable;
  dev->exclusive_ = exclusive;

  // Logical/physical sector sizes: geometry first, alignment descriptor as
  // fallback (volumes often do not answer IOCTL_DISK_GET_DRIVE_GEOMETRY_EX).
  DWORD prop_size = 0;
  std::string prop = QueryDeviceProperty(
      handle, StorageAccessAlignmentProperty, PropertyStandardQuery,
      &prop_size);
  if (prop.size() >= sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR)) {
    const auto* alignment =
        reinterpret_cast<const STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR*>(
            prop.data());
    if (alignment->BytesPerLogicalSector) {
      dev->info_.logical_sector_size = alignment->BytesPerLogicalSector;
    }
    if (alignment->BytesPerPhysicalSector) {
      dev->info_.physical_sector_size = alignment->BytesPerPhysicalSector;
    }
  }

  DISK_GEOMETRY_EX geometry{};
  DWORD bytes = 0;
  if (DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                      &geometry, sizeof(geometry), &bytes, nullptr)) {
    dev->info_.logical_sector_size = geometry.Geometry.BytesPerSector;
    dev->info_.size_bytes = static_cast<uint64_t>(geometry.DiskSize.QuadPart);
  } else {
    GET_LENGTH_INFORMATION length{};
    if (DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length,
                        sizeof(length), &bytes, nullptr)) {
      dev->info_.size_bytes = static_cast<uint64_t>(length.Length.QuadPart);
    } else {
      LARGE_INTEGER file_size{};
      if (GetFileSizeEx(handle, &file_size)) {
        dev->info_.size_bytes = static_cast<uint64_t>(file_size.QuadPart);
      }
    }
  }
  if (dev->info_.logical_sector_size == 0) dev->info_.logical_sector_size = 512;
  if (dev->info_.physical_sector_size == 0) {
    dev->info_.physical_sector_size = dev->info_.logical_sector_size;
  }

  // Model string.
  prop = QueryDeviceProperty(handle, StorageDeviceProperty,
                             PropertyStandardQuery, &prop_size);
  if (prop.size() >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
    const auto* desc =
        reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(prop.data());
    if (desc->ProductIdOffset && desc->ProductIdOffset < prop.size()) {
      const char* product = prop.data() + desc->ProductIdOffset;
      dev->info_.description = product;
      while (!dev->info_.description.empty() &&
             dev->info_.description.back() == ' ') {
        dev->info_.description.pop_back();
      }
    }
    if (desc->RemovableMedia) {
      // not exposed in DeviceInfo; the enumerator reports it
    }
  }
  dev->info_.writable = writable;
  dev->info_.is_raw_device = true;
  if (dev->info_.description.empty()) {
    dev->info_.description = WideToUtf8(path);
  }
  return dev;
}

std::unique_ptr<BlockDevice> OpenRawDevice(const std::wstring& path,
                                           bool writable, bool exclusive,
                                           std::string* error) {
  return WindowsRawBlockDevice::Open(path, writable, exclusive, error);
}

bool WindowsRawBlockDevice::RawReadAligned(uint64_t aligned_offset,
                                           void* buffer, size_t size) {
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(aligned_offset & 0xffffffffu);
  overlapped.OffsetHigh = static_cast<DWORD>(aligned_offset >> 32);
  DWORD read = 0;
  if (!ReadFile(impl_->handle, buffer, static_cast<DWORD>(size), &read,
                &overlapped)) {
    SetError("raw read failed: " + WinErrorString(GetLastError()));
    return false;
  }
  if (read != size) {
    SetError("raw short read (" + std::to_string(read) + " of " +
                  std::to_string(size) + ")");
    return false;
  }
  return true;
}

bool WindowsRawBlockDevice::RawWriteAligned(uint64_t aligned_offset,
                                            const void* buffer, size_t size) {
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(aligned_offset & 0xffffffffu);
  overlapped.OffsetHigh = static_cast<DWORD>(aligned_offset >> 32);
  DWORD written = 0;
  if (!WriteFile(impl_->handle, buffer, static_cast<DWORD>(size), &written,
                 &overlapped)) {
    SetError("raw write failed: " + WinErrorString(GetLastError()));
    return false;
  }
  if (written != size) {
    SetError("raw short write");
    return false;
  }
  return true;
}

bool WindowsRawBlockDevice::ReadExact(uint64_t offset, void* buffer,
                                      size_t size) {
  const uint32_t sector = info_.logical_sector_size;
  if (size == 0) return true;
  if (offset > info_.size_bytes || size > info_.size_bytes - offset) {
    SetError("read out of bounds");
    return false;
  }
  uint8_t* out = static_cast<uint8_t*>(buffer);
  uint64_t aligned_start = offset & ~static_cast<uint64_t>(sector - 1);
  uint64_t aligned_end =
      (offset + size + sector - 1) & ~static_cast<uint64_t>(sector - 1);

  if (aligned_start == offset &&
      (reinterpret_cast<uintptr_t>(buffer) % sector) == 0 &&
      size % sector == 0) {
    return RawReadAligned(offset, buffer, size);
  }

  std::vector<uint8_t> scratch(
      static_cast<size_t>(aligned_end - aligned_start) + sector);
  uint8_t* aligned_buf = scratch.data();
  uintptr_t misalign = reinterpret_cast<uintptr_t>(aligned_buf) % sector;
  if (misalign) aligned_buf += sector - misalign;
  size_t aligned_size = static_cast<size_t>(aligned_end - aligned_start);
  if (!RawReadAligned(aligned_start, aligned_buf, aligned_size)) return false;
  std::memcpy(out, aligned_buf + (offset - aligned_start), size);
  return true;
}

bool WindowsRawBlockDevice::WriteExact(uint64_t offset, const void* buffer,
                                       size_t size) {
  const uint32_t sector = info_.logical_sector_size;
  if (size == 0) return true;
  if (!writable_) {
    SetError("device opened read-only");
    return false;
  }
  if (offset > info_.size_bytes || size > info_.size_bytes - offset) {
    SetError("write out of bounds");
    return false;
  }
  uint8_t* in = const_cast<uint8_t*>(static_cast<const uint8_t*>(buffer));
  uint64_t aligned_start = offset & ~static_cast<uint64_t>(sector - 1);
  uint64_t aligned_end =
      (offset + size + sector - 1) & ~static_cast<uint64_t>(sector - 1);

  if (aligned_start == offset &&
      (reinterpret_cast<uintptr_t>(buffer) % sector) == 0 &&
      size % sector == 0) {
    return RawWriteAligned(offset, buffer, size);
  }

  // Read-modify-write through an aligned bounce buffer.
  std::vector<uint8_t> scratch(
      static_cast<size_t>(aligned_end - aligned_start) + sector);
  uint8_t* aligned_buf = scratch.data();
  uintptr_t misalign = reinterpret_cast<uintptr_t>(aligned_buf) % sector;
  if (misalign) aligned_buf += sector - misalign;
  size_t aligned_size = static_cast<size_t>(aligned_end - aligned_start);
  if (!RawReadAligned(aligned_start, aligned_buf, aligned_size)) return false;
  std::memcpy(aligned_buf + (offset - aligned_start), in, size);
  return RawWriteAligned(aligned_start, aligned_buf, aligned_size);
}

bool WindowsRawBlockDevice::Read(uint64_t offset, void* buffer, size_t size) {
  std::shared_lock<std::shared_mutex> lock(io_mutex_);
  if (impl_->handle == INVALID_HANDLE_VALUE) {
    SetError("device not open");
    return false;
  }
  return ReadExact(offset, buffer, size);
}

bool WindowsRawBlockDevice::Write(uint64_t offset, const void* buffer,
                                  size_t size) {
  std::unique_lock<std::shared_mutex> lock(io_mutex_);
  if (impl_->handle == INVALID_HANDLE_VALUE) {
    SetError("device not open");
    return false;
  }
  return WriteExact(offset, buffer, size);
}

bool WindowsRawBlockDevice::Flush() {
  std::unique_lock<std::shared_mutex> lock(io_mutex_);
  if (impl_->handle == INVALID_HANDLE_VALUE) {
    SetError("device not open");
    return false;
  }
  if (!FlushFileBuffers(impl_->handle)) {
    SetError("FlushFileBuffers failed: " + WinErrorString(GetLastError()));
    return false;
  }
  return true;
}

bool WindowsRawBlockDevice::LockVolumes(std::string* error) {
  if (impl_->volumes_locked) return true;
  if (impl_->handle == INVALID_HANDLE_VALUE) {
    if (error) *error = "device not open";
    return false;
  }

  // Find our disk number.
  STORAGE_DEVICE_NUMBER sdn{};
  DWORD bytes = 0;
  if (!DeviceIoControl(impl_->handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr,
                       0, &sdn, sizeof(sdn), &bytes, nullptr)) {
    if (error) {
      *error = "cannot query device number: " +
               WinErrorString(GetLastError());
    }
    return false;
  }

  // Enumerate volumes and lock/dismount the ones on this disk.
  WCHAR volume_name[MAX_PATH];
  HANDLE find = FindFirstVolumeW(volume_name, MAX_PATH);
  bool ok = true;
  if (find != INVALID_HANDLE_VALUE) {
    do {
      size_t len = wcslen(volume_name);
      if (len && volume_name[len - 1] == L'\\') {
        volume_name[len - 1] = L'\0';  // strip trailing backslash
      }
      HANDLE vol = CreateFileW(volume_name, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
      if (vol != INVALID_HANDLE_VALUE) {
        uint32_t disk_number = 0;
        if (VolumeDiskNumber(vol, &disk_number, nullptr) &&
            disk_number == sdn.DeviceNumber) {
          DWORD returned = 0;
          if (!DeviceIoControl(vol, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                               &returned, nullptr)) {
            ok = false;
            if (error) {
              *error = "cannot lock volume " + WideToUtf8(volume_name) + ": " +
                       WinErrorString(GetLastError());
            }
          } else {
            DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0,
                            &returned, nullptr);
            impl_->locked_volumes.push_back(vol);
            continue;  // keep the handle open (lock is released on close)
          }
        }
        CloseHandle(vol);
      }
    } while (FindNextVolumeW(find, volume_name, MAX_PATH));
    FindVolumeClose(find);
  }
  impl_->volumes_locked = ok;
  return ok;
}

void WindowsRawBlockDevice::Close() {
  if (!impl_) return;
  for (HANDLE vol : impl_->locked_volumes) {
    DWORD returned = 0;
    DeviceIoControl(vol, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0,
                    &returned, nullptr);
    CloseHandle(vol);
  }
  impl_->locked_volumes.clear();
  if (impl_->handle != INVALID_HANDLE_VALUE) {
    CloseHandle(impl_->handle);
    impl_->handle = INVALID_HANDLE_VALUE;
  }
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

std::vector<RawDiskInfo> EnumeratePhysicalDrives(std::string* error) {
  std::vector<RawDiskInfo> disks;

  // The system disk: the disk that hosts %SystemDrive%.
  uint32_t system_disk = 0xffffffffu;
  {
    WCHAR system_root[MAX_PATH];
    if (GetWindowsDirectoryW(system_root, MAX_PATH)) {
      std::wstring volume = L"\\\\.\\";
      volume.push_back(system_root[0]);
      volume.push_back(L':');
      HANDLE h = CreateFileW(volume.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        VolumeDiskNumber(h, &system_disk, nullptr);
        CloseHandle(h);
      }
    }
  }

  for (uint32_t number = 0; number < 64; ++number) {
    std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      if (number == 0) {
        if (error) {
          DWORD ec = GetLastError();
          if (ec == ERROR_ACCESS_DENIED) {
            *error =
                "access to physical drives requires Administrator "
                "privileges; run xfswin from an elevated prompt";
          } else {
            *error = "cannot open PhysicalDrive0: " + WinErrorString(ec);
          }
        }
        return disks;
      }
      break;  // no more disks
    }

    RawDiskInfo info;
    info.disk_number = number;
    info.device_path = WideToUtf8(path);

    DISK_GEOMETRY_EX geometry{};
    DWORD bytes = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                        &geometry, sizeof(geometry), &bytes, nullptr)) {
      info.logical_sector_size = geometry.Geometry.BytesPerSector;
      info.size_bytes = static_cast<uint64_t>(geometry.DiskSize.QuadPart);
    }

    DWORD prop_size = 0;
    std::string prop = QueryDeviceProperty(
        h, StorageAccessAlignmentProperty, PropertyStandardQuery, &prop_size);
    if (prop.size() >= sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR)) {
      info.physical_sector_size =
          reinterpret_cast<const STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR*>(
              prop.data())
              ->BytesPerPhysicalSector;
    }
    if (!info.physical_sector_size) {
      info.physical_sector_size = info.logical_sector_size;
    }

    prop = QueryDeviceProperty(h, StorageDeviceProperty,
                               PropertyStandardQuery, &prop_size);
    if (prop.size() >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
      const auto* desc =
          reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(prop.data());
      if (desc->ProductIdOffset && desc->ProductIdOffset < prop.size()) {
        info.model = prop.data() + desc->ProductIdOffset;
        while (!info.model.empty() && info.model.back() == ' ') {
          info.model.pop_back();
        }
      }
      info.is_removable = desc->RemovableMedia != 0;
    }

    // Partition table.
    std::vector<uint8_t> layout_buf(64 * 1024);
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
                        layout_buf.data(),
                        static_cast<DWORD>(layout_buf.size()), &bytes,
                        nullptr)) {
      const auto* layout =
          reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(
              layout_buf.data());
      bool gpt = layout->PartitionStyle == PARTITION_STYLE_GPT;
      auto guid_is_zero = [](const GUID& g) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&g);
        for (int k = 0; k < 16; ++k) {
          if (bytes[k] != 0) return false;
        }
        return true;
      };
      for (DWORD i = 0; i < layout->PartitionCount; ++i) {
        const PARTITION_INFORMATION_EX& part = layout->PartitionEntry[i];
        if (gpt) {
          if (guid_is_zero(part.Gpt.PartitionType)) continue;
        } else {
          if (part.Mbr.PartitionType == 0) continue;
        }
        RawPartitionInfo pi;
        pi.number = i + 1;
        pi.offset_bytes =
            static_cast<uint64_t>(part.StartingOffset.QuadPart);
        pi.length_bytes =
            static_cast<uint64_t>(part.PartitionLength.QuadPart);
        pi.bootable = part.Mbr.BootIndicator != 0;
        pi.gpt = gpt;
        if (gpt) {
          char buf[64];
          std::snprintf(buf, sizeof(buf),
                        "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                        part.Gpt.PartitionType.Data1,
                        part.Gpt.PartitionType.Data2,
                        part.Gpt.PartitionType.Data3,
                        part.Gpt.PartitionType.Data4[0],
                        part.Gpt.PartitionType.Data4[1],
                        part.Gpt.PartitionType.Data4[2],
                        part.Gpt.PartitionType.Data4[3],
                        part.Gpt.PartitionType.Data4[4],
                        part.Gpt.PartitionType.Data4[5],
                        part.Gpt.PartitionType.Data4[6],
                        part.Gpt.PartitionType.Data4[7]);
          pi.type = buf;
        } else {
          char buf[16];
          std::snprintf(buf, sizeof(buf), "0x%02X", part.Mbr.PartitionType);
          pi.type = buf;
        }
        info.partitions.push_back(pi);
      }
    }

    info.is_system_disk = (number == system_disk);
    disks.push_back(std::move(info));
    CloseHandle(h);
  }

  // Pagefile volumes: mark disks that hold an active pagefile.
  WCHAR pagefiles[1024] = {};
  DWORD type = 0;
  DWORD size = sizeof(pagefiles);
  if (RegGetValueW(HKEY_LOCAL_MACHINE,
                   L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\"
                   L"Memory Management",
                   L"PagingFiles", RRF_RT_REG_MULTI_SZ, &type, pagefiles,
                   &size) == ERROR_SUCCESS) {
    for (const WCHAR* p = pagefiles; *p;) {
      std::wstring entry(p);
      if (entry.size() >= 2 && entry[1] == L':') {
        std::wstring volume = L"\\\\.\\";
        volume.push_back(entry[0]);
        volume.push_back(L':');
        HANDLE h = CreateFileW(volume.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
          uint32_t disk_number = 0;
          if (VolumeDiskNumber(h, &disk_number, nullptr)) {
            for (auto& disk : disks) {
              if (disk.disk_number == disk_number) disk.has_pagefile = true;
            }
          }
          CloseHandle(h);
        }
      }
      p += entry.size() + 1;
    }
  }

  return disks;
}

}  // namespace xfs

#else  // !_WIN32

namespace xfs {

std::vector<RawDiskInfo> EnumeratePhysicalDrives(std::string* error) {
  if (error) *error = "raw devices are only supported on Windows";
  return {};
}

std::unique_ptr<BlockDevice> OpenRawDevice(const std::wstring&, bool, bool,
                                           std::string* error) {
  if (error) *error = "raw devices are only supported on Windows";
  return nullptr;
}

}  // namespace xfs

#endif  // _WIN32
