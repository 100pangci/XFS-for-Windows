// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// RUI/RUD parser and preflight validation.  See include/xfs/rmap_intent.h.
#include "xfs/rmap_intent.h"

#include <limits>

#include "xfs/endian.h"
#include "xfs/log_format.h"
#include "xfs/rmap.h"

namespace xfs {
namespace {

namespace lf = logfmt;

constexpr uint64_t kMaxFileOffset = (1ULL << 54) + (1ULL << 21) - 2;

uint32_t LoadLE32At(const std::vector<uint8_t>& b, size_t off) {
  return LoadLE32(b.data() + off);
}
uint64_t LoadLE64At(const std::vector<uint8_t>& b, size_t off) {
  return static_cast<uint64_t>(LoadLE32(b.data() + off)) |
         (static_cast<uint64_t>(LoadLE32(b.data() + off + 4)) << 32);
}

}  // namespace

const char* RmapIntentOpName(uint32_t flags) {
  switch (flags & lf::kRmapExtentTypeMask) {
    case lf::kRmapExtentMap: return "MAP";
    case lf::kRmapExtentMapShared: return "MAP_SHARED";
    case lf::kRmapExtentUnmap: return "UNMAP";
    case lf::kRmapExtentUnmapShared: return "UNMAP_SHARED";
    case lf::kRmapExtentConvert: return "CONVERT";
    case lf::kRmapExtentConvertShared: return "CONVERT_SHARED";
    case lf::kRmapExtentAlloc: return "ALLOC";
    case lf::kRmapExtentFree: return "FREE";
    default: return "UNKNOWN";
  }
}

Result<RmapIntent> ParseRui(const std::vector<uint8_t>& format,
                            const XfsGeometry& geo) {
  if (format.size() < lf::kRuiFormatSize) {
    return XFS_ERR(XfsError::kCorruptBtree, "RUI item is too small")
        .Actual(format.size())
        .Metadata("RUI");
  }
  const uint32_t nextents = LoadLE32At(format, lf::kIntentNextentsOffset);
  if (nextents == 0) {
    return XFS_ERR(XfsError::kCorruptBtree, "RUI has no extents")
        .Metadata("RUI");
  }
  const uint64_t max_extents =
      (std::numeric_limits<size_t>::max() - lf::kRuiFormatSize) /
      lf::kRuiExtentSize;
  if (nextents > max_extents) {
    return XFS_ERR(XfsError::kCorruptBtree, "RUI extent count overflows")
        .Actual(nextents)
        .Metadata("RUI");
  }
  const size_t expected = lf::kRuiFormatSize +
                          static_cast<size_t>(nextents) * lf::kRuiExtentSize;
  if (format.size() != expected) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "RUI size does not match its extent count")
        .Expected(expected)
        .Actual(format.size())
        .Metadata("RUI");
  }

  RmapIntent intent;
  intent.id = LoadLE64At(format, lf::kIntentIdOffset);
  intent.extents.reserve(nextents);
  for (uint32_t i = 0; i < nextents; ++i) {
    const size_t base =
        lf::kRuiFormatSize + static_cast<size_t>(i) * lf::kRuiExtentSize;
    RmapIntentExtent e;
    e.owner = LoadLE64At(format, base + lf::kRuiOwnerOffset);
    e.startblock = LoadLE64At(format, base + lf::kRuiStartblockOffset);
    e.startoff = LoadLE64At(format, base + lf::kRuiStartoffOffset);
    e.len = LoadLE32At(format, base + lf::kRuiLenOffset);
    e.flags = LoadLE32At(format, base + lf::kRuiFlagsOffset);

    if ((e.flags & ~lf::kRmapExtentFlags) != 0) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "RUI extent has reserved flag bits")
          .Actual(e.flags)
          .Metadata("RUI");
    }
    const uint32_t op = e.flags & lf::kRmapExtentTypeMask;
    if (op < lf::kRmapExtentMap || op > lf::kRmapExtentFree) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "RUI extent has an unknown operation")
          .Actual(op)
          .Metadata("RUI");
    }
    if (e.len == 0) {
      return XFS_ERR(XfsError::kCorruptBtree, "RUI extent has zero length")
          .Metadata("RUI");
    }
    if (!geo.FsbRangeValid(e.startblock, e.len)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "RUI extent leaves its allocation group")
          .Fsb(e.startblock)
          .Expected(e.len)
          .Metadata("RUI");
    }
    if (RmapIntentIsShared(e.flags)) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     std::string("RUI requires unsupported reflink semantics: ") +
                         RmapIntentOpName(e.flags))
          .Actual(e.flags)
          .Metadata("RUI");
    }

    const bool special = RmapNonInodeOwner(e.owner);
    if (special) {
      if (e.owner > kRmapOwnerFs || e.owner < kRmapOwnerInodes) {
        return XFS_ERR(XfsError::kUnsupportedFeature,
                       "RUI has an unsupported special owner")
            .Actual(e.owner)
            .Metadata("RUI");
      }
      if (e.startoff != 0 || (e.flags & (lf::kRmapExtentAttrFork |
                                         lf::kRmapExtentBmbtBlock |
                                         lf::kRmapExtentUnwritten)) != 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "RUI metadata extent has an offset or fork flag")
            .Actual(e.flags)
            .Metadata("RUI");
      }
    } else {
      if (!geo.InoValid(e.owner)) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "RUI owner is not a valid inode number")
            .Actual(e.owner)
            .Metadata("RUI");
      }
      if (e.startoff > kMaxFileOffset ||
          e.len - 1 > kMaxFileOffset - e.startoff) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "RUI logical offset or length overflows")
            .Actual(e.startoff)
            .Metadata("RUI");
      }
      const bool bmbt = (e.flags & lf::kRmapExtentBmbtBlock) != 0;
      const bool attr = (e.flags & lf::kRmapExtentAttrFork) != 0;
      const bool unwritten = (e.flags & lf::kRmapExtentUnwritten) != 0;
      if (bmbt && e.startoff != 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "RUI bmbt extent has a nonzero logical offset")
            .Actual(e.startoff)
            .Metadata("RUI");
      }
      if (unwritten && (bmbt || attr)) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "RUI marks metadata/attr extent unwritten")
            .Metadata("RUI");
      }
      if (op == lf::kRmapExtentConvert && (bmbt || attr)) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "RUI CONVERT applies only to data extents")
            .Actual(e.flags)
            .Metadata("RUI");
      }
    }
    intent.extents.push_back(e);
  }
  return intent;
}

Result<uint64_t> ParseRud(const std::vector<uint8_t>& format) {
  if (format.size() != lf::kRudFormatSize) {
    return XFS_ERR(XfsError::kCorruptBtree, "RUD item has the wrong size")
        .Expected(lf::kRudFormatSize)
        .Actual(format.size())
        .Metadata("RUD");
  }
  const uint32_t pad = LoadLE32At(format, 4);
  if (pad != 0) {
    return XFS_ERR(XfsError::kCorruptBtree, "RUD pad field is nonzero")
        .Actual(pad)
        .Metadata("RUD");
  }
  return LoadLE64At(format, lf::kIntentIdOffset);
}

}  // namespace xfs
