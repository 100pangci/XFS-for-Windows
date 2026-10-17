// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - shared test fixture image (one file, allocated extents).
//
// XfsFilesystem::Mount takes ownership of the device it mounts, but tests want
// to mount the same storage repeatedly (read-only, read-write, after a "crash")
// and to reset it between tests. FixtureImage keeps the storage, hands out
// non-owning BorrowedBlockDevice wrappers, and can restore the pristine image.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "support/fixture_image.h"
#include "test_util.h"
#include "xfs/block_device.h"
#include "xfs/filesystem.h"

namespace xfstest {

// Forwards to a device owned by someone else. Mounting a filesystem on a
// wrapper never closes the underlying storage.
class BorrowedBlockDevice final : public xfs::BlockDevice {
 public:
  explicit BorrowedBlockDevice(xfs::BlockDevice* device) : device_(device) {}

  // Pretend the device is not writable (mount policy tests).
  void SetWritable(bool writable) { writable_ = writable; }

  bool Read(uint64_t offset, void* buffer, size_t size) override {
    return device_->Read(offset, buffer, size);
  }
  bool Write(uint64_t offset, const void* buffer, size_t size) override {
    if (!writable_) {
      last_error_ = "test device opened read-only";
      return false;
    }
    return device_->Write(offset, buffer, size);
  }
  bool Flush() override { return device_->Flush(); }
  uint64_t Size() const override { return device_->Size(); }
  uint32_t SectorSize() const override { return device_->SectorSize(); }
  const xfs::DeviceInfo& Info() const override {
    info_ = device_->Info();
    info_.writable = writable_;
    return info_;
  }
  void Close() override {}

 private:
  xfs::BlockDevice* device_;
  mutable xfs::DeviceInfo info_;
  bool writable_ = true;
};

class FixtureImage {
 public:
  // Builds the image once per process (the formatter profile needs a 256+ MiB
  // device because of the 64 MiB minimum log).
  static FixtureImage& Instance();

  xfs::BlockDevice& storage() { return *device_; }
  const FixtureInfo& info() const { return info_; }
  // The pristine image (used by tests to prove that a mutation did not touch
  // metadata regions).
  const std::vector<uint8_t>& pristine() const { return pristine_; }
  // Restores the pristine image (call at the start of every mutating test).
  void Reset();
  // Mounts the current image; read_write requests a writable mount.
  xfs::Result<std::unique_ptr<xfs::XfsFilesystem>> Mount(bool read_write);
  // Path of the fixture file inside the filesystem.
  const std::string& FileName() const { return options_.file_name; }

 private:
  FixtureImage();

  FixtureOptions options_;
  std::unique_ptr<xfstest::MemoryBlockDevice> device_;
  std::vector<uint8_t> pristine_;
  FixtureInfo info_;
};

}  // namespace xfstest
