// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - test fixture: a tiny filesystem with one file.
//
// The formatter only creates empty filesystems, and the writer has no
// allocator yet, so tests need an image that already contains a regular file
// with allocated, written blocks. This builder produces one:
//
//   1. format a fresh filesystem with the project formatter (all invariants
//      come from the formatter, not from hand-built structures),
//   2. allocate exactly one inode and one contiguous extent with the same
//      bookkeeping a real allocator does (bnobt, cntbt, AGF, AGI, inobt,
//      superblock counters),
//   3. insert one entry in the (shortform) root directory,
//   4. write deterministic file data.
//
// The result is a *strictly consistent* filesystem: CI verifies fixture images
// with Linux `xfs_repair -n`, a loop mount and an independent Python pattern
// check, so a bug in this builder cannot hide behind our own reader.
//
// Everything here is test support: it is not part of the library and is never
// used against a real device.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/uuid.h"

namespace xfstest {

// Deterministic file content. The pattern is also implemented (independently)
// in tests/tools/fixture_probe.py and tests/scripts/verify_fixture_linux.sh:
//   byte(i) = ((i * 167) ^ (i >> 7) ^ 0x5A) & 0xFF
inline uint8_t FixturePatternByte(uint64_t i) {
  return static_cast<uint8_t>((i * 167u) ^ (i >> 7) ^ 0x5Au);
}

struct FixtureOptions {
  uint64_t size_bytes = 300ull << 20;  // must satisfy the formatter profile
  uint32_t file_blocks = 8;            // 32 KiB of file data
  std::string file_name = "hello.bin";
  xfs::Uuid uuid{};
  bool quiet = true;
};

struct FixtureInfo {
  uint64_t block_size = 0;
  uint64_t file_ino = 0;
  uint64_t file_size = 0;
  uint64_t data_fsb = 0;      // first filesystem block of the file's extent
  uint64_t data_blocks = 0;   // blocks in the file
};

// Formats `device` and populates it. The device must be writable and large
// enough for the formatter profile (300 MiB is a safe choice).
xfs::Status BuildFixtureImage(xfs::BlockDevice& device,
                              const FixtureOptions& options,
                              FixtureInfo* info);

}  // namespace xfstest
