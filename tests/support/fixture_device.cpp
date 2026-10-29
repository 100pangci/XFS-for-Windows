// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "support/fixture_device.h"

#include <cstring>
#include <memory>

namespace xfstest {

FixtureImage::FixtureImage() {
  options_.size_bytes = 300ull << 20;
  options_.uuid = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x47, 0x88,
                   0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00};
  device_ = std::make_unique<MemoryBlockDevice>(options_.size_bytes, 512);
  xfs::Status status = BuildFixtureImage(*device_, options_, &info_);
  if (!status.ok()) {
    Fail(__FILE__, __LINE__,
         "fixture image build failed: " + status.error().Format());
  }
  pristine_ = device_->bytes();
}

FixtureImage& FixtureImage::Instance() {
  static FixtureImage instance;
  return instance;
}

void FixtureImage::Reset() { device_->bytes() = pristine_; }

xfs::Result<std::unique_ptr<xfs::XfsFilesystem>> FixtureImage::Mount(
    bool read_write) {
  auto borrowed = std::make_unique<BorrowedBlockDevice>(device_.get());
  xfs::MountOptions options;
  options.read_write = read_write;
  return xfs::XfsFilesystem::Mount(std::move(borrowed), options);
}

}  // namespace xfstest
