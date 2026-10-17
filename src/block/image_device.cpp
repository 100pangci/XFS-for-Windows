// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/image_device.h"

#include <system_error>

namespace xfs {

std::unique_ptr<ImageBlockDevice> ImageBlockDevice::Open(
    const std::filesystem::path& path, bool writable, uint32_t sector_size) {
  std::error_code ec;
  uint64_t size = std::filesystem::file_size(path, ec);
  if (ec) return nullptr;

  auto dev = std::unique_ptr<ImageBlockDevice>(new ImageBlockDevice());
  dev->path_ = path;
  std::ios::openmode mode = std::ios::binary | std::ios::in;
  if (writable) mode |= std::ios::out;
  dev->file_.open(path, mode);
  if (!dev->file_.is_open()) return nullptr;

  dev->info_.size_bytes = size;
  dev->info_.logical_sector_size = sector_size;
  dev->info_.physical_sector_size = sector_size;
  dev->info_.writable = writable;
  dev->info_.is_raw_device = false;
  dev->info_.description = path.string();
  return dev;
}

std::unique_ptr<ImageBlockDevice> ImageBlockDevice::Create(
    const std::filesystem::path& path, uint64_t size_bytes,
    uint32_t sector_size) {
  if (size_bytes == 0) return nullptr;

  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    std::filesystem::remove(path, ec);
    if (ec) return nullptr;
  }

  auto dev = std::unique_ptr<ImageBlockDevice>(new ImageBlockDevice());
  dev->path_ = path;
  dev->file_.open(path, std::ios::binary | std::ios::in | std::ios::out |
                            std::ios::trunc);
  if (!dev->file_.is_open()) return nullptr;

  // Size the file by writing at the last byte (sparse on NTFS).
  dev->file_.seekp(static_cast<std::streamoff>(size_bytes - 1));
  const char zero = 0;
  dev->file_.write(&zero, 1);
  dev->file_.flush();
  if (!dev->file_.good()) {
    dev->file_.close();
    std::filesystem::remove(path, ec);
    return nullptr;
  }
  dev->file_.seekg(0, std::ios::beg);

  dev->info_.size_bytes = size_bytes;
  dev->info_.logical_sector_size = sector_size;
  dev->info_.physical_sector_size = sector_size;
  dev->info_.writable = true;
  dev->info_.is_raw_device = false;
  dev->info_.description = path.string();
  return dev;
}

bool ImageBlockDevice::Read(uint64_t offset, void* buffer, size_t size) {
  std::lock_guard<std::mutex> lock(io_mutex_);
  if (!file_.is_open()) {
    last_error_ = "image not open";
    return false;
  }
  if (size == 0) return true;
  if (offset > info_.size_bytes || size > info_.size_bytes - offset) {
    last_error_ = "read out of bounds";
    return false;
  }
  file_.clear();
  file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  file_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
  if (!file_ || static_cast<size_t>(file_.gcount()) != size) {
    last_error_ = "short read";
    return false;
  }
  return true;
}

bool ImageBlockDevice::Write(uint64_t offset, const void* buffer,
                             size_t size) {
  std::lock_guard<std::mutex> lock(io_mutex_);
  if (!file_.is_open()) {
    last_error_ = "image not open";
    return false;
  }
  if (!info_.writable) {
    last_error_ = "image opened read-only";
    return false;
  }
  if (size == 0) return true;
  if (offset > info_.size_bytes || size > info_.size_bytes - offset) {
    last_error_ = "write out of bounds";
    return false;
  }
  file_.clear();
  file_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  file_.write(static_cast<const char*>(buffer),
              static_cast<std::streamsize>(size));
  if (!file_) {
    last_error_ = "short write";
    return false;
  }
  return true;
}

bool ImageBlockDevice::Flush() {
  if (!file_.is_open()) {
    last_error_ = "image not open";
    return false;
  }
  file_.flush();
  if (!file_.good()) {
    last_error_ = "flush failed";
    return false;
  }
  return true;
}

void ImageBlockDevice::Close() {
  if (file_.is_open()) file_.close();
}

}  // namespace xfs
