// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - CUI/CUD (refcount update intent/done) parsing (RW-6C).
//
// CUI and CUD are host-order log items.  The format was verified against
// libxfs/xfs_log_format.h, xfs_refcount_item.c and xfs_refcount.c in the local
// Linux tree.  CUI carries the operation type in the low byte of pe_flags:
// 1 INCREASE, 2 DECREASE, 3 ALLOC_COW, 4 FREE_COW.
#pragma once

#include <cstdint>
#include <vector>

#include "xfs/error.h"
#include "xfs/superblock.h"

namespace xfs {

enum class RefcountIntentType : uint32_t {
  kIncrease = 1,
  kDecrease = 2,
  kAllocCow = 3,
  kFreeCow = 4,
};

constexpr uint32_t kRefcountIntentTypeMask = 0xff;

struct RefcountIntentExtent {
  uint64_t startblock = 0;  // encoded filesystem block (one AG)
  uint32_t len = 0;
  uint32_t flags = 0;
  RefcountIntentType type() const {
    return static_cast<RefcountIntentType>(flags & kRefcountIntentTypeMask);
  }
};

struct RefcountIntent {
  uint64_t id = 0;
  std::vector<RefcountIntentExtent> extents;
};

struct RefcountIntentDone {
  uint64_t id = 0;
};

// Parses the single format region of XFS_LI_CUI (type 0x1242).
Result<RefcountIntent> ParseRefcountIntent(const std::vector<uint8_t>& region,
                                           const XfsGeometry& geo);
// Parses the single format region of XFS_LI_CUD (type 0x1243).
Result<RefcountIntentDone> ParseRefcountIntentDone(
    const std::vector<uint8_t>& region);

}  // namespace xfs
