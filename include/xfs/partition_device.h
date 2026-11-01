// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - bounds-checked view of one partition on a block device.
//
// UX-1 mounts the XFS filesystem that lives at a partition offset, but the
// DEV-1 raw backend owns a whole-disk handle (volume ownership is a whole-disk
// operation).  OffsetBlockDevice wraps that handle without changing any
// backend semantics: every request is translated by a fixed byte offset and
// checked against the partition length.  The wrapper never reopens, retries or
// reconnects a device; a terminal backend state is reported unchanged.
#pragma once

#include <cstdint>
#include <memory>

#include "xfs/block_device.h"
#include "xfs/error.h"

namespace xfs {

class OffsetBlockDevice final : public BlockDevice {
 public:
  // Takes ownership of `inner`.  `offset` and `length` must be non-zero,
  // sector aligned and fit inside inner->Size().  The wrapper refuses to open
  // (returning an error) instead of silently truncating an out-of-range view.
  static Result<std::unique_ptr<OffsetBlockDevice>> Create(
      std::unique_ptr<BlockDevice> inner, uint64_t offset, uint64_t length);

  ~OffsetBlockDevice() override;
  OffsetBlockDevice(const OffsetBlockDevice&) = delete;
  OffsetBlockDevice& operator=(const OffsetBlockDevice&) = delete;

  bool Read(uint64_t offset, void* buffer, size_t size) override;
  bool Write(uint64_t offset, const void* buffer, size_t size) override;
  bool Flush() override;
  uint64_t Size() const override { return length_; }
  uint32_t SectorSize() const override { return inner_->SectorSize(); }
  const DeviceInfo& Info() const override { return info_; }
  DeviceState State() const override { return inner_->State(); }
  DeviceErrorKind LastErrorKind() const override {
    return inner_->LastErrorKind();
  }
  void Close() override;

  uint64_t PartitionOffset() const { return offset_; }
  uint64_t PartitionLength() const { return length_; }
  BlockDevice& Inner() { return *inner_; }
  const BlockDevice& Inner() const { return *inner_; }

 private:
  OffsetBlockDevice() = default;

  // Overflow-safe [offset, size) check against the partition length.
  bool InRange(uint64_t offset, size_t size) const;

  std::unique_ptr<BlockDevice> inner_;
  uint64_t offset_ = 0;
  uint64_t length_ = 0;
  DeviceInfo info_;
};

}  // namespace xfs