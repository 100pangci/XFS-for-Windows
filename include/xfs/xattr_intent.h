// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - ATTRI/ATTRD (logged extended-attribute operation)
// parsing (RW-6D).
//
// Linux only emits these items while the logged-xattrs log-incompat feature is
// active (the LARP debug knob / parent pointers).  The format was verified
// against libxfs/xfs_log_format.h and xfs_attr_item.c:
//
//   xfs_attri_log_format (40 bytes, host order)
//     +0  u16 type (XFS_LI_ATTRI)
//     +2  u16 size
//     +4  u32 igen        (parent-pointer operations only)
//     +8  u64 id
//     +16 u64 ino
//     +24 u32 op_flags
//     +28 u32 name_len
//     +32 u32 value_len
//     +36 u32 attr_filter
//
//   The format region is followed by the name bytes, the optional value bytes
//   (SET/REPLACE), or the old/new name/value vectors for the private
//   parent-pointer operations.  Every vector is 4-byte rounded in the log.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xfs/error.h"
#include "xfs/superblock.h"
#include "xfs/xattr.h"

namespace xfs {

enum class XattrIntentOp : uint32_t {
  kSet = 1,
  kRemove = 2,
  kReplace = 3,
  kParentSet = 4,
  kParentRemove = 5,
  kParentReplace = 6,
};

constexpr uint32_t kXattrIntentOpMask = 0xff;
constexpr uint32_t kXattrIntentFilterMask =
    kAttrRoot | kAttrSecure | kAttrParent | kAttrIncomplete;

struct XattrIntent {
  uint64_t id = 0;
  uint64_t ino = 0;
  uint32_t igen = 0;
  uint32_t attr_filter = 0;
  XattrIntentOp op = XattrIntentOp::kSet;
  std::string name;
  std::vector<uint8_t> value;
  std::string old_name;
  std::string new_name;
  std::vector<uint8_t> new_value;
};

struct XattrIntentDone {
  uint64_t id = 0;
};

// Parses an XFS_LI_ATTRI item (format + name/value regions).
Result<XattrIntent> ParseXattrIntent(
    const std::vector<std::vector<uint8_t>>& regions,
    const XfsGeometry& geo);
// Parses the single format region of XFS_LI_ATTRD (type 0x1247).
Result<XattrIntentDone> ParseXattrIntentDone(
    const std::vector<uint8_t>& region);

}  // namespace xfs
