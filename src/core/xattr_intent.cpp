// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - ATTRI/ATTRD parsing (RW-6D).
#include "xfs/xattr_intent.h"

#include <algorithm>
#include <cstring>

#include "xfs/endian.h"
#include "xfs/parent.h"

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
  if (!geo.InoValid(out.ino)) {
    return XFS_ERR(XfsError::kCorruptBtree, "ATTRI inode is out of range")
        .Metadata("ATTRI")
        .Actual(out.ino);
  }
  if (value_len > kXattrMaxValueBytes) {
    return XFS_ERR(XfsError::kCorruptBtree, "ATTRI value length is invalid")
        .Metadata("ATTRI")
        .Actual(value_len);
  }

  const bool parent_op = out.op == XattrIntentOp::kParentSet ||
                         out.op == XattrIntentOp::kParentRemove ||
                         out.op == XattrIntentOp::kParentReplace;
  if (parent_op && !geo.features.parent) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "parent-pointer ATTRI on a filesystem without the parent "
                   "feature")
        .Metadata("ATTRI");
  }
  if (parent_op) {
    // xfs_attr_item.c: parent operations always carry exactly the parent
    // filter, a local 12-byte xfs_parent_rec, and the child generation.
    if (out.attr_filter != kAttrParent) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "parent-pointer ATTRI has a bad attribute filter")
          .Metadata("ATTRI")
          .Actual(out.attr_filter);
    }
    if (value_len != kParentRecSize) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "parent-pointer ATTRI value is not 12 bytes")
          .Metadata("ATTRI")
          .Expected(kParentRecSize)
          .Actual(value_len);
    }
  } else {
    const uint8_t nsp = out.attr_filter & kAttrNspOnDiskMask;
    if (nsp != 0 && nsp != kAttrRoot && nsp != kAttrSecure) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "ATTRI has an invalid namespace combination")
          .Metadata("ATTRI")
          .Actual(nsp);
    }
  }

  size_t expected_regions = 0;
  uint32_t name_len = 0;
  uint32_t old_name_len = 0;
  uint32_t new_name_len = 0;
  switch (out.op) {
    case XattrIntentOp::kSet:
    case XattrIntentOp::kReplace:
      name_len = LoadLE32(p + kAttriNameLenOffset);
      expected_regions = 2 + (value_len != 0 ? 1 : 0);
      break;
    case XattrIntentOp::kRemove:
      name_len = LoadLE32(p + kAttriNameLenOffset);
      expected_regions = 2;
      if (value_len != 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "ATTRI remove carries a value")
            .Metadata("ATTRI");
      }
      break;
    case XattrIntentOp::kParentSet:
    case XattrIntentOp::kParentRemove:
      name_len = LoadLE32(p + kAttriNameLenOffset);
      expected_regions = 3;
      break;
    case XattrIntentOp::kParentReplace:
      // xfs_attri_log_format overlays old_name_len/new_name_len over the
      // name_len field for PPTR_REPLACE.
      old_name_len = LoadLE16(p + kAttriNameLenOffset);
      new_name_len = LoadLE16(p + kAttriNameLenOffset + 2);
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
  auto valid_len = [](uint32_t len) { return len > 0 && len < 256; };
  if (out.op == XattrIntentOp::kParentReplace) {
    if (!valid_len(old_name_len) || !valid_len(new_name_len)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "parent-pointer ATTRI name length is invalid")
          .Metadata("ATTRI");
    }
  } else if (!valid_len(name_len)) {
    return XFS_ERR(XfsError::kCorruptBtree, "ATTRI name length is invalid")
        .Metadata("ATTRI")
        .Actual(name_len);
  }

  auto decode_parent = [&](const std::vector<uint8_t>& name_region,
                           uint32_t nlen,
                           const std::vector<uint8_t>& value_region,
                           const char* what,
                           ParentPointer* out_pointer) -> Status {
    auto name = ParseVector(name_region, nlen, what);
    if (!name.ok()) return name.error();
    auto value = ParseVector(value_region, kParentRecSize, what);
    if (!value.ok()) return value.error();
    XattrEntry entry;
    entry.flags = kAttrParent;
    entry.name.assign(reinterpret_cast<const char*>(name.value().data()),
                      name.value().size());
    entry.value = std::move(value.value());
    auto decoded = DecodeParentPointer(entry, geo, out.ino);
    if (!decoded.ok()) {
      auto error = decoded.error();
      error.metadata = "ATTRI";
      return error;
    }
    *out_pointer = std::move(decoded.value());
    return OkStatus();
  };

  if (out.op == XattrIntentOp::kParentReplace) {
    ParentPointer old_pointer;
    ParentPointer new_pointer;
    Status s = decode_parent(regions[1], old_name_len, regions[3],
                             "ATTRI old parent pointer", &old_pointer);
    if (!s.ok()) return s.error();
    s = decode_parent(regions[2], new_name_len, regions[4],
                      "ATTRI new parent pointer", &new_pointer);
    if (!s.ok()) return s.error();
    out.name = old_pointer.name;
    out.value = EncodeParentPointerValue(old_pointer);
    out.old_name = old_pointer.name;
    out.new_name = new_pointer.name;
    out.new_value = EncodeParentPointerValue(new_pointer);
    return out;
  }

  auto name = ParseVector(regions[1], name_len, "ATTRI name");
  if (!name.ok()) return name.error();
  out.name.assign(reinterpret_cast<const char*>(name.value().data()),
                  name.value().size());
  if (parent_op) {
    ParentPointer pointer;
    Status s = decode_parent(regions[1], name_len, regions[2],
                             "ATTRI parent pointer", &pointer);
    if (!s.ok()) return s.error();
    out.value = EncodeParentPointerValue(pointer);
  } else if (value_len != 0) {
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
