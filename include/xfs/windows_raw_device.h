// XFS for Windows - raw Windows block device backend.
//
// Provides read-only access to \\.\PhysicalDriveN and \\.\HarddiskVolumeX,
// plus the destructive-write path used by the formatter (exclusive access,
// volume locking/dismount, sector-aligned unbuffered I/O with bounce
// buffers).
//
// On non-Windows builds only the declarations exist and all operations fail,
// so the core stays portable for tests.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xfs/block_device.h"

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
  std::string model;
  std::string device_path;
  bool is_system_disk = false;
  bool has_pagefile = false;
  bool is_removable = false;
  std::vector<RawPartitionInfo> partitions;
};

// Enumerates \\.\PhysicalDrive0..N with partition tables and safety flags.
std::vector<RawDiskInfo> EnumeratePhysicalDrives(std::string* error);

// Opens a raw device path (\\.\PhysicalDriveN, \\.\HarddiskVolumeX, ...).
// `exclusive` requests exclusive shared-mode access (needed before writing).
std::unique_ptr<BlockDevice> OpenRawDevice(const std::wstring& path,
                                           bool writable, bool exclusive,
                                           std::string* error);

class WindowsRawBlockDevice final : public BlockDevice {
 public:
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
  void Close() override;

  // Locks and dismounts all volumes residing on this device (the disks that
  // carry mounted filesystems). Must be called before writing. Idempotent.
  bool LockVolumes(std::string* error);

 private:
  WindowsRawBlockDevice() = default;

  bool RawReadAligned(uint64_t aligned_offset, void* buffer, size_t size);
  bool RawWriteAligned(uint64_t aligned_offset, const void* buffer,
                       size_t size);
  bool ReadExact(uint64_t offset, void* buffer, size_t size);
  bool WriteExact(uint64_t offset, const void* buffer, size_t size);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  DeviceInfo info_;
  bool writable_ = false;
  bool exclusive_ = false;
};

}  // namespace xfs
