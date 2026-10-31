// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - raw Windows block device backend.
//
// DEV-1 gives this backend an explicit device model:
//
//   - it probes identity, bus type and sector geometry with DeviceIoControl
//     and rejects nonsense instead of falling back to 512 bytes;
//   - a writable open requires exclusive share mode plus volume ownership
//     (every volume on the disk is locked and dismounted);
//   - the device class (image/virtual/removable/fixed/volume) and the write
//     policy decide whether a write is allowed at all;
//   - every request is bounds-checked, chunked below the 32-bit transfer
//     limit, and sector-aligned with a bounce buffer when necessary;
//   - a removed device is a terminal lifecycle state; the backend never
//     reconnects by path or disk number;
//   - Close waits for in-flight I/O and Flush, and is idempotent.
//
// On non-Windows builds only the declarations exist and all operations fail,
// so the core stays portable for tests.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/device_policy.h"

namespace xfs {

struct RawPartitionInfo {
  uint32_t number = 0;
  uint64_t offset_bytes = 0;
  uint64_t length_bytes = 0;
  bool bootable = false;
  bool gpt = false;
  std::string type;  // GUID or MBR type string
};

struct RawDiskInfo {
  uint32_t disk_number = 0;
  uint64_t size_bytes = 0;
  uint32_t logical_sector_size = 512;
  uint32_t physical_sector_size = 512;
  uint32_t alignment_offset = 0;
  std::string model;
  std::string serial_number;
  std::string stable_id;
  std::string device_path;
  DeviceClass device_class = DeviceClass::kUnknown;
  DeviceBusType bus_type = DeviceBusType::kUnknown;
  bool is_system_disk = false;
  bool has_pagefile = false;
  bool is_removable = false;
  bool offline = false;
  bool read_only = false;
  std::vector<RawPartitionInfo> partitions;
};

// Enumerates \\.\PhysicalDrive0..N with geometry, identity and safety flags.
std::vector<RawDiskInfo> EnumeratePhysicalDrives(std::string* error);

// Opens a raw device path (\\.\PhysicalDriveN, \\.\HarddiskVolumeX, ...) under
// an explicit policy.  A writable open probes the device first, validates its
// geometry, applies the policy, and acquires volume ownership before it
// returns success.  On refusal the handle is closed and `error` names the
// policy reason.
std::unique_ptr<BlockDevice> OpenRawDevice(const std::wstring& path,
                                           const DeviceOpenPolicy& policy,
                                           std::string* error);

// Backward-compatible overload.  `writable` uses the conservative qualification
// policy (virtual disks only; fixed and removable disks are refused).  Callers
// that need another class must use the policy overload.
std::unique_ptr<BlockDevice> OpenRawDevice(const std::wstring& path,
                                           bool writable, bool exclusive,
                                           std::string* error);

class WindowsRawBlockDevice final : public BlockDevice {
 public:
  static std::unique_ptr<WindowsRawBlockDevice> Open(
      const std::wstring& path, const DeviceOpenPolicy& policy,
      std::string* error);

  static std::unique_ptr<WindowsRawBlockDevice> Open(const std::wstring& path,
                                                     bool writable,
                                                     bool exclusive,
                                                     std::string* error);

  ~WindowsRawBlockDevice() override;
  WindowsRawBlockDevice(const WindowsRawBlockDevice&) = delete;
  WindowsRawBlockDevice& operator=(const WindowsRawBlockDevice&) = delete;

  bool Read(uint64_t offset, void* buffer, size_t size) override;
  bool Write(uint64_t offset, const void* buffer, size_t size) override;
  bool Flush() override;
  uint64_t Size() const override { return info_.size_bytes; }
  uint32_t SectorSize() const override { return info_.logical_sector_size; }
  const DeviceInfo& Info() const override { return info_; }
  DeviceState State() const override;
  DeviceErrorKind LastErrorKind() const override;
  void Close() override;

  // Locks and dismounts all volumes residing on this device.  A writable Open
  // calls this automatically; the method stays public for qualification tools
  // and is idempotent.  Must succeed before the first write.
  bool LockVolumes(std::string* error);

  const DeviceIdentity& Identity() const;
  const DeviceOpenPolicy& Policy() const;
  bool VolumeOwnershipConfirmed() const;

  // Human-readable geometry/identity/policy report (one line per field).
  std::string Describe() const;

  // Public only so the backend's file-local trace/ownership helpers can name
  // the type; the definition stays in windows_raw_device.cpp.
  struct Impl;

 private:
  WindowsRawBlockDevice() = default;

  // I/O concurrency: reads execute concurrently (positioned OVERLAPPED I/O),
  // writes are exclusive.  Flush is a durability barrier.  Close drains both.
  // Lock order: lifecycle_mutex_ -> flush_barrier_.  No filesystem lock is
  // ever acquired while either is held.
  mutable std::shared_mutex lifecycle_mutex_;
  mutable std::shared_mutex flush_barrier_;

  bool RawTransfer(bool write, uint64_t offset, void* buffer, size_t size,
                   bool short_transfer, std::string* error);
  bool RawTransferChunk(bool write, uint64_t offset, void* buffer, size_t size,
                        size_t* transferred, std::string* error);

  // Marks the handle terminal and records the classified error.  Only touches
  // atomics and the last-error string, so it is safe to call while the
  // lifecycle lock is held (the I/O paths do).
  void Invalidate(DeviceErrorKind kind, std::string message);

  std::unique_ptr<Impl> impl_;
  DeviceInfo info_;
  bool writable_ = false;
  bool exclusive_ = false;
  std::atomic<DeviceState> state_{DeviceState::kClosed};
  std::atomic<DeviceErrorKind> last_error_kind_{DeviceErrorKind::kNone};
};

}  // namespace xfs
