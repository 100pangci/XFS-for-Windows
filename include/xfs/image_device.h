// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - image file block device (*.img / *.raw).
//
// I/O semantics:
//   - Read/Write are positioned I/O (FILE_FLAG_OVERLAPPED on Windows,
//     pread/pwrite on POSIX): independent ranges can be in flight at once.
//   - Write never tears a caller's buffer and never silently short-writes;
//     a short transfer is retried and then reported as a failure.
//   - Flush is a durability barrier: it waits for earlier writes, excludes
//     new writes during the durable flush, and reports failure instead of
//     pretending success.
#pragma once

#include <filesystem>
#include <memory>
#include <shared_mutex>

#include "xfs/block_device.h"
#include "xfs/concurrency_hooks.h"

namespace xfs {

class ImageBlockDevice final : public BlockDevice {
 public:
  // Opens an existing image. writable=false opens read-only. `error` (optional)
  // receives the backend message on failure.
  static std::unique_ptr<ImageBlockDevice> Open(const std::filesystem::path& path,
                                                bool writable,
                                                uint32_t sector_size = 512,
                                                std::string* error = nullptr);

  // Creates (or overwrites) an image with the given size. The file is
  // allocated sparsely where the filesystem supports it.
  static std::unique_ptr<ImageBlockDevice> Create(const std::filesystem::path& path,
                                                  uint64_t size_bytes,
                                                  uint32_t sector_size = 512,
                                                  std::string* error = nullptr);

  ~ImageBlockDevice() override;

  bool Read(uint64_t offset, void* buffer, size_t size) override;
  bool Write(uint64_t offset, const void* buffer, size_t size) override;
  bool Flush() override;
  uint64_t Size() const override { return info_.size_bytes; }
  uint32_t SectorSize() const override { return info_.logical_sector_size; }
  const DeviceInfo& Info() const override { return info_; }
  void Close() override;

 private:
  ImageBlockDevice();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  // Positioned I/O runs concurrently: Read and Write take the lifecycle lock
  // shared and the flush barrier shared, so independent ranges do not serialize
  // behind each other. Flush takes the barrier exclusively (a durability
  // barrier waits for every earlier write), Close takes the lifecycle lock
  // exclusively (no I/O may outlive the handle).
  mutable conc::SharedMutex lifecycle_mutex_;
  mutable conc::SharedMutex flush_barrier_;
  DeviceInfo info_;
  std::filesystem::path path_;
};

}  // namespace xfs
