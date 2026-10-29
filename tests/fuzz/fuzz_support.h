// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Shared helpers for the libFuzzer harnesses.
//
// The harnesses are platform independent and link only xfs_core; they never
// include WinFsp or the CLI. Inputs are untrusted bytes and every parser must
// return an error instead of crashing, hanging or reading out of bounds.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/superblock.h"
#include "xfs/uuid.h"

namespace xfs::fuzz {

// A bounds-checked in-memory block device. Out-of-range I/O fails exactly like
// a truncated image; it is never clamped.
class MemoryDevice final : public BlockDevice {
 public:
  explicit MemoryDevice(size_t size = 1 << 20) : data_(size, 0) {
    info_.size_bytes = size;
    info_.logical_sector_size = 512;
    info_.physical_sector_size = 512;
    info_.writable = true;
    info_.description = "fuzz memory device";
  }

  bool Read(uint64_t offset, void* buffer, size_t size) override {
    if (!InRange(offset, size)) return false;
    std::memcpy(buffer, data_.data() + offset, size);
    return true;
  }

  bool Write(uint64_t offset, const void* buffer, size_t size) override {
    if (!InRange(offset, size)) return false;
    std::memcpy(data_.data() + offset, buffer, size);
    return true;
  }

  bool Flush() override { return true; }
  uint64_t Size() const override { return data_.size(); }
  uint32_t SectorSize() const override { return 512; }
  const DeviceInfo& Info() const override { return info_; }
  void Close() override {}

  std::vector<uint8_t>& bytes() { return data_; }
  const std::vector<uint8_t>& bytes() const { return data_; }

 private:
  bool InRange(uint64_t offset, size_t size) const {
    return offset <= data_.size() && size <= data_.size() - offset;
  }

  std::vector<uint8_t> data_;
  DeviceInfo info_;
};

inline constexpr Uuid kFuzzUuid = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                   0x47, 0x88, 0x99, 0xaa, 0xbb, 0xcc,
                                   0xdd, 0xee, 0xff, 0x00};

// Geometry used by the inode/directory/AG harnesses. It is deliberately a
// plausible v5 geometry: 4 KiB blocks, 512-byte sectors, 512-byte inodes,
// one AG, and the modern inode features enabled. Parser validation still
// rejects anything inconsistent with it.
inline XfsGeometry FuzzGeometry() {
  XfsGeometry g;
  g.block_size = 4096;
  g.sector_size = 512;
  g.inode_size = 512;
  g.inopblock = 8;
  g.block_log = 12;
  g.sector_log = 9;
  g.inode_log = 9;
  g.inopblog = 3;
  g.agblklog = 16;
  g.agno_log = 0;  // derived from agcount by the allocator, unused by parsers
  g.agino_log = 19;
  g.agcount = 1;
  g.agblocks = 65536;
  g.dblocks = 65536;
  g.logstart = 8;
  g.logblocks = 8;
  g.rootino = 128;
  g.dir_blk_log = 0;
  g.dir_block_size = 4096;
  g.ino_align = 1;
  g.uuid = kFuzzUuid;
  g.meta_uuid = kFuzzUuid;
  g.features.ftype = true;
  g.features.spinodes = true;
  g.features.finobt = true;
  g.features.rmapbt = true;
  g.features.reflink = true;
  g.features.bigtime = true;
  g.features.nrext64 = true;
  g.features.inobtcnt = true;
  return g;
}

inline void CopyInput(const uint8_t* data, size_t size, size_t offset,
                      std::vector<uint8_t>* out) {
  if (offset >= out->size() || size == 0) return;
  const size_t count = std::min(size, out->size() - offset);
  std::memcpy(out->data() + offset, data, count);
}

}  // namespace xfs::fuzz
