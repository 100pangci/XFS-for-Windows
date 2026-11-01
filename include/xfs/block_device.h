// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - block device abstraction.
//
// Everything below the XFS core talks to storage through this interface.
// Implementations:
//   - ImageBlockDevice       : *.img / *.raw files (tests, formatter targets)
//   - WindowsRawBlockDevice  : \\.\PhysicalDriveN, \\.\HarddiskVolumeX
//   - MemoryBlockDevice      : in-memory (corruption tests)
//
// Read/Write accept arbitrary byte offsets and sizes. Backends that require
// sector alignment (e.g. unbuffered raw device I/O) handle alignment
// internally with bounce buffers.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include "xfs/device_policy.h"

namespace xfs {

struct DeviceInfo {
  uint64_t size_bytes = 0;
  uint32_t logical_sector_size = 512;
  uint32_t physical_sector_size = 512;
  uint32_t alignment_offset = 0;
  bool writable = false;
  bool is_raw_device = false;
  DeviceClass device_class = DeviceClass::kUnknown;
  DeviceBusType bus_type = DeviceBusType::kUnknown;
  DeviceCacheMode cache_mode = DeviceCacheMode::kBuffered;
  bool removable_media = false;
  bool offline = false;
  bool read_only_media = false;
  bool ownership_confirmed = false;
  bool is_system_disk = false;
  bool has_pagefile = false;
  uint32_t disk_number = 0;
  uint32_t partition_number = 0;
  std::string stable_id;      // DeviceGuid or serial/model fallback
  std::string serial_number;
  std::string description;  // path, model, or other human readable identity
};

class BlockDevice {
 public:
  virtual ~BlockDevice() = default;

  // Reads exactly `size` bytes at `offset`. Returns false on any I/O error,
  // short read, or out-of-bounds request.
  virtual bool Read(uint64_t offset, void* buffer, size_t size) = 0;

  // Writes exactly `size` bytes at `offset`. Returns false on error or if the
  // device was opened read-only.
  virtual bool Write(uint64_t offset, const void* buffer, size_t size) = 0;

  // Asks the backend to push pending writes to the device (a durability
  // barrier). Success means the platform accepted the barrier request; it
  // does not by itself prove that the device's own volatile cache survives a
  // power loss. Backends document exactly what they can and cannot prove
  // (see docs/WINDOWS_BLOCK_DEVICE.md).
  virtual bool Flush() = 0;

  // Total device size in bytes.
  virtual uint64_t Size() const = 0;

  // Logical sector size in bytes.
  virtual uint32_t SectorSize() const = 0;

  virtual const DeviceInfo& Info() const = 0;

  // Explicit lifecycle state. A backend whose device disappeared reports
  // kInvalid and must fail every later operation instead of reconnecting to a
  // device that happens to reuse the same path or disk number.
  virtual DeviceState State() const { return DeviceState::kOpen; }

  // Classification of the most recent failure. A terminal kind means the
  // handle must not be used again.
  virtual DeviceErrorKind LastErrorKind() const {
    return DeviceErrorKind::kNone;
  }

  // Releases the underlying handle. Safe to call multiple times. Waits for
  // in-flight I/O and Flush before releasing the handle.
  virtual void Close() = 0;

  // Last backend error, for diagnostics. The value is copied under a lock:
  // positioned I/O may fail on several threads at once, and callers must never
  // observe a torn std::string. Diagnostics are inherently "last error wins";
  // successful I/O does not clear it.
  std::string LastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
  }

 protected:
  void SetError(std::string message) const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = std::move(message);
  }

  mutable std::mutex error_mutex_;
  mutable std::string last_error_;
};

}  // namespace xfs
