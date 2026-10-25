// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - BUI/BUD (bmap update intent/done) parsing.
//
// The format was verified against libxfs/xfs_log_format.h and
// xfs_bmap_item.c.  A BUI describes one deferred data-fork mapping change; the
// matching BUD cancels it after the deferred bmap work commits.
#pragma once

#include <cstdint>
#include <vector>

#include "xfs/error.h"
#include "xfs/superblock.h"

namespace xfs {

enum class BmapIntentType : uint32_t {
  kMap = 1,
  kUnmap = 2,
};

constexpr uint32_t kBmapIntentTypeMask = 0xff;
constexpr uint32_t kBmapIntentAttrFork = 1u << 31;
constexpr uint32_t kBmapIntentUnwritten = 1u << 30;
constexpr uint32_t kBmapIntentRealtime = 1u << 29;
constexpr uint32_t kBmapIntentFlags =
    kBmapIntentTypeMask | kBmapIntentAttrFork | kBmapIntentUnwritten |
    kBmapIntentRealtime;

struct BmapIntentExtent {
  uint64_t owner = 0;
  uint64_t startblock = 0;
  uint64_t startoff = 0;
  uint32_t len = 0;
  uint32_t flags = 0;

  BmapIntentType type() const {
    return static_cast<BmapIntentType>(flags & kBmapIntentTypeMask);
  }
  bool attr_fork() const { return (flags & kBmapIntentAttrFork) != 0; }
  bool unwritten() const { return (flags & kBmapIntentUnwritten) != 0; }
  bool realtime() const { return (flags & kBmapIntentRealtime) != 0; }
};

struct BmapIntent {
  uint64_t id = 0;
  std::vector<BmapIntentExtent> extents;
};

struct BmapIntentDone {
  uint64_t id = 0;
};

// Parses the single format region of XFS_LI_BUI (type 0x1244).
Result<BmapIntent> ParseBmapIntent(const std::vector<uint8_t>& region,
                                   const XfsGeometry& geo);
// Parses the single format region of XFS_LI_BUD (type 0x1245).
Result<BmapIntentDone> ParseBmapIntentDone(
    const std::vector<uint8_t>& region);

}  // namespace xfs
