// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - image file block device (*.img / *.raw).
#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "xfs/block_device.h"

namespace xfs {

class ImageBlockDevice final : public BlockDevice {
 public:
  // Opens an existing image. writable=false opens read-only.
  static std::unique_ptr<ImageBlockDevice> Open(const std::filesystem::path& path,
                                                bool writable,
                                                uint32_t sector_size = 512);

  // Creates (or overwrites) an image with the given size. The file is
  // allocated sparsely where the filesystem supports it.
  static std::unique_ptr<ImageBlockDevice> Create(const std::filesystem::path& path,
                                                  uint64_t size_bytes,
                                                  uint32_t sector_size = 512);

  bool Read(uint64_t offset, void* buffer, size_t size) override;
  bool Write(uint64_t offset, const void* buffer, size_t size) override;
  bool Flush() override;
  uint64_t Size() const override { return info_.size_bytes; }
  uint32_t SectorSize() const override { return info_.logical_sector_size; }
  const DeviceInfo& Info() const override { return info_; }
  void Close() override;

 private:
  ImageBlockDevice() = default;

  std::fstream file_;
  mutable std::mutex io_mutex_;
  DeviceInfo info_;
  std::filesystem::path path_;
};

}  // namespace xfs
