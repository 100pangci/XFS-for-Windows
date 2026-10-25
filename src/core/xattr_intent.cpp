// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - ATTRI/ATTRD parsing (RW-6D).
#include "xfs/xattr_intent.h"

#include <algorithm>
#include <cstring>

#include "xfs/endian.h"

namespace xfs {
namespace {

constexpr size_t kAttriFormatSize = 40;
constexpr size_t kAttriIdOffset = 8;
constexpr size_t kAttriInoOffset = 16;
constexpr size_t kAttriIgenOffset = 4;
constexpr size_t kAttriOpOffset = 24;
constexpr size_t kAttriNameLenOffset = 28;
constexpr size_t kAttriValueLenOffset = 32;
constexpr size_t kAttriFilterOffset = 36;
constexpr size_t kAttrdFormatSize = 16;
constexpr size_t kAttrdIdOffset = 8;

size_t RoundUp4(size_t value) { return (value + 3) & ~size_t{3}; }

Result<std::vector<uint8_t>> ParseVector(const std::vector<uint8_t>& region,
                                         size_t len, const char* what) {
  if (region.size() != RoundUp4(len)) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   std::string(what) + " length is not 4-byte rounded")
        .Metadata("ATTRI")
        .Expected(RoundUp4(len))
        .Actual(region.size());
  }
  if (len == 0) return std::vector<uint8_t>();
  return std::vector<uint8_t>(region.begin(), region.begin() + len);
}

}  // namespace

Result<XattrIntent> ParseXattrIntent(
    const std::vector<std::vector<uint8_t>>& regions,
    const XfsGeometry& geo) {
  if (regions.empty() || regions[0].size() != kAttriFormatSize) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "ATTRI format region has the wrong size")
        .Metadata("ATTRI");
  }
  const uint8_t* p = regions[0].data();
  if (LoadLE16(p) != 0x1246) {
    return XFS_ERR(XfsError::kCorruptBtree, "ATTRI has a bad item type")
        .Metadata("ATTRI")
        .Actual(LoadLE16(p));
  }
  if (LoadLE16(p + 2) != regions.size()) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "ATTRI has a bad region count")
        .Metadata("ATTRI")
        .Expected(regions.size())
        .Actual(LoadLE16(p + 2));
  }
  XattrIntent out;
  out.id = LoadLE64(p + kAttriIdOffset);
  out.ino = LoadLE64(p + kAttriInoOffset);
  out.igen = LoadLE32(p + kAttriIgenOffset);
  const uint32_t op_flags = LoadLE32(p + kAttriOpOffset);
  const uint32_t name_len = LoadLE32(p + kAttriNameLenOffset);
  const uint32_t value_len = LoadLE32(p + kAttriValueLenOffset);
  out.attr_filter = LoadLE32(p + kAttriFilterOffset);
  if ((op_flags & ~kXattrIntentOpMask) != 0) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "ATTRI has unknown operation flags")
        .Metadata("ATTRI")
        .Actual(op_flags);
  }
  out.op = static_cast<XattrIntentOp>(op_flags & kXattrIntentOpMask);
  if ((out.attr_filter & ~kXattrIntentFilterMask) != 0) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "ATTRI has unknown attribute filter bits")
        .Metadata("ATTRI")
        .Actual(out.attr_filter);
  }
  const uint8_t nsp = out.attr_filter & kAttrNspOnDiskMask;
  if (nsp != 0 && nsp != kAttrRoot && nsp != kAttrSecure &&
      nsp != kAttrParent) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "ATTRI has an invalid namespace combination")
        .Metadata("ATTRI")
        .Actual(nsp);
  }
  if (name_len == 0 || name_len > 255) {
    return XFS_ERR(XfsError::kCorruptBtree, "ATTRI name length is invalid")
        .Metadata("ATTRI")
        .Actual(name_len);
  }
  if (value_len > kXattrMaxValueBytes) {
    return XFS_ERR(XfsError::kCorruptBtree, "ATTRI value length is invalid")
        .Metadata("ATTRI")
        .Actual(value_len);
  }
  if (!geo.InoValid(out.ino)) {
    return XFS_ERR(XfsError::kCorruptBtree, "ATTRI inode is out of range")
        .Metadata("ATTRI")
        .Actual(out.ino);
  }

  size_t expected_regions = 0;
  switch (out.op) {
    case XattrIntentOp::kSet:
    case XattrIntentOp::kReplace:
      expected_regions = 2 + (value_len != 0 ? 1 : 0);
      break;
    case XattrIntentOp::kRemove:
      expected_regions = 2;
      if (value_len != 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "ATTRI remove carries a value")
            .Metadata("ATTRI");
      }
      break;
    case XattrIntentOp::kParentSet:
    case XattrIntentOp::kParentRemove:
      expected_regions = 3;
      break;
    case XattrIntentOp::kParentReplace:
      expected_regions = 5;
      break;
    default:
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "ATTRI has an unsupported operation")
          .Metadata("ATTRI")
          .Actual(op_flags);
  }
  if (regions.size() != expected_regions) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "ATTRI region count does not match the operation")
        .Metadata("ATTRI")
        .Expected(expected_regions)
        .Actual(regions.size());
  }

  auto name = ParseVector(regions[1], name_len, "ATTRI name");
  if (!name.ok()) return name.error();
  out.name.assign(reinterpret_cast<const char*>(name.value().data()),
                  name.value().size());
  if (value_len != 0) {
    auto value = ParseVector(regions[2], value_len, "ATTRI value");
    if (!value.ok()) return value.error();
    out.value = std::move(value.value());
  }
  Status valid = ValidateXattrName(out.name,
                                   static_cast<uint8_t>(out.attr_filter),
                                   true);
  if (!valid.ok()) {
    auto err = valid.error();
    err.metadata = "ATTRI";
    return err;
  }
  if (out.op == XattrIntentOp::kParentSet ||
      out.op == XattrIntentOp::kParentRemove ||
      out.op == XattrIntentOp::kParentReplace) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "parent-pointer ATTRI operations are outside RW-6D")
        .Metadata("ATTRI")
        .Actual(op_flags);
  }
  return out;
}

Result<XattrIntentDone> ParseXattrIntentDone(
    const std::vector<uint8_t>& region) {
  if (region.size() != kAttrdFormatSize) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "ATTRD format region has the wrong size")
        .Metadata("ATTRD")
        .Actual(region.size());
  }
  if (LoadLE16(region.data()) != 0x1247) {
    return XFS_ERR(XfsError::kCorruptBtree, "ATTRD has a bad item type")
        .Metadata("ATTRD")
        .Actual(LoadLE16(region.data()));
  }
  if (LoadLE16(region.data() + 2) != 1) {
    return XFS_ERR(XfsError::kCorruptBtree, "ATTRD has a bad region count")
        .Metadata("ATTRD")
        .Actual(LoadLE16(region.data() + 2));
  }
  XattrIntentDone out;
  out.id = LoadLE64(region.data() + kAttrdIdOffset);
  return out;
}

}  // namespace xfs
