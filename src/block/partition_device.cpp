// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/partition_device.h"

#include <limits>
#include <utility>

namespace xfs {

Result<std::unique_ptr<OffsetBlockDevice>> OffsetBlockDevice::Create(
    std::unique_ptr<BlockDevice> inner, uint64_t offset, uint64_t length) {
  if (!inner) {
    return XFS_ERR(XfsError::kInvalidArgument, "no block device supplied");
  }
  if (length == 0) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "partition length must not be zero");
  }
  const uint64_t device_size = inner->Size();
  if (offset > device_size || length > device_size - offset) {
    return XFS_ERR(XfsError::kOutOfRange,
                   "partition lies outside the block device")
        .Expected(device_size)
        .Actual(offset)
        .Metadata("partition");
  }
  const uint32_t sector = inner->SectorSize();
  if (sector == 0 || offset % sector != 0 || length % sector != 0) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "partition offset and length must be sector aligned")
        .Actual(sector)
        .Metadata("partition");
  }

  auto view = std::unique_ptr<OffsetBlockDevice>(new OffsetBlockDevice());
  view->inner_ = std::move(inner);
  view->offset_ = offset;
  view->length_ = length;
  view->info_ = view->inner_->Info();
  view->info_.size_bytes = length;
  view->info_.alignment_offset = 0;
  view->info_.description = "partition view: " + view->info_.description;
  if (view->info_.partition_number == 0) {
    view->info_.partition_number = 0;
  }
  return view;
}

OffsetBlockDevice::~OffsetBlockDevice() { Close(); }

bool OffsetBlockDevice::InRange(uint64_t offset, size_t size) const {
  const uint64_t request = static_cast<uint64_t>(size);
  if (offset > length_) return false;
  return request <= length_ - offset;
}

bool OffsetBlockDevice::Read(uint64_t offset, void* buffer, size_t size) {
  if (!InRange(offset, size)) {
    SetError("read outside the partition");
    return false;
  }
  if (size == 0) return true;
  return inner_->Read(offset_ + offset, buffer, size);
}

bool OffsetBlockDevice::Write(uint64_t offset, const void* buffer, size_t size) {
  if (!InRange(offset, size)) {
    SetError("write outside the partition");
    return false;
  }
  if (size == 0) return true;
  return inner_->Write(offset_ + offset, buffer, size);
}

bool OffsetBlockDevice::Flush() { return inner_->Flush(); }

void OffsetBlockDevice::Close() {
  if (!inner_) return;
  inner_->Close();
}

}  // namespace xfs