// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - formatter (mkfs) interface.
//
// The formatter creates a brand new XFS v5 filesystem (crc=1, internal log,
// no realtime device) that Linux can mount natively. It is the only code
// path in this project that writes to a device, and it must never touch
// existing filesystem data by accident:
//   - the target must be opened exclusively
//   - callers are expected to have run the CLI safety checks
//   - Format() checks that the target size is sane and refuses nonsense
#pragma once

#include <cstdint>
#include <string>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/superblock.h"
#include "xfs/uuid.h"

namespace xfs::mkfs {

struct FormatOptions {
  uint32_t block_size = 4096;   // fs block size
  uint32_t sector_size = 0;     // 0 = device logical sector size
  uint32_t inode_size = 0;      // 0 = 512 (v5 default)
  std::string label;            // up to 12 bytes
  Uuid uuid{};                  // all-zero = generate random
  bool quiet = false;
  // Optional modern inode feature profile (RW-6A). None are enabled by
  // default, matching the historical image profile used by the older tests.
  bool finobt = false;
  bool inobtcount = false;
  bool sparse_inodes = false;
  bool bigtime = false;
  bool nrext64 = false;
};

struct FormatReport {
  uint32_t block_size = 0;
  uint32_t sector_size = 0;
  uint32_t inode_size = 0;
  uint64_t data_blocks = 0;
  uint64_t ag_count = 0;
  uint64_t ag_size_blocks = 0;
  uint64_t log_start = 0;
  uint64_t log_blocks = 0;
  uint64_t root_ino = 0;
  uint64_t inode_chunks = 0;
  Uuid uuid;
  std::string summary;
};

// Formats `device` as a fresh XFS v5 filesystem.
Result<FormatReport> Format(BlockDevice& device, const FormatOptions& options);

}  // namespace xfs::mkfs
