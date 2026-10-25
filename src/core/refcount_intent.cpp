// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/refcount_intent.h"

#include "xfs/endian.h"
#include "xfs/log_format.h"

namespace xfs {

namespace lf = logfmt;

Result<RefcountIntent> ParseRefcountIntent(const std::vector<uint8_t>& region,
                                           const XfsGeometry& geo) {
  if (region.size() < lf::kIntentLogFormatSize) {
    return XFS_ERR(XfsError::kCorruptBtree, "CUI format region is too small")
        .Actual(region.size())
        .Metadata("CUI");
  }
  const uint8_t* p = region.data();
  const uint16_t type = LoadLE16(p);
  const uint16_t size = LoadLE16(p + 2);
  const uint32_t nextents = LoadLE32(p + lf::kIntentNextentsOffset);
  if (type != lf::kItemCui) {
    return XFS_ERR(XfsError::kCorruptBtree, "CUI item type mismatch")
        .Expected(lf::kItemCui)
        .Actual(type)
        .Metadata("CUI");
  }
  if (size != 1) {
    return XFS_ERR(XfsError::kCorruptBtree, "CUI size field is not one")
        .Actual(size)
        .Metadata("CUI");
  }
  if (nextents == 0) {
    return XFS_ERR(XfsError::kCorruptBtree, "CUI has no extents")
        .Metadata("CUI");
  }
  constexpr size_t kExtentSize = 16;
  const uint64_t want = lf::kIntentLogFormatSize +
                        static_cast<uint64_t>(nextents) * kExtentSize;
  if (want != region.size()) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "CUI extent array does not match its size")
        .Expected(want)
        .Actual(region.size())
        .Metadata("CUI");
  }

  RefcountIntent out;
  out.id = LoadLE64(p + lf::kIntentIdOffset);
  out.extents.reserve(nextents);
  for (uint32_t i = 0; i < nextents; ++i) {
    const uint8_t* e = p + lf::kIntentLogFormatSize + i * kExtentSize;
    RefcountIntentExtent ext;
    ext.startblock = LoadLE64(e);
    ext.len = LoadLE32(e + 8);
    ext.flags = LoadLE32(e + 12);
    if (ext.flags & ~kRefcountIntentTypeMask) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "CUI has unsupported operation flags")
          .Actual(ext.flags)
          .Metadata("CUI");
    }
    const uint32_t op = ext.flags & kRefcountIntentTypeMask;
    if (op < 1 || op > 4) {
      return XFS_ERR(XfsError::kCorruptBtree, "CUI has an invalid operation")
          .Actual(op)
          .Metadata("CUI");
    }
    if (ext.len == 0 || !geo.FsbRangeValid(ext.startblock, ext.len)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "CUI extent is outside one allocation group")
          .Fsb(ext.startblock)
          .Actual(ext.len)
          .Metadata("CUI");
    }
    out.extents.push_back(ext);
  }
  return out;
}

Result<RefcountIntentDone> ParseRefcountIntentDone(
    const std::vector<uint8_t>& region) {
  if (region.size() != 16) {
    return XFS_ERR(XfsError::kCorruptBtree, "CUD format region size is not 16")
        .Expected(16)
        .Actual(region.size())
        .Metadata("CUD");
  }
  const uint8_t* p = region.data();
  const uint16_t type = LoadLE16(p);
  const uint16_t size = LoadLE16(p + 2);
  if (type != lf::kItemCud) {
    return XFS_ERR(XfsError::kCorruptBtree, "CUD item type mismatch")
        .Expected(lf::kItemCud)
        .Actual(type)
        .Metadata("CUD");
  }
  if (size != 1) {
    return XFS_ERR(XfsError::kCorruptBtree, "CUD size field is not one")
        .Actual(size)
        .Metadata("CUD");
  }
  RefcountIntentDone out;
  out.id = LoadLE64(p + lf::kIntentIdOffset);
  return out;
}

}  // namespace xfs
