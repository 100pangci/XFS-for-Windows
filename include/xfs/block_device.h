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
#include <string>

namespace xfs {

struct DeviceInfo {
  uint64_t size_bytes = 0;
  uint32_t logical_sector_size = 512;
  uint32_t physical_sector_size = 512;
  bool writable = false;
  bool is_raw_device = false;
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

  // Flushes pending writes to stable storage.
  virtual bool Flush() = 0;

  // Total device size in bytes.
  virtual uint64_t Size() const = 0;

  // Logical sector size in bytes.
  virtual uint32_t SectorSize() const = 0;

  virtual const DeviceInfo& Info() const = 0;

  // Releases the underlying handle. Safe to call multiple times.
  virtual void Close() = 0;

  // Last backend error, for diagnostics.
  const std::string& LastError() const { return last_error_; }

 protected:
  mutable std::string last_error_;
};

}  // namespace xfs
