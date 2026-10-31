// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/windows_raw_device.h"

#include "xfs/log.h"

#ifdef _WIN32

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef DEVICE_TYPE
#define DEVICE_TYPE DWORD
#endif

namespace xfs {
namespace {

constexpr size_t kMaxTransferChunk = 1024 * 1024;  // 1 MiB per ReadFile/WriteFile
constexpr size_t kMaxPropertyBuffer = 16 * 1024;

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
  if (len <= 0) return {};
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

std::string FormatWinError(DWORD code) {
  return " (error " + std::to_string(code) + ": " + WinErrorString(code) + ")";
}

std::string TrimSpaces(std::string value) {
  while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) {
    value.pop_back();
  }
  size_t begin = 0;
  while (begin < value.size() && value[begin] == ' ') ++begin;
  return value.substr(begin);
}

std::string GuidToString(const GUID& guid) {
  char buf[64];
  std::snprintf(buf, sizeof(buf),
                "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3,
                guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
  return buf;
}

bool GuidIsZero(const GUID& guid) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&guid);
  for (int i = 0; i < 16; ++i) {
    if (bytes[i] != 0) return false;
  }
  return true;
}

// Queries a storage property.  Returns the raw bytes; an empty string means
// the device did not answer.
std::string QueryDeviceProperty(HANDLE h, STORAGE_PROPERTY_ID property,
                                STORAGE_QUERY_TYPE type, DWORD* out_size) {
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = property;
  query.QueryType = type;
  std::vector<uint8_t> buffer(kMaxPropertyBuffer);
  DWORD bytes = 0;
  if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                       buffer.data(), static_cast<DWORD>(buffer.size()),
                       &bytes, nullptr)) {
    if (out_size) *out_size = 0;
    return {};
  }
  if (out_size) *out_size = bytes;
  return std::string(reinterpret_cast<char*>(buffer.data()), bytes);
}

bool QueryDeviceNumber(HANDLE h, STORAGE_DEVICE_NUMBER* out) {
  DWORD bytes = 0;
  return DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, out,
                         sizeof(*out), &bytes, nullptr) != FALSE;
}

// STORAGE_DEVICE_NUMBER_EX is available on Windows 10 1903+.  Use the SDK
// layout (Version, Size, Flags, DeviceType, DeviceNumber, DeviceGuid,
// PartitionNumber) and validate what the driver returned; SDKs without the
// IOCTL fall back to the serial number for identity.
#ifdef IOCTL_STORAGE_GET_DEVICE_NUMBER_EX
bool QueryDeviceNumberEx(HANDLE h, STORAGE_DEVICE_NUMBER_EX* out) {
  std::memset(out, 0, sizeof(*out));
  DWORD bytes = 0;
  if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER_EX, nullptr, 0, out,
                       sizeof(*out), &bytes, nullptr)) {
    return false;
  }
  if (bytes < sizeof(*out)) return false;
  if (out->Version == 0) return false;
  if (out->Size < sizeof(*out)) return false;
  // A randomly generated GUID is not stable across reboots, and a conflict
  // means two devices share identifying hardware; neither is usable.
  if ((out->Flags & STORAGE_DEVICE_FLAGS_RANDOM_DEVICEGUID_REASON_CONFLICT) ||
      (out->Flags & STORAGE_DEVICE_FLAGS_RANDOM_DEVICEGUID_REASON_NOHWID)) {
    return false;
  }
  return true;
}
#endif

bool QueryDiskAttributes(HANDLE h, bool* offline, bool* read_only) {
  GET_DISK_ATTRIBUTES attributes{};
  attributes.Version = sizeof(GET_DISK_ATTRIBUTES);
  DWORD bytes = 0;
  if (!DeviceIoControl(h, IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0,
                       &attributes, sizeof(attributes), &bytes, nullptr)) {
    return false;
  }
  if (offline) *offline = (attributes.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0;
  if (read_only) {
    *read_only = (attributes.Attributes & DISK_ATTRIBUTE_READ_ONLY) != 0;
  }
  return true;
}

// IOCTL_DISK_IS_WRITABLE is definitive only when it succeeds or reports
// ERROR_WRITE_PROTECT.  Other failures mean "unknown" and must not make a
// writable VHD look read-only.
bool QueryIsWritable(HANDLE h, bool* writable) {
  DWORD bytes = 0;
  if (DeviceIoControl(h, IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0,
                      &bytes, nullptr)) {
    *writable = true;
    return true;
  }
  if (GetLastError() == ERROR_WRITE_PROTECT) {
    *writable = false;
    return true;
  }
  return false;
}

// Returns the disk number hosting the Windows system volume, or 0xffffffff.
uint32_t SystemDiskNumber() {
  WCHAR windows_dir[MAX_PATH];
  if (!GetWindowsDirectoryW(windows_dir, MAX_PATH)) return 0xffffffffu;
  std::wstring volume = L"\\\\.\\";
  volume.push_back(windows_dir[0]);
  volume.push_back(L':');
  HANDLE h = CreateFileW(volume.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return 0xffffffffu;
  STORAGE_DEVICE_NUMBER sdn{};
  uint32_t disk = 0xffffffffu;
  if (QueryDeviceNumber(h, &sdn)) disk = sdn.DeviceNumber;
  CloseHandle(h);
  return disk;
}

std::set<uint32_t> PagefileDiskNumbers(bool* complete = nullptr) {
  std::set<uint32_t> disks;
  if (complete) *complete = true;
  WCHAR pagefiles[4096] = {};
  DWORD type = 0;
  DWORD size = sizeof(pagefiles);
  const LSTATUS status = RegGetValueW(
      HKEY_LOCAL_MACHINE,
      L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\"
      L"Memory Management",
      L"PagingFiles", RRF_RT_REG_MULTI_SZ, &type, pagefiles, &size);
  if (status == ERROR_FILE_NOT_FOUND) {
    // No pagefile is configured.
    return disks;
  }
  if (status != ERROR_SUCCESS) {
    // The configuration exists but cannot be read; callers that need a
    // safety decision must treat this as unknown.
    if (complete) *complete = false;
    return disks;
  }
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
        STORAGE_DEVICE_NUMBER sdn{};
        if (QueryDeviceNumber(h, &sdn)) {
          disks.insert(sdn.DeviceNumber);
        } else if (complete) {
          *complete = false;
        }
        CloseHandle(h);
      } else if (complete) {
        *complete = false;
      }
    }
    p += entry.size() + 1;
  }
  return disks;
}

// True when `volume` has at least one extent on `disk_number`.
// Returns 1 when the volume is on `disk_number`, 0 when it is provably on
// another disk, and -1 when the relationship cannot be determined.  Unknown
// must refuse the writable open: a volume we cannot classify could still be
// mounted on the disk we are about to write.
//
// A volume may span several disks, so the extent array is variable length.
// Start with room for 32 extents and retry with a larger buffer when the
// driver answers ERROR_MORE_DATA.  Any other failure is "unknown": a volume
// whose extents we cannot read must not be silently treated as belonging to
// another disk.
int VolumeDiskRelation(HANDLE volume, uint32_t disk_number) {
  constexpr size_t kInitialExtents = 32;
  std::vector<uint8_t> buffer(sizeof(VOLUME_DISK_EXTENTS) +
                              (kInitialExtents - 1) * sizeof(DISK_EXTENT));
  for (int attempt = 0; attempt < 3; ++attempt) {
    DWORD bytes = 0;
    if (DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr,
                        0, buffer.data(), static_cast<DWORD>(buffer.size()),
                        &bytes, nullptr)) {
      if (bytes < sizeof(VOLUME_DISK_EXTENTS)) return -1;
      const auto* extents =
          reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
      const size_t count = extents->NumberOfDiskExtents;
      const size_t needed =
          sizeof(VOLUME_DISK_EXTENTS) +
          (count > 0 ? (count - 1) * sizeof(DISK_EXTENT) : 0);
      if (count > 0 && needed > bytes) return -1;
      for (size_t i = 0; i < count; ++i) {
        if (extents->Extents[i].DiskNumber == disk_number) return 1;
      }
      return 0;
    }
    if (GetLastError() != ERROR_MORE_DATA) return -1;
    // The driver needs a larger buffer.  It normally reports the required
    // extent count; if it did not, double the buffer for the next attempt.
    size_t next_extents = kInitialExtents;
    if (bytes >= sizeof(VOLUME_DISK_EXTENTS)) {
      const auto* extents =
          reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
      next_extents = std::max<size_t>(extents->NumberOfDiskExtents,
                                      kInitialExtents);
    } else {
      next_extents = kInitialExtents << (attempt + 1);
    }
    if (next_extents > 4096) return -1;
    const size_t needed = sizeof(VOLUME_DISK_EXTENTS) +
                          (next_extents - 1) * sizeof(DISK_EXTENT);
    if (needed <= buffer.size()) return -1;  // no progress; avoid a loop
    buffer.resize(needed);
  }
  return -1;
}

DeviceBusType MapBusType(STORAGE_BUS_TYPE bus) {
  switch (bus) {
    case BusTypeScsi: return DeviceBusType::kScsi;
    case BusTypeAtapi: return DeviceBusType::kAtapi;
    case BusTypeAta: return DeviceBusType::kAta;
    case BusType1394: return DeviceBusType::k1394;
    case BusTypeSsa: return DeviceBusType::kSsa;
    case BusTypeFibre: return DeviceBusType::kFibre;
    case BusTypeUsb: return DeviceBusType::kUsb;
    case BusTypeRAID: return DeviceBusType::kRaid;
    case BusTypeiScsi: return DeviceBusType::kIscsi;
    case BusTypeSas: return DeviceBusType::kSas;
    case BusTypeSata: return DeviceBusType::kSata;
    case BusTypeSd: return DeviceBusType::kSd;
    case BusTypeMmc: return DeviceBusType::kMmc;
    case BusTypeVirtual: return DeviceBusType::kVirtual;
    case BusTypeFileBackedVirtual: return DeviceBusType::kFileBackedVirtual;
    case BusTypeSpaces: return DeviceBusType::kSpaces;
    case BusTypeNvme: return DeviceBusType::kNvme;
    case BusTypeSCM: return DeviceBusType::kScm;
    case BusTypeUfs: return DeviceBusType::kUfs;
    default: return DeviceBusType::kOther;
  }
}

bool BusIsVirtual(DeviceBusType bus) {
  return bus == DeviceBusType::kVirtual ||
         bus == DeviceBusType::kFileBackedVirtual;
}

DeviceClass ClassifyDisk(bool removable, DeviceBusType bus) {
  if (BusIsVirtual(bus)) return DeviceClass::kVirtualDisk;
  if (removable || bus == DeviceBusType::kUsb) {
    return DeviceClass::kRemovableDisk;
  }
  return DeviceClass::kFixedDisk;
}

// Per-thread manual-reset event for overlapped raw-device transfers.
struct ThreadEvent {
  HANDLE handle = INVALID_HANDLE_VALUE;
  ThreadEvent() { handle = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
  ~ThreadEvent() {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};

HANDLE TransferEvent() {
  static thread_local ThreadEvent event;
  return event.handle;
}

std::string EnvironmentValue(const wchar_t* name) {
  DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (size == 0) return {};
  std::wstring value(size, L'\0');
  DWORD written = GetEnvironmentVariableW(name, value.data(), size);
  if (written == 0 || written >= size) return {};
  value.resize(written);
  return WideToUtf8(value);
}

}  // namespace

// ---------------------------------------------------------------------------
// Implementation state
// ---------------------------------------------------------------------------
struct WindowsRawBlockDevice::Impl {
  HANDLE handle = INVALID_HANDLE_VALUE;
  std::wstring path;
  std::vector<HANDLE> locked_volumes;
  // Read by VolumeOwnershipConfirmed() without ownership_mutex, written by
  // LockVolumes()/Close() while holding it.
  std::atomic<bool> volumes_locked{false};
  std::mutex ownership_mutex;
  DeviceIdentity identity;
  DeviceOpenPolicy policy;

  DeviceFaultPlan faults;
  std::mutex fault_mutex;
  uint32_t read_calls = 0;
  uint32_t write_calls = 0;
  uint32_t flush_calls = 0;
  uint64_t operation_count = 0;

  std::FILE* trace = nullptr;
  std::mutex trace_mutex;
  uint64_t trace_sequence = 0;
};

namespace {

void TraceOperation(WindowsRawBlockDevice::Impl* impl, const char* op,
                    uint64_t offset, uint64_t size, bool ok, uint32_t error,
                    DeviceState state) {
  if (impl == nullptr || impl->trace == nullptr) return;
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  std::lock_guard<std::mutex> lock(impl->trace_mutex);
  std::fprintf(impl->trace,
               "{\"seq\":%llu,\"qpc\":%lld,\"op\":\"%s\",\"offset\":%llu,"
               "\"size\":%llu,\"ok\":%s,\"error\":%u,\"state\":\"%s\"}\n",
               static_cast<unsigned long long>(++impl->trace_sequence),
               static_cast<long long>(counter.QuadPart), op,
               static_cast<unsigned long long>(offset),
               static_cast<unsigned long long>(size), ok ? "true" : "false",
               error, ToString(state));
  std::fflush(impl->trace);
}

void ReleaseLockedVolumes(WindowsRawBlockDevice::Impl* impl) {
  for (HANDLE volume : impl->locked_volumes) {
    DWORD returned = 0;
    DeviceIoControl(volume, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0,
                    &returned, nullptr);
    CloseHandle(volume);
  }
  impl->locked_volumes.clear();
  impl->volumes_locked = false;
}

}  // namespace

WindowsRawBlockDevice::~WindowsRawBlockDevice() { Close(); }

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------
std::unique_ptr<WindowsRawBlockDevice> WindowsRawBlockDevice::Open(
    const std::wstring& path, const DeviceOpenPolicy& policy,
    std::string* error) {
  auto fail = [&](std::string message)
      -> std::unique_ptr<WindowsRawBlockDevice> {
    if (error) *error = std::move(message);
    return nullptr;
  };

  DeviceOpenPolicy effective = policy;
  if (effective.writable) {
    // A writer must own the disk.  Sharing with another writer is never
    // allowed; sharing with readers is not enough either, because a reader can
    // keep the volume mounted and block ownership.
    effective.exclusive = true;
  }
  // Qualification matrix only: XFSWIN_DEVICE_CACHE_MODE lets a run compare
  // buffered / write-through / no-buffering behavior.  The default policy is
  // not changed by this variable.
  const std::string cache_override =
      EnvironmentValue(L"XFSWIN_DEVICE_CACHE_MODE");
  if (!cache_override.empty()) {
    DeviceCacheMode parsed = DeviceCacheMode::kBuffered;
    if (ParseDeviceCacheMode(cache_override, &parsed)) {
      effective.cache_mode = parsed;
    }
  }

  DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED;
  switch (effective.cache_mode) {
    case DeviceCacheMode::kBuffered:
      break;
    case DeviceCacheMode::kWriteThrough:
      flags |= FILE_FLAG_WRITE_THROUGH;
      break;
    case DeviceCacheMode::kNoBuffering:
      flags |= FILE_FLAG_NO_BUFFERING;
      break;
    case DeviceCacheMode::kNoBufferingWriteThrough:
      flags |= FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
      break;
  }
  const DWORD share = effective.exclusive
                          ? 0
                          : (FILE_SHARE_READ | FILE_SHARE_WRITE);
  const DWORD access = GENERIC_READ | (effective.writable ? GENERIC_WRITE : 0);

  HANDLE handle = CreateFileW(path.c_str(), access, share, nullptr,
                              OPEN_EXISTING, flags, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    return fail("cannot open " + WideToUtf8(path) + FormatWinError(code) +
                (effective.exclusive ? " (exclusive access requested)" : ""));
  }

  auto dev = std::unique_ptr<WindowsRawBlockDevice>(new WindowsRawBlockDevice());
  dev->impl_ = std::make_unique<Impl>();
  dev->impl_->handle = handle;
  dev->impl_->path = path;
  dev->impl_->policy = effective;
  dev->writable_ = effective.writable;
  dev->exclusive_ = effective.exclusive;
  dev->state_.store(DeviceState::kOpening);

  auto close_and_fail = [&](std::string message)
      -> std::unique_ptr<WindowsRawBlockDevice> {
    ReleaseLockedVolumes(dev->impl_.get());
    CloseHandle(handle);
    dev->impl_->handle = INVALID_HANDLE_VALUE;
    dev->state_.store(DeviceState::kClosed);
    return fail(std::move(message));
  };

  // --- identity ---------------------------------------------------------
  STORAGE_DEVICE_NUMBER sdn{};
  const bool have_sdn = QueryDeviceNumber(handle, &sdn);
  if (have_sdn) {
    dev->info_.disk_number = sdn.DeviceNumber;
    dev->info_.partition_number = sdn.PartitionNumber;
  }
  dev->impl_->identity.path = WideToUtf8(path);
  dev->impl_->identity.disk_number = dev->info_.disk_number;
  dev->impl_->identity.partition_number = dev->info_.partition_number;

  // --- bus type / removable / model / serial ----------------------------
  DWORD property_size = 0;
  std::string property = QueryDeviceProperty(
      handle, StorageDeviceProperty, PropertyStandardQuery, &property_size);
  DeviceBusType bus_type = DeviceBusType::kUnknown;
  bool removable_media = false;
  std::string model;
  std::string serial;
  if (property.size() >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
    const auto* desc =
        reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(property.data());
    bus_type = MapBusType(desc->BusType);
    removable_media = desc->RemovableMedia != 0;
    if (desc->ProductIdOffset != 0 &&
        desc->ProductIdOffset < property.size()) {
      model = TrimSpaces(property.data() + desc->ProductIdOffset);
    }
    if (desc->SerialNumberOffset != 0 &&
        desc->SerialNumberOffset < property.size()) {
      serial = TrimSpaces(property.data() + desc->SerialNumberOffset);
    }
  }

  // --- stable identity --------------------------------------------------
  std::string stable_id;
#ifdef IOCTL_STORAGE_GET_DEVICE_NUMBER_EX
  STORAGE_DEVICE_NUMBER_EX sdnx{};
  if (QueryDeviceNumberEx(handle, &sdnx) && !GuidIsZero(sdnx.DeviceGuid)) {
    stable_id = GuidToString(sdnx.DeviceGuid);
  }
#endif
  if (stable_id.empty()) {
    // Fallback: a serial number is stable per physical device; if the device
    // does not expose one, bind the identity to the model and size.  This is
    // diagnostic only: the backend never reopens a device by identity.
    stable_id = serial.empty() ? (model + "/" + std::to_string(dev->info_.disk_number))
                               : ("serial:" + serial);
  }
  dev->impl_->identity.stable_id = stable_id;
  dev->impl_->identity.serial_number = serial;
  dev->impl_->identity.model = model;
  dev->info_.stable_id = stable_id;
  dev->info_.serial_number = serial;

  // --- geometry ---------------------------------------------------------
  DeviceGeometry geometry;
  DWORD alignment_size = 0;
  std::string alignment_property = QueryDeviceProperty(
      handle, StorageAccessAlignmentProperty, PropertyStandardQuery,
      &alignment_size);
  if (alignment_property.size() >=
      sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR)) {
    const auto* alignment =
        reinterpret_cast<const STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR*>(
            alignment_property.data());
    geometry.logical_sector_size = alignment->BytesPerLogicalSector;
    geometry.physical_sector_size = alignment->BytesPerPhysicalSector;
    geometry.alignment_offset = alignment->BytesOffsetForSectorAlignment;
  }

  DISK_GEOMETRY_EX disk_geometry{};
  DWORD bytes = 0;
  bool have_geometry = false;
  std::vector<uint8_t> geometry_buffer(sizeof(DISK_GEOMETRY_EX) + 1024);
  if (DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                      geometry_buffer.data(),
                      static_cast<DWORD>(geometry_buffer.size()), &bytes,
                      nullptr)) {
    const auto* geometry_ex =
        reinterpret_cast<const DISK_GEOMETRY_EX*>(geometry_buffer.data());
    disk_geometry = *geometry_ex;
    have_geometry = true;
  } else {
    DISK_GEOMETRY legacy{};
    if (DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0,
                        &legacy, sizeof(legacy), &bytes, nullptr)) {
      disk_geometry.Geometry = legacy;
      have_geometry = true;
    }
  }

  if (geometry.logical_sector_size == 0 && have_geometry) {
    geometry.logical_sector_size = disk_geometry.Geometry.BytesPerSector;
  }
  if (geometry.physical_sector_size == 0) {
    geometry.physical_sector_size = geometry.logical_sector_size;
  }
  if (have_geometry && disk_geometry.DiskSize.QuadPart > 0) {
    // A negative capacity is a driver error; casting it to uint64_t would
    // turn it into a huge, apparently valid size.
    geometry.size_bytes =
        static_cast<uint64_t>(disk_geometry.DiskSize.QuadPart);
  }
  if (geometry.size_bytes == 0) {
    GET_LENGTH_INFORMATION length{};
    if (DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length,
                        sizeof(length), &bytes, nullptr) &&
        length.Length.QuadPart > 0) {
      geometry.size_bytes = static_cast<uint64_t>(length.Length.QuadPart);
    } else {
      LARGE_INTEGER file_size{};
      if (GetFileSizeEx(handle, &file_size) && file_size.QuadPart > 0) {
        geometry.size_bytes = static_cast<uint64_t>(file_size.QuadPart);
      }
    }
  }
  if (have_geometry && geometry.logical_sector_size != 0 &&
      disk_geometry.Geometry.BytesPerSector != 0 &&
      geometry.logical_sector_size != disk_geometry.Geometry.BytesPerSector) {
    // Two independent queries disagree.  A read-only inspection can use the
    // larger value for alignment; a writer must not guess.
    if (effective.writable) {
      return close_and_fail(
          "device reports inconsistent logical sector sizes (" +
          std::to_string(geometry.logical_sector_size) + " vs " +
          std::to_string(disk_geometry.Geometry.BytesPerSector) + ")");
    }
    geometry.logical_sector_size =
        std::max(geometry.logical_sector_size,
                 static_cast<uint32_t>(disk_geometry.Geometry.BytesPerSector));
  }

  std::string geometry_reason;
  if (!IsValidDeviceGeometry(geometry, &geometry_reason)) {
    return close_and_fail("unusable device geometry: " + geometry_reason);
  }

  // --- offline / read-only ---------------------------------------------
  bool offline = false;
  bool read_only_media = false;
  QueryDiskAttributes(handle, &offline, &read_only_media);
  bool disk_writable = true;
  if (QueryIsWritable(handle, &disk_writable) && !disk_writable) {
    read_only_media = true;
  }

  // --- class ------------------------------------------------------------
  const std::string utf8_path = WideToUtf8(path);
  const bool volume_path =
      utf8_path.find("HarddiskVolume") != std::string::npos ||
      utf8_path.find("Volume{") != std::string::npos ||
      utf8_path.find("\\\\.\\Volume") != std::string::npos ||
      utf8_path.find("//./Volume") != std::string::npos;
  DeviceClass device_class = DeviceClass::kUnknown;
  if (have_sdn && sdn.DeviceType == FILE_DEVICE_DISK_FILE_SYSTEM) {
    device_class = DeviceClass::kVolume;
  } else if (volume_path) {
    device_class = DeviceClass::kVolume;
  } else if ((have_sdn && (sdn.DeviceType == FILE_DEVICE_DISK ||
                           sdn.DeviceType == FILE_DEVICE_VIRTUAL_DISK)) ||
             utf8_path.find("PhysicalDrive") != std::string::npos ||
             utf8_path.find("physicaldrive") != std::string::npos) {
    device_class = ClassifyDisk(removable_media, bus_type);
  }

  const bool is_disk = device_class == DeviceClass::kVirtualDisk ||
                       device_class == DeviceClass::kRemovableDisk ||
                       device_class == DeviceClass::kFixedDisk;
  const uint32_t system_disk = is_disk ? SystemDiskNumber() : 0xffffffffu;
  bool pagefile_known = true;
  const std::set<uint32_t> pagefile_disks =
      is_disk ? PagefileDiskNumbers(&pagefile_known) : std::set<uint32_t>();
  const bool is_system_disk =
      is_disk && have_sdn && system_disk != 0xffffffffu &&
      dev->info_.disk_number == system_disk;
  const bool has_pagefile =
      is_disk && have_sdn && pagefile_disks.count(dev->info_.disk_number) != 0;

  dev->info_.size_bytes = geometry.size_bytes;
  dev->info_.logical_sector_size = geometry.logical_sector_size;
  dev->info_.physical_sector_size = geometry.physical_sector_size;
  dev->info_.alignment_offset = geometry.alignment_offset;
  dev->info_.writable = effective.writable;
  dev->info_.is_raw_device = true;
  dev->info_.device_class = device_class;
  dev->info_.bus_type = bus_type;
  dev->info_.cache_mode = effective.cache_mode;
  dev->info_.removable_media = removable_media;
  dev->info_.offline = offline;
  dev->info_.read_only_media = read_only_media;
  dev->info_.is_system_disk = is_system_disk;
  dev->info_.has_pagefile = has_pagefile;
  dev->info_.description = model.empty() ? utf8_path : model;

  // --- policy -----------------------------------------------------------
  if (effective.writable) {
    if (!have_sdn) {
      return close_and_fail(
          "cannot query the storage device number; refusing a writable open "
          "because the system/pagefile safety checks need it");
    }
    if (is_disk && system_disk == 0xffffffffu) {
      return close_and_fail(
          "cannot determine the Windows system disk; refusing a writable "
          "whole-disk open");
    }
    if (is_disk && !pagefile_known) {
      return close_and_fail(
          "cannot determine which disks hold a pagefile; refusing a writable "
          "whole-disk open");
    }
    std::string reason;
    WriteEligibility eligibility = EvaluateDeviceWriteClass(
        device_class, geometry, read_only_media, offline, is_system_disk,
        has_pagefile, effective, &reason);
    if (eligibility != WriteEligibility::kEligible) {
      return close_and_fail("writable open refused (" +
                            std::string(ToString(eligibility)) + "): " +
                            reason);
    }
    if (effective.cache_mode == DeviceCacheMode::kNoBuffering ||
        effective.cache_mode == DeviceCacheMode::kNoBufferingWriteThrough) {
      if (geometry.alignment_offset != 0) {
        return close_and_fail(
            "unbuffered writable I/O requires a zero alignment offset");
      }
    }

    // Volume ownership: lock and dismount every volume on the disk.  This is
    // what prevents Windows from mounting or writing the same range while the
    // XFS writer is active.
    if (!dev->LockVolumes(error)) {
      return close_and_fail(error && !error->empty()
                                ? *error
                                : "cannot acquire volume ownership");
    }
    dev->info_.ownership_confirmed = true;
    eligibility = EvaluateDeviceWriteEligibility(
        device_class, geometry, read_only_media, offline, is_system_disk,
        has_pagefile, /*ownership_confirmed=*/true, effective, &reason);
    if (eligibility != WriteEligibility::kEligible) {
      return close_and_fail("writable open refused (" +
                            std::string(ToString(eligibility)) + "): " +
                            reason);
    }
  }

  // --- optional fault injection / trace (qualification only) -----------
  const std::string fault_text = EnvironmentValue(L"XFSWIN_DEVICE_FAULT");
  if (!fault_text.empty()) {
    dev->impl_->faults = DeviceFaultPlan::Parse(fault_text);
  }
  const std::string trace_path = EnvironmentValue(L"XFSWIN_DEVICE_TRACE");
  if (!trace_path.empty()) {
    // The trace path is a UTF-8 narrow path.  Use _wfopen with a converted
    // wide string so non-ASCII paths work.
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, trace_path.data(),
                                       static_cast<int>(trace_path.size()),
                                       nullptr, 0);
    std::wstring wide_path(static_cast<size_t>(wide_len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, trace_path.data(),
                        static_cast<int>(trace_path.size()), wide_path.data(),
                        wide_len);
    FILE* trace = nullptr;
    if (_wfopen_s(&trace, wide_path.c_str(), L"ab") == 0) {
      dev->impl_->trace = trace;
    }
  }

  dev->state_.store(DeviceState::kOpen);
  TraceOperation(dev->impl_.get(), "open", 0, geometry.size_bytes, true, 0,
                 DeviceState::kOpen);
  return dev;
}

std::unique_ptr<WindowsRawBlockDevice> WindowsRawBlockDevice::Open(
    const std::wstring& path, bool writable, bool exclusive,
    std::string* error) {
  DeviceOpenPolicy policy;
  policy.writable = writable;
  policy.exclusive = exclusive;
  policy.require_volume_ownership = writable;
  policy.allow_virtual_disk = true;
  policy.allow_removable_disk = false;
  policy.allow_fixed_disk = false;
  policy.cache_mode = writable ? DeviceCacheMode::kNoBuffering
                               : DeviceCacheMode::kBuffered;
  return Open(path, policy, error);
}

std::unique_ptr<BlockDevice> OpenRawDevice(const std::wstring& path,
                                           const DeviceOpenPolicy& policy,
                                           std::string* error) {
  return WindowsRawBlockDevice::Open(path, policy, error);
}

std::unique_ptr<BlockDevice> OpenRawDevice(const std::wstring& path,
                                           bool writable, bool exclusive,
                                           std::string* error) {
  return WindowsRawBlockDevice::Open(path, writable, exclusive, error);
}

// ---------------------------------------------------------------------------
// Volume ownership
// ---------------------------------------------------------------------------
bool WindowsRawBlockDevice::LockVolumes(std::string* error) {
  if (!impl_) {
    if (error) *error = "device not open";
    return false;
  }
  std::unique_lock<std::shared_mutex> lifecycle(lifecycle_mutex_);
  std::lock_guard<std::mutex> ownership(impl_->ownership_mutex);
  if (impl_->volumes_locked) return true;
  if (impl_->handle == INVALID_HANDLE_VALUE) {
    if (error) *error = "device not open";
    return false;
  }

  STORAGE_DEVICE_NUMBER sdn{};
  if (!QueryDeviceNumber(impl_->handle, &sdn)) {
    if (error) {
      *error = "cannot query device number" +
               FormatWinError(GetLastError());
    }
    return false;
  }
  if (sdn.DeviceType == FILE_DEVICE_DISK_FILE_SYSTEM) {
    if (error) {
      *error = "volume ownership must be acquired from the whole-disk handle";
    }
    return false;
  }

  WCHAR volume_name[MAX_PATH];
  HANDLE find = FindFirstVolumeW(volume_name, MAX_PATH);
  if (find == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    // FindFirstVolumeW reports an empty volume list as "no more files" on
    // current Windows; accept both end-of-enumeration codes.  Any other
    // failure means volumes we cannot see could still be mounted.
    if (code == ERROR_NO_MORE_FILES || code == ERROR_NO_MORE_ITEMS) {
      impl_->volumes_locked = true;
      TraceOperation(impl_.get(), "lock_volumes", 0, 0, true, 0,
                     state_.load());
      return true;
    }
    if (error) {
      *error = "cannot enumerate volumes" + FormatWinError(code);
    }
    return false;
  }

  bool ok = true;
  do {
    size_t len = wcslen(volume_name);
    if (len && volume_name[len - 1] == L'\\') {
      volume_name[len - 1] = L'\0';
    }

    HANDLE volume = CreateFileW(volume_name, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    bool read_only_handle = false;
    if (volume == INVALID_HANDLE_VALUE) {
      // A volume on another disk may be inaccessible; only fail ownership if
      // we can still prove it belongs to this disk.  Try a read-only handle to
      // query the extents.
      volume = CreateFileW(volume_name, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
      read_only_handle = volume != INVALID_HANDLE_VALUE;
    }
    if (volume == INVALID_HANDLE_VALUE) {
      // Cannot open the volume at all.  We cannot prove it is not ours, so
      // refuse ownership instead of writing behind a mounted volume.
      ok = false;
      if (error) {
        *error = "cannot open volume " + WideToUtf8(volume_name) +
                 FormatWinError(GetLastError());
      }
      break;
    }

    const int relation = VolumeDiskRelation(volume, sdn.DeviceNumber);
    if (relation < 0) {
      ok = false;
      if (error) {
        *error = "cannot determine which disk volume " +
                 WideToUtf8(volume_name) + " belongs to";
      }
      CloseHandle(volume);
      break;
    }
    if (relation == 0) {
      CloseHandle(volume);
      continue;
    }
    if (read_only_handle) {
      ok = false;
      if (error) {
        *error = "cannot open volume " + WideToUtf8(volume_name) +
                 " for exclusive ownership (access denied)";
      }
      CloseHandle(volume);
      break;
    }

    DWORD returned = 0;
    if (!DeviceIoControl(volume, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                         &returned, nullptr)) {
      ok = false;
      if (error) {
        *error = "cannot lock volume " + WideToUtf8(volume_name) +
                 FormatWinError(GetLastError());
      }
      CloseHandle(volume);
      break;
    }
    if (!DeviceIoControl(volume, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0,
                         &returned, nullptr)) {
      ok = false;
      if (error) {
        *error = "cannot dismount volume " + WideToUtf8(volume_name) +
                 FormatWinError(GetLastError());
      }
      DeviceIoControl(volume, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0,
                      &returned, nullptr);
      CloseHandle(volume);
      break;
    }
    // After a successful dismount the volume must report "not mounted".  A
    // mounted volume here means Windows remounted it between the two calls.
    // FSCTL_IS_VOLUME_MOUNTED answers FALSE with ERROR_NOT_READY when the
    // volume is not mounted; any other failure is unknown and must refuse
    // ownership rather than be read as "dismounted".
    if (DeviceIoControl(volume, FSCTL_IS_VOLUME_MOUNTED, nullptr, 0, nullptr, 0,
                        &returned, nullptr)) {
      ok = false;
      if (error) {
        *error = "volume " + WideToUtf8(volume_name) +
                 " is still mounted after dismount";
      }
      DeviceIoControl(volume, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0,
                      &returned, nullptr);
      CloseHandle(volume);
      break;
    }
    const DWORD mounted_error = GetLastError();
    if (mounted_error != ERROR_NOT_READY) {
      ok = false;
      if (error) {
        *error = "cannot verify that volume " + WideToUtf8(volume_name) +
                 " was dismounted" + FormatWinError(mounted_error);
      }
      DeviceIoControl(volume, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0,
                      &returned, nullptr);
      CloseHandle(volume);
      break;
    }
    impl_->locked_volumes.push_back(volume);
  } while (FindNextVolumeW(find, volume_name, MAX_PATH));
  const DWORD enumerate_error = GetLastError();
  FindVolumeClose(find);
  // FindNextVolumeW ends normally with ERROR_NO_MORE_FILES.  (Some Windows
  // versions have reported ERROR_NO_MORE_ITEMS here; accept both.)  Any other
  // error means the enumeration stopped early: volumes we never saw could
  // still be mounted, so ownership is not proven.
  if (ok && enumerate_error != ERROR_NO_MORE_FILES &&
      enumerate_error != ERROR_NO_MORE_ITEMS) {
    ok = false;
    if (error) {
      *error = "cannot enumerate volumes" + FormatWinError(enumerate_error);
    }
  }

  if (!ok) {
    ReleaseLockedVolumes(impl_.get());
    return false;
  }
  impl_->volumes_locked = true;
  TraceOperation(impl_.get(), "lock_volumes", 0, 0, true, 0, state_.load());
  return true;
}

// ---------------------------------------------------------------------------
// Low-level I/O
// ---------------------------------------------------------------------------
bool WindowsRawBlockDevice::RawTransferChunk(bool write, uint64_t offset,
                                             void* buffer, size_t size,
                                             size_t* transferred,
                                             std::string* error) {
  *transferred = 0;
  if (size == 0) return true;
  if (size > kMaxTransferChunk) {
    if (error) *error = "raw transfer chunk is too large";
    return false;
  }

  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffu);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  HANDLE event = TransferEvent();
  if (event != INVALID_HANDLE_VALUE) {
    ResetEvent(event);
    overlapped.hEvent = event;
  }

  DWORD got = 0;
  BOOL ok;
  if (write) {
    ok = WriteFile(impl_->handle, buffer, static_cast<DWORD>(size), &got,
                   &overlapped);
  } else {
    ok = ReadFile(impl_->handle, buffer, static_cast<DWORD>(size), &got,
                  &overlapped);
  }
  if (!ok) {
    DWORD code = GetLastError();
    if (code == ERROR_IO_PENDING) {
      if (!GetOverlappedResult(impl_->handle, &overlapped, &got, TRUE)) {
        code = GetLastError();
        if (error) {
          *error = std::string(write ? "raw write failed" : "raw read failed") +
                   FormatWinError(code);
        }
        last_error_kind_.store(ClassifyDeviceError(code));
        if (IsTerminalDeviceError(code)) {
          Invalidate(ClassifyDeviceError(code),
                     error ? *error : std::string("terminal device error"));
        }
        return false;
      }
    } else {
      if (error) {
        *error = std::string(write ? "raw write failed" : "raw read failed") +
                 FormatWinError(code);
      }
      last_error_kind_.store(ClassifyDeviceError(code));
      if (IsTerminalDeviceError(code)) {
        Invalidate(ClassifyDeviceError(code),
                   error ? *error : std::string("terminal device error"));
      }
      return false;
    }
  }
  if (got == 0) {
    if (error) {
      *error = std::string(write ? "raw write made no progress"
                                 : "raw read made no progress");
    }
    return false;
  }
  *transferred = got;
  return true;
}

bool WindowsRawBlockDevice::RawTransfer(bool write, uint64_t offset,
                                        void* buffer, size_t size,
                                        bool short_transfer,
                                        std::string* error) {
  const uint32_t sector = info_.logical_sector_size;
  if (size == 0) return true;

  std::string reason;
  if (!ValidateDeviceRequest(info_.size_bytes, offset, size, &reason)) {
    if (error) *error = reason;
    return false;
  }

  const bool unbuffered = info_.cache_mode == DeviceCacheMode::kNoBuffering ||
                          info_.cache_mode ==
                              DeviceCacheMode::kNoBufferingWriteThrough;
  // Windows requires unbuffered offsets and lengths to be multiples of the
  // logical sector size, and recommends (some devices require) buffer
  // addresses aligned to the physical sector size.  Use the physical size for
  // the bounce buffer address and for any continuation after a short
  // transfer; 4096 is a safe floor for devices that did not report it.
  const size_t buffer_align =
      unbuffered
          ? std::max<size_t>({static_cast<size_t>(sector),
                              static_cast<size_t>(info_.physical_sector_size),
                              static_cast<size_t>(4096)})
          : static_cast<size_t>(sector);
  const size_t short_unit = unbuffered ? buffer_align : sector;

  // Fault injection: make the first transfer of this logical operation short.
  // The normal loop then has to finish the request, which is exactly the
  // partial-I/O path a real device can exercise.
  const bool short_first = short_transfer && size > short_unit;
  auto maybe_shorten = [&](size_t want, bool first) -> size_t {
    if (!first || !short_first) return want;
    size_t half = (want / 2 / short_unit) * short_unit;
    if (half < short_unit) half = short_unit;
    if (half >= want) return want;  // cannot split a single unit
    return half;
  };
  // A short transfer that does not complete the request leaves the next
  // request at an advanced offset and buffer address; both must stay aligned
  // for unbuffered I/O.  Anything else is a device error, not something to
  // paper over.
  auto usable_short = [&](size_t got, size_t remaining) {
    return got == remaining || got % short_unit == 0;
  };

  if (!unbuffered) {
    // Buffered handles accept arbitrary offsets, lengths and buffer addresses.
    // The backend still chunks the transfer so no request exceeds the 32-bit
    // Windows transfer size.
    uint64_t pos = offset;
    uint8_t* out = static_cast<uint8_t*>(buffer);
    size_t remaining = size;
    bool first = true;
    while (remaining > 0) {
      const size_t want =
          maybe_shorten(std::min(remaining, kMaxTransferChunk), first);
      size_t got = 0;
      if (!RawTransferChunk(write, pos, out, want, &got, error)) return false;
      pos += got;
      out += got;
      remaining -= got;
      first = false;
    }
    return true;
  }

  // Unbuffered I/O: offsets and lengths must be logical-sector aligned and
  // buffer addresses should be physical-sector aligned (some devices enforce
  // it).  The alignment offset is required to be zero for writable whole-disk
  // handles; read-only handles with a non-zero offset use buffered mode.
  if (info_.alignment_offset != 0) {
    if (error) {
      *error = "unbuffered I/O requested on a device with a non-zero "
               "alignment offset";
    }
    return false;
  }
  const uint64_t request_end = offset + size;
  const uint64_t aligned_start = AlignDownDevice(offset, sector);
  uint64_t aligned_end = 0;
  if (!AlignUpDeviceChecked(request_end, sector, &aligned_end)) {
    if (error) *error = "request alignment overflow";
    return false;
  }
  if (aligned_end > info_.size_bytes) {
    if (error) *error = "aligned request exceeds the device size";
    return false;
  }

  const bool direct =
      aligned_start == offset && aligned_end == request_end &&
      IsDirectDeviceRequest(offset, size, buffer,
                            static_cast<uint32_t>(buffer_align));
  if (direct) {
    uint64_t pos = offset;
    uint8_t* out = static_cast<uint8_t*>(buffer);
    size_t remaining = size;
    bool first = true;
    while (remaining > 0) {
      const size_t want =
          maybe_shorten(std::min(remaining, kMaxTransferChunk), first);
      size_t got = 0;
      if (!RawTransferChunk(write, pos, out, want, &got, error)) return false;
      if (!usable_short(got, remaining)) {
        if (error) *error = "device returned an unaligned short transfer";
        return false;
      }
      pos += got;
      out += got;
      remaining -= got;
      first = false;
    }
    return true;
  }

  // Bounce path.  Process aligned chunks of at most kMaxTransferChunk bytes.
  const size_t align = buffer_align;
  uint64_t chunk_start = aligned_start;
  bool first_chunk = true;
  while (chunk_start < aligned_end) {
    size_t chunk_len = static_cast<size_t>(
        std::min<uint64_t>(aligned_end - chunk_start, kMaxTransferChunk));
    chunk_len = (chunk_len / sector) * sector;
    chunk_len = maybe_shorten(chunk_len, first_chunk);
    if (chunk_len == 0) {
      if (error) *error = "internal alignment chunk is empty";
      return false;
    }
    std::vector<uint8_t> storage(chunk_len + align + sector);
    const uintptr_t base = reinterpret_cast<uintptr_t>(storage.data());
    uint8_t* aligned_buffer = reinterpret_cast<uint8_t*>(
        (base + align - 1) & ~(static_cast<uintptr_t>(align) - 1));

    const uint64_t chunk_end = chunk_start + chunk_len;
    const uint64_t copy_start = std::max<uint64_t>(chunk_start, offset);
    const uint64_t copy_end = std::min<uint64_t>(chunk_end, request_end);
    const bool fully_covered =
        offset <= chunk_start && request_end >= chunk_end;

    if (write) {
      if (!fully_covered) {
        // Read-modify-write: fill the aligned chunk, then overwrite the
        // caller's bytes.  A short read is not acceptable here, so loop until
        // the whole chunk is in the bounce buffer.
        size_t read_total = 0;
        while (read_total < chunk_len) {
          size_t got = 0;
          if (!RawTransferChunk(false, chunk_start + read_total,
                                aligned_buffer + read_total,
                                chunk_len - read_total, &got, error)) {
            return false;
          }
          if (!usable_short(got, chunk_len - read_total)) {
            if (error) *error = "device returned an unaligned short transfer";
            return false;
          }
          read_total += got;
        }
      }
      std::memcpy(aligned_buffer + (copy_start - chunk_start),
                  static_cast<const uint8_t*>(buffer) +
                      (copy_start - offset),
                  static_cast<size_t>(copy_end - copy_start));
      uint64_t write_pos = chunk_start;
      size_t remaining = chunk_len;
      uint8_t* write_buffer = aligned_buffer;
      while (remaining > 0) {
        size_t got = 0;
        if (!RawTransferChunk(true, write_pos, write_buffer, remaining, &got,
                              error)) {
          return false;
        }
        if (!usable_short(got, remaining)) {
          if (error) *error = "device returned an unaligned short transfer";
          return false;
        }
        write_pos += got;
        write_buffer += got;
        remaining -= got;
      }
    } else {
      uint64_t read_pos = chunk_start;
      size_t remaining = chunk_len;
      uint8_t* read_buffer = aligned_buffer;
      while (remaining > 0) {
        size_t got = 0;
        if (!RawTransferChunk(false, read_pos, read_buffer, remaining, &got,
                              error)) {
          return false;
        }
        if (!usable_short(got, remaining)) {
          if (error) *error = "device returned an unaligned short transfer";
          return false;
        }
        read_pos += got;
        read_buffer += got;
        remaining -= got;
      }
      std::memcpy(static_cast<uint8_t*>(buffer) + (copy_start - offset),
                  aligned_buffer + (copy_start - chunk_start),
                  static_cast<size_t>(copy_end - copy_start));
    }
    chunk_start = chunk_end;
    first_chunk = false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Public I/O
// ---------------------------------------------------------------------------
bool WindowsRawBlockDevice::Read(uint64_t offset, void* buffer, size_t size) {
  if (size == 0) return true;
  if (!impl_ || state_.load() == DeviceState::kClosed ||
      state_.load() == DeviceState::kInvalid) {
    SetError("device is not open");
    TraceOperation(impl_.get(), "read", offset, size, false, 0, state_.load());
    return false;
  }

  DeviceFaultPlan faults;
  uint32_t call = 0;
  bool fail = false;
  bool invalidate = false;
  bool short_transfer = false;
  {
    std::lock_guard<std::mutex> lock(impl_->fault_mutex);
    faults = impl_->faults;
    call = ++impl_->read_calls;
    ++impl_->operation_count;
    if (faults.read_fail_at != 0 && call == faults.read_fail_at) fail = true;
    if (faults.read_short_at != 0 && call == faults.read_short_at) {
      short_transfer = true;
    }
    if (faults.invalidate_after != 0 &&
        impl_->operation_count > faults.invalidate_after) {
      fail = true;
      invalidate = true;
    }
  }
  if (fail) {
    if (invalidate) {
      Invalidate(DeviceErrorKind::kDeviceRemoved,
                 "injected device invalidation");
    } else {
      last_error_kind_.store(DeviceErrorKind::kOther);
      SetError("injected read failure");
    }
    TraceOperation(impl_.get(), "read", offset, size, false, 0, state_.load());
    return false;
  }

  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mutex_);
  if (state_.load() == DeviceState::kClosed ||
      state_.load() == DeviceState::kInvalid ||
      impl_->handle == INVALID_HANDLE_VALUE) {
    SetError("device is not open");
    TraceOperation(impl_.get(), "read", offset, size, false, 0, state_.load());
    return false;
  }

  std::string error;
  const bool ok =
      RawTransfer(false, offset, buffer, size, short_transfer, &error);
  if (!ok) {
    SetError(error.empty() ? "raw read failed" : error);
    TraceOperation(impl_.get(), "read", offset, size, false, 0, state_.load());
    return false;
  }
  TraceOperation(impl_.get(), "read", offset, size, true, 0, state_.load());
  return true;
}

bool WindowsRawBlockDevice::Write(uint64_t offset, const void* buffer,
                                  size_t size) {
  if (size == 0) return true;
  if (!writable_) {
    SetError("device opened read-only");
    TraceOperation(impl_.get(), "write", offset, size, false, 0, state_.load());
    return false;
  }
  if (!impl_ || state_.load() == DeviceState::kClosed ||
      state_.load() == DeviceState::kInvalid) {
    SetError("device is not open");
    TraceOperation(impl_.get(), "write", offset, size, false, 0, state_.load());
    return false;
  }

  DeviceFaultPlan faults;
  uint32_t call = 0;
  bool fail = false;
  bool invalidate = false;
  bool short_transfer = false;
  {
    std::lock_guard<std::mutex> lock(impl_->fault_mutex);
    faults = impl_->faults;
    call = ++impl_->write_calls;
    ++impl_->operation_count;
    if (faults.write_fail_at != 0 && call == faults.write_fail_at) fail = true;
    if (faults.write_short_at != 0 && call == faults.write_short_at) {
      short_transfer = true;
    }
    if (faults.invalidate_after != 0 &&
        impl_->operation_count > faults.invalidate_after) {
      fail = true;
      invalidate = true;
    }
  }
  if (fail) {
    if (invalidate) {
      Invalidate(DeviceErrorKind::kDeviceRemoved,
                 "injected device invalidation");
    } else {
      last_error_kind_.store(DeviceErrorKind::kOther);
      SetError("injected write failure");
    }
    TraceOperation(impl_.get(), "write", offset, size, false, 0, state_.load());
    return false;
  }

  // Lock order: lifecycle_mutex_ -> flush_barrier_.  Writers take the barrier
  // exclusively: an unaligned write is a read-modify-write of the enclosing
  // sector block, so two concurrent writers could silently lose each other's
  // bytes.  Exclusive use also keeps new Flush calls from overtaking a write.
  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mutex_);
  std::unique_lock<std::shared_mutex> barrier(flush_barrier_);
  if (state_.load() == DeviceState::kClosed ||
      state_.load() == DeviceState::kInvalid ||
      impl_->handle == INVALID_HANDLE_VALUE) {
    SetError("device is not open");
    TraceOperation(impl_.get(), "write", offset, size, false, 0, state_.load());
    return false;
  }

  std::string error;
  const bool ok = RawTransfer(true, offset, const_cast<void*>(buffer), size,
                              short_transfer, &error);
  if (!ok) {
    SetError(error.empty() ? "raw write failed" : error);
    TraceOperation(impl_.get(), "write", offset, size, false, 0,
                   state_.load());
    return false;
  }
  TraceOperation(impl_.get(), "write", offset, size, true, 0, state_.load());
  return true;
}

bool WindowsRawBlockDevice::Flush() {
  if (!impl_ || state_.load() == DeviceState::kClosed ||
      state_.load() == DeviceState::kInvalid) {
    SetError("device is not open");
    TraceOperation(impl_.get(), "flush", 0, 0, false, 0, state_.load());
    return false;
  }

  DeviceFaultPlan faults;
  uint32_t call = 0;
  bool fail = false;
  bool invalidate = false;
  {
    std::lock_guard<std::mutex> lock(impl_->fault_mutex);
    faults = impl_->faults;
    call = ++impl_->flush_calls;
    ++impl_->operation_count;
    if (faults.flush_fail_at != 0 && call == faults.flush_fail_at) fail = true;
    if (faults.invalidate_after != 0 &&
        impl_->operation_count > faults.invalidate_after) {
      fail = true;
      invalidate = true;
    }
  }
  if (fail) {
    if (invalidate) {
      Invalidate(DeviceErrorKind::kDeviceRemoved,
                 "injected device invalidation");
    } else {
      last_error_kind_.store(DeviceErrorKind::kOther);
      SetError("injected flush failure");
    }
    TraceOperation(impl_.get(), "flush", 0, 0, false, 0, state_.load());
    return false;
  }

  std::shared_lock<std::shared_mutex> lifecycle(lifecycle_mutex_);
  std::unique_lock<std::shared_mutex> barrier(flush_barrier_);
  if (state_.load() == DeviceState::kClosed ||
      state_.load() == DeviceState::kInvalid ||
      impl_->handle == INVALID_HANDLE_VALUE) {
    SetError("device is not open");
    TraceOperation(impl_.get(), "flush", 0, 0, false, 0, state_.load());
    return false;
  }

  if (!writable_) {
    // Nothing can be dirty on a read-only handle; FlushFileBuffers would fail
    // with ACCESS_DENIED on some raw handles.
    TraceOperation(impl_.get(), "flush", 0, 0, true, 0, state_.load());
    return true;
  }

  state_.store(DeviceState::kFlushing);
  // FlushFileBuffers asks the Windows storage stack to push cached data to the
  // device.  It does not prove that a device-side volatile write cache was
  // flushed or that the media survives power loss; the backend reports exactly
  // the API result (see docs/WINDOWS_BLOCK_DEVICE.md).
  const BOOL ok = FlushFileBuffers(impl_->handle);
  const DWORD code = ok ? 0 : GetLastError();
  if (!ok) {
    const DeviceErrorKind kind = ClassifyDeviceError(code);
    last_error_kind_.store(kind);
    SetError("FlushFileBuffers failed" + FormatWinError(code));
    if (IsTerminalDeviceError(code)) {
      Invalidate(kind, "FlushFileBuffers failed" + FormatWinError(code));
    } else {
      state_.store(DeviceState::kOpen);
    }
    TraceOperation(impl_.get(), "flush", 0, 0, false, code, state_.load());
    return false;
  }
  state_.store(DeviceState::kOpen);
  TraceOperation(impl_.get(), "flush", 0, 0, true, 0, state_.load());
  return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
DeviceState WindowsRawBlockDevice::State() const { return state_.load(); }

DeviceErrorKind WindowsRawBlockDevice::LastErrorKind() const {
  return last_error_kind_.load();
}

void WindowsRawBlockDevice::Invalidate(DeviceErrorKind kind,
                                       std::string message) {
  last_error_kind_.store(kind);
  SetError(std::move(message));
  DeviceState expected = state_.load();
  while (expected != DeviceState::kClosed &&
         expected != DeviceState::kClosing &&
         expected != DeviceState::kInvalid) {
    if (state_.compare_exchange_weak(expected, DeviceState::kInvalid)) break;
  }
}

void WindowsRawBlockDevice::Close() {
  if (!impl_) {
    state_.store(DeviceState::kClosed);
    return;
  }
  std::unique_lock<std::shared_mutex> lifecycle(lifecycle_mutex_);
  if (state_.load() == DeviceState::kClosed) return;
  state_.store(DeviceState::kClosing);

  {
    std::lock_guard<std::mutex> ownership(impl_->ownership_mutex);
    ReleaseLockedVolumes(impl_.get());
  }
  if (impl_->handle != INVALID_HANDLE_VALUE) {
    CloseHandle(impl_->handle);
    impl_->handle = INVALID_HANDLE_VALUE;
  }
  if (impl_->trace != nullptr) {
    TraceOperation(impl_.get(), "close", 0, 0, true, 0, DeviceState::kClosed);
    std::lock_guard<std::mutex> lock(impl_->trace_mutex);
    std::fclose(impl_->trace);
    impl_->trace = nullptr;
  }
  state_.store(DeviceState::kClosed);
}

const DeviceIdentity& WindowsRawBlockDevice::Identity() const {
  static const DeviceIdentity kEmpty;
  return impl_ ? impl_->identity : kEmpty;
}

const DeviceOpenPolicy& WindowsRawBlockDevice::Policy() const {
  static const DeviceOpenPolicy kEmpty;
  return impl_ ? impl_->policy : kEmpty;
}

bool WindowsRawBlockDevice::VolumeOwnershipConfirmed() const {
  return impl_ && impl_->volumes_locked.load(std::memory_order_relaxed);
}

std::string WindowsRawBlockDevice::Describe() const {
  std::ostringstream out;
  out << "path: " << info_.description << "\n";
  out << "device_class: " << ToString(info_.device_class) << "\n";
  out << "bus_type: " << ToString(info_.bus_type) << "\n";
  out << "size_bytes: " << info_.size_bytes << "\n";
  out << "logical_sector_size: " << info_.logical_sector_size << "\n";
  out << "physical_sector_size: " << info_.physical_sector_size << "\n";
  out << "alignment_offset: " << info_.alignment_offset << "\n";
  out << "disk_number: " << info_.disk_number << "\n";
  out << "partition_number: " << info_.partition_number << "\n";
  out << "stable_id: " << info_.stable_id << "\n";
  out << "serial_number: " << info_.serial_number << "\n";
  out << "removable_media: " << (info_.removable_media ? "yes" : "no") << "\n";
  out << "offline: " << (info_.offline ? "yes" : "no") << "\n";
  out << "read_only_media: " << (info_.read_only_media ? "yes" : "no")
      << "\n";
  out << "system_disk: " << (info_.is_system_disk ? "yes" : "no") << "\n";
  out << "pagefile_disk: " << (info_.has_pagefile ? "yes" : "no") << "\n";
  out << "writable: " << (info_.writable ? "yes" : "no") << "\n";
  out << "ownership_confirmed: "
      << (info_.ownership_confirmed ? "yes" : "no") << "\n";
  out << "cache_mode: " << ToString(info_.cache_mode) << "\n";
  out << "state: " << ToString(state_.load()) << "\n";
  return out.str();
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------
std::vector<RawDiskInfo> EnumeratePhysicalDrives(std::string* error) {
  std::vector<RawDiskInfo> disks;
  const uint32_t system_disk = SystemDiskNumber();
  const std::set<uint32_t> pagefile_disks = PagefileDiskNumbers();

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
            *error = "cannot open PhysicalDrive0" + FormatWinError(ec);
          }
        }
        return disks;
      }
      break;
    }

    RawDiskInfo info;
    info.disk_number = number;
    info.device_path = WideToUtf8(path);
    // Unknown until a probe succeeds; never pretend a failed query means 512.
    info.logical_sector_size = 0;
    info.physical_sector_size = 0;

    DWORD bytes = 0;
    std::vector<uint8_t> geometry_buffer(sizeof(DISK_GEOMETRY_EX) + 1024);
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                        geometry_buffer.data(),
                        static_cast<DWORD>(geometry_buffer.size()), &bytes,
                        nullptr)) {
      const auto* geometry =
          reinterpret_cast<const DISK_GEOMETRY_EX*>(geometry_buffer.data());
      info.logical_sector_size = geometry->Geometry.BytesPerSector;
      info.size_bytes = static_cast<uint64_t>(geometry->DiskSize.QuadPart);
    }

    DWORD property_size = 0;
    std::string property = QueryDeviceProperty(
        h, StorageAccessAlignmentProperty, PropertyStandardQuery,
        &property_size);
    if (property.size() >= sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR)) {
      const auto* alignment =
          reinterpret_cast<const STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR*>(
              property.data());
      if (alignment->BytesPerLogicalSector != 0) {
        info.logical_sector_size = alignment->BytesPerLogicalSector;
      }
      info.physical_sector_size = alignment->BytesPerPhysicalSector;
      info.alignment_offset = alignment->BytesOffsetForSectorAlignment;
    }
    if (info.physical_sector_size == 0) {
      info.physical_sector_size = info.logical_sector_size;
    }

    property = QueryDeviceProperty(h, StorageDeviceProperty,
                                   PropertyStandardQuery, &property_size);
    DeviceBusType bus_type = DeviceBusType::kUnknown;
    if (property.size() >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
      const auto* desc =
          reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(property.data());
      bus_type = MapBusType(desc->BusType);
      if (desc->ProductIdOffset != 0 &&
          desc->ProductIdOffset < property.size()) {
        info.model = TrimSpaces(property.data() + desc->ProductIdOffset);
      }
      if (desc->SerialNumberOffset != 0 &&
          desc->SerialNumberOffset < property.size()) {
        info.serial_number =
            TrimSpaces(property.data() + desc->SerialNumberOffset);
      }
      info.is_removable = desc->RemovableMedia != 0;
    }
    info.bus_type = bus_type;
    info.device_class = ClassifyDisk(info.is_removable, bus_type);

#ifdef IOCTL_STORAGE_GET_DEVICE_NUMBER_EX
    STORAGE_DEVICE_NUMBER_EX sdnx{};
    if (QueryDeviceNumberEx(h, &sdnx) && !GuidIsZero(sdnx.DeviceGuid)) {
      info.stable_id = GuidToString(sdnx.DeviceGuid);
    }
#endif
    if (info.stable_id.empty()) {
      info.stable_id = info.serial_number.empty()
                           ? info.model + "/" + std::to_string(number)
                           : "serial:" + info.serial_number;
    }

    QueryDiskAttributes(h, &info.offline, &info.read_only);
    bool disk_writable = true;
    if (QueryIsWritable(h, &disk_writable) && !disk_writable) {
      info.read_only = true;
    }

    info.is_system_disk = system_disk != 0xffffffffu && number == system_disk;
    info.has_pagefile = pagefile_disks.count(number) != 0;

    // Partition table.
    std::vector<uint8_t> layout_buf(64 * 1024);
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
                        layout_buf.data(),
                        static_cast<DWORD>(layout_buf.size()), &bytes,
                        nullptr)) {
      const auto* layout =
          reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(
              layout_buf.data());
      const bool gpt = layout->PartitionStyle == PARTITION_STYLE_GPT;
      for (DWORD i = 0; i < layout->PartitionCount; ++i) {
        const PARTITION_INFORMATION_EX& part = layout->PartitionEntry[i];
        if (gpt) {
          if (GuidIsZero(part.Gpt.PartitionType)) continue;
        } else {
          if (part.Mbr.PartitionType == 0) continue;
        }
        RawPartitionInfo pi;
        pi.number = i + 1;
        pi.offset_bytes = static_cast<uint64_t>(part.StartingOffset.QuadPart);
        pi.length_bytes = static_cast<uint64_t>(part.PartitionLength.QuadPart);
        pi.bootable = part.Mbr.BootIndicator != 0;
        pi.gpt = gpt;
        if (gpt) {
          pi.type = GuidToString(part.Gpt.PartitionType);
        } else {
          char buf[16];
          std::snprintf(buf, sizeof(buf), "0x%02X", part.Mbr.PartitionType);
          pi.type = buf;
        }
        info.partitions.push_back(pi);
      }
    }

    disks.push_back(std::move(info));
    CloseHandle(h);
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

std::unique_ptr<BlockDevice> OpenRawDevice(const std::wstring&,
                                           const DeviceOpenPolicy&,
                                           std::string* error) {
  if (error) *error = "raw devices are only supported on Windows";
  return nullptr;
}

std::unique_ptr<BlockDevice> OpenRawDevice(const std::wstring&, bool, bool,
                                           std::string* error) {
  if (error) *error = "raw devices are only supported on Windows";
  return nullptr;
}

}  // namespace xfs

#endif  // _WIN32
