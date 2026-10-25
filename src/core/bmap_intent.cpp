// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/bmap_intent.h"

#include "xfs/endian.h"
#include "xfs/log_format.h"

namespace xfs {

namespace lf = logfmt;

Result<BmapIntent> ParseBmapIntent(const std::vector<uint8_t>& region,
                                   const XfsGeometry& geo) {
  if (region.size() < lf::kIntentLogFormatSize) {
    return XFS_ERR(XfsError::kCorruptBtree, "BUI format region is too small")
        .Actual(region.size())
        .Metadata("BUI");
  }
  const uint8_t* p = region.data();
  const uint16_t type = LoadLE16(p);
  const uint16_t size = LoadLE16(p + 2);
  const uint32_t nextents = LoadLE32(p + lf::kIntentNextentsOffset);
  if (type != lf::kItemBui) {
    return XFS_ERR(XfsError::kCorruptBtree, "BUI item type mismatch")
        .Expected(lf::kItemBui)
        .Actual(type)
        .Metadata("BUI");
  }
  if (size != 1) {
    return XFS_ERR(XfsError::kCorruptBtree, "BUI size field is not one")
        .Actual(size)
        .Metadata("BUI");
  }
  // Linux accepts exactly one mapping operation per BUI
  // (xfs_bui_validate requires XFS_BUI_MAX_FAST_EXTENTS).
  if (nextents != 1) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "BUI must contain exactly one mapping")
        .Actual(nextents)
        .Metadata("BUI");
  }
  constexpr size_t kExtentSize = 32;
  const uint64_t want = lf::kIntentLogFormatSize +
                        static_cast<uint64_t>(nextents) * kExtentSize;
  if (want != region.size()) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "BUI extent array does not match its size")
        .Expected(want)
        .Actual(region.size())
        .Metadata("BUI");
  }
  constexpr uint64_t kMaxFileOffset = 1ull << 54;

  BmapIntent out;
  out.id = LoadLE64(p + lf::kIntentIdOffset);
  out.extents.reserve(nextents);
  for (uint32_t i = 0; i < nextents; ++i) {
    const uint8_t* e = p + lf::kIntentLogFormatSize + i * kExtentSize;
    BmapIntentExtent ext;
    ext.owner = LoadLE64(e);
    ext.startblock = LoadLE64(e + 8);
    ext.startoff = LoadLE64(e + 16);
    ext.len = LoadLE32(e + 24);
    ext.flags = LoadLE32(e + 28);
    if (ext.flags & ~kBmapIntentFlags) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "BUI has unsupported flag bits")
          .Actual(ext.flags)
          .Metadata("BUI");
    }
    if (ext.attr_fork()) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "BUI targets an attribute fork")
          .Ino(ext.owner)
          .Metadata("BUI");
    }
    if (ext.realtime()) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "BUI targets the realtime device")
          .Ino(ext.owner)
          .Metadata("BUI");
    }
    const uint32_t op = ext.flags & kBmapIntentTypeMask;
    if (op != static_cast<uint32_t>(BmapIntentType::kMap) &&
        op != static_cast<uint32_t>(BmapIntentType::kUnmap)) {
      return XFS_ERR(XfsError::kCorruptBtree, "BUI has an invalid operation")
          .Actual(op)
          .Metadata("BUI");
    }
    if (ext.len == 0 || ext.startoff > kMaxFileOffset ||
        ext.len > kMaxFileOffset - ext.startoff) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "BUI file range is invalid")
          .Ino(ext.owner)
          .Actual(ext.len)
          .Metadata("BUI");
    }
    if (!geo.InoValid(ext.owner)) {
      return XFS_ERR(XfsError::kCorruptBtree, "BUI owner is not an inode")
          .Ino(ext.owner)
          .Metadata("BUI");
    }
    if (!geo.FsbRangeValid(ext.startblock, ext.len)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "BUI physical range is invalid")
          .Ino(ext.owner)
          .Fsb(ext.startblock)
          .Actual(ext.len)
          .Metadata("BUI");
    }
    out.extents.push_back(ext);
  }
  return out;
}

Result<BmapIntentDone> ParseBmapIntentDone(
    const std::vector<uint8_t>& region) {
  if (region.size() != 16) {
    return XFS_ERR(XfsError::kCorruptBtree, "BUD format region size is not 16")
        .Expected(16)
        .Actual(region.size())
        .Metadata("BUD");
  }
  const uint8_t* p = region.data();
  const uint16_t type = LoadLE16(p);
  const uint16_t size = LoadLE16(p + 2);
  if (type != lf::kItemBud) {
    return XFS_ERR(XfsError::kCorruptBtree, "BUD item type mismatch")
        .Expected(lf::kItemBud)
        .Actual(type)
        .Metadata("BUD");
  }
  if (size != 1) {
    return XFS_ERR(XfsError::kCorruptBtree, "BUD size field is not one")
        .Actual(size)
        .Metadata("BUD");
  }
  BmapIntentDone out;
  out.id = LoadLE64(p + lf::kIntentIdOffset);
  return out;
}

}  // namespace xfs
