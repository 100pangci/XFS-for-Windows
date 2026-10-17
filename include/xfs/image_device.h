// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - image file block device (*.img / *.raw).
//
// Write semantics:
//   - Read/Write are positioned I/O: there is no shared file position, so
//     concurrent reads do not serialize behind each other.
//   - Write never tears a caller's buffer and never silently short-writes;
//     a short transfer is retried and then reported as a failure.
//   - Flush is a durable flush (FlushFileBuffers on Windows): it returns only
//     after the data has been handed to stable storage, and reports failure
//     instead of pretending success.
#pragma once

#include <filesystem>
#include <memory>
#include <shared_mutex>

#include "xfs/block_device.h"

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
  // Reads share the lock; writes and flushes are exclusive. Every request
  // carries its own offset, so a reader can never observe a half-updated file
  // position.
  mutable std::shared_mutex io_mutex_;
  DeviceInfo info_;
  std::filesystem::path path_;
};

}  // namespace xfs
