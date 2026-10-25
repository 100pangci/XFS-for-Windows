// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - RUI/RUD log intent parser (RW-6B).
//
// RUI and RUD are host-order log items:
//
//   struct xfs_rui_log_format {
//       u16 type; u16 size; u32 nextents; u64 id;
//       struct xfs_map_extent { u64 owner, startblock, startoff;
//                               u32 len, flags; } extents[];
//   };
//   struct xfs_rud_log_format { u16 type; u16 size; u32 pad; u64 id; };
//
// `startblock` is an encoded filesystem block, not a linear daddr.  The parser
// validates every extent and refuses shared (reflink) operations: this build
// keeps reflink disabled, and a recovery that cannot reproduce refcount
// semantics must fail before the first metadata write.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xfs/error.h"
#include "xfs/superblock.h"

namespace xfs {

struct RmapIntentExtent {
  uint64_t owner = 0;
  uint64_t startblock = 0;  // encoded fsblock
  uint64_t startoff = 0;
  uint32_t len = 0;
  uint32_t flags = 0;
};

struct RmapIntent {
  uint64_t id = 0;
  std::vector<RmapIntentExtent> extents;
};

// Parses one RUI item format (region 0 as reconstructed from the log).  All
// structural, overflow, geometry, owner and flag checks are performed here.
Result<RmapIntent> ParseRui(const std::vector<uint8_t>& format,
                            const XfsGeometry& geo);

// Parses a RUD item (16 bytes) and returns the RUI id.
Result<uint64_t> ParseRud(const std::vector<uint8_t>& format);

// True for operations that are only meaningful on a reflink filesystem.
inline bool RmapIntentIsShared(uint32_t flags) {
  const uint32_t op = flags & 0xffu;
  return op == 2u || op == 4u || op == 6u;
}

// Human-readable operation name for diagnostics.
const char* RmapIntentOpName(uint32_t flags);

}  // namespace xfs
