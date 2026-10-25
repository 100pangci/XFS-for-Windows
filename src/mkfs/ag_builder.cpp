// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <cstring>
#include <algorithm>

#include "mkfs/mkfs_internal.h"
#include "xfs/ag.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/ondisk.h"
#include "xfs/rmap.h"

namespace xfs::mkfs::internal {
namespace {

namespace agf = ondisk::agf;
namespace agi = ondisk::agi;
namespace agfl = ondisk::agfl;
namespace bt = ondisk::btree;

constexpr uint32_t kNullAgBlock = 0xffffffffu;
constexpr uint32_t kNullAgino = 0xffffffffu;

uint64_t BlockDaddr(const FormatGeometry& geo, uint32_t agno,
                    uint32_t agbno) {
  // bb_blkno is a device daddr (512-byte units) computed from contiguous AG
  // byte offsets, not from the fsblock number encoding.
  return (geo.AgByteOffset(agno) >> 9) +
         (static_cast<uint64_t>(agbno) << (geo.blocklog - 9));
}

void InitBtreeHeader(const FormatGeometry& geo, std::vector<uint8_t>* buf,
                     uint32_t magic, uint16_t numrecs, uint32_t agno,
                     uint32_t agbno) {
  uint8_t* p = buf->data();
  PutBE32(p, bt::kMagic, magic);
  PutBE16(p, bt::kLevel, 0);
  PutBE16(p, bt::kNumrecs, numrecs);
  PutBE32(p, bt::kSLeftsib, kNullAgBlock);
  PutBE32(p, bt::kSRightSib, kNullAgBlock);
  PutBE64(p, bt::kSBlkno, BlockDaddr(geo, agno, agbno));
  PutBE64(p, bt::kSLsn, 0);
  std::memcpy(p + bt::kSUuid, geo.meta_uuid.data(), 16);
  PutBE32(p, bt::kSOwner, agno);
}

void FinishBtreeCrc(const FormatGeometry& geo, std::vector<uint8_t>* buf) {
  UpdateXfsChecksum(buf->data(), geo.block_size, bt::kSCrc);
}

}  // namespace

void BuildAgfImage(const FormatGeometry& geo, const FormatGeometry::AgState& ag,
                   std::vector<uint8_t>* buf) {
  buf->assign(geo.sector_size, 0);
  uint8_t* p = buf->data();

  PutBE32(p, agf::kMagic, ondisk::kAgfMagic);
  PutBE32(p, agf::kVersionnum, agf::kVersion);
  PutBE32(p, agf::kSeqno, ag.agno);
  PutBE32(p, agf::kLength, ag.agsize);
  PutBE32(p, agf::kBnoRoot, geo.bno_block);
  PutBE32(p, agf::kCntRoot, geo.cnt_block);
  PutBE32(p, agf::kBnoLevel, 1);
  PutBE32(p, agf::kCntLevel, 1);
  PutBE32(p, agf::kFlfirst, 0);
  PutBE32(p, agf::kFllast, ag.agfl.empty()
                                ? 0u
                                : static_cast<uint32_t>(ag.agfl.size() - 1));
  PutBE32(p, agf::kFlcount, static_cast<uint32_t>(ag.agfl.size()));
  uint32_t free_blocks = 0;
  uint32_t longest = 0;
  for (const auto& extent : ag.free_extents) {
    free_blocks += extent.count;
    if (extent.count > longest) longest = extent.count;
  }
  PutBE32(p, agf::kFreeblks, free_blocks);
  PutBE32(p, agf::kLongest, longest);
  // xfs_repair counts free-space btree blocks except the roots; a fresh
  // filesystem has only roots, so this counter is zero.
  PutBE32(p, agf::kBtreeblks, 0);
  if (geo.rmapbt) {
    PutBE32(p, agf::kRmapRoot, geo.rmap_block);
    PutBE32(p, agf::kRmapLevel, 1);
    PutBE32(p, agf::kRmapBlocks, 1);
  }
  if (geo.reflink) {
    PutBE32(p, agf::kRefcountRoot, geo.refcount_block);
    PutBE32(p, agf::kRefcountLevel, 1);
    PutBE32(p, agf::kRefcountBlocks, 1);
  }
  std::memcpy(p + agf::kUuid, geo.meta_uuid.data(), 16);
  PutBE64(p, agf::kLsn, 0);
  UpdateXfsChecksum(p, geo.sector_size, agf::kCrc);
}

void BuildAgiImage(const FormatGeometry& geo, const FormatGeometry::AgState& ag,
                   std::vector<uint8_t>* buf) {
  buf->assign(geo.sector_size, 0);
  uint8_t* p = buf->data();

  PutBE32(p, agi::kMagic, ondisk::kAgiMagic);
  PutBE32(p, agi::kVersionnum, agi::kVersion);
  PutBE32(p, agi::kSeqno, ag.agno);
  PutBE32(p, agi::kLength, ag.agsize);
  PutBE32(p, agi::kCount, ag.has_inodes ? 64u : 0u);
  PutBE32(p, agi::kRoot, geo.ibt_block);
  PutBE32(p, agi::kLevel, 1);
  PutBE32(p, agi::kFreecount, ag.has_inodes ? 61u : 0u);
  PutBE32(p, agi::kNewino, ag.has_inodes ? geo.rootino_agino : kNullAgino);
  PutBE32(p, agi::kDirino, ag.has_inodes ? geo.rootino_agino : kNullAgino);
  for (size_t i = 0; i < agi::kUnlinkedBuckets; ++i) {
    PutBE32(p, agi::kUnlinked + i * 4, kNullAgino);
  }
  if (geo.finobt) {
    PutBE32(p, agi::kFreeRoot, geo.fibt_block);
    PutBE32(p, agi::kFreeLevel, 1);
  }
  if (geo.inobtcount) {
    PutBE32(p, agi::kIblocks, 1);
    PutBE32(p, agi::kFblocks, geo.finobt ? 1 : 0);
  }
  std::memcpy(p + agi::kUuid, geo.meta_uuid.data(), 16);
  PutBE64(p, agi::kLsn, 0);
  UpdateXfsChecksum(p, geo.sector_size, agi::kCrc);
}

void BuildAgflImage(const FormatGeometry& geo,
                    const FormatGeometry::AgState& ag,
                    std::vector<uint8_t>* buf) {
  buf->assign(geo.sector_size, 0);
  uint8_t* p = buf->data();

  PutBE32(p, agfl::kMagic, ondisk::kAgflMagic);
  PutBE32(p, agfl::kSeqno, ag.agno);
  std::memcpy(p + agfl::kUuid, geo.meta_uuid.data(), 16);
  PutBE64(p, agfl::kLsn, 0);

  uint32_t entries = (geo.sector_size - kAgflHeaderSize) / 4;
  for (uint32_t i = 0; i < entries; ++i) {
    uint32_t value = i < ag.agfl.size() ? ag.agfl[i] : kNullAgBlock;
    PutBE32(p, kAgflHeaderSize + i * 4, value);
  }
  UpdateXfsChecksum(p, geo.sector_size, agfl::kCrc);
}

void BuildBnobtRoot(const FormatGeometry& geo,
                    const FormatGeometry::AgState& ag,
                    std::vector<uint8_t>* buf) {
  buf->assign(geo.block_size, 0);
  InitBtreeHeader(geo, buf, ondisk::kBtbCrcMagic,
                  static_cast<uint16_t>(ag.free_extents.size()), ag.agno,
                  geo.bno_block);
  uint8_t* rec = buf->data() + bt::kSHeaderLenV5;
  for (const auto& extent : ag.free_extents) {
    PutBE32(rec, 0, extent.start);
    PutBE32(rec, 4, extent.count);
    rec += 8;
  }
  FinishBtreeCrc(geo, buf);
}

void BuildCntbtRoot(const FormatGeometry& geo,
                    const FormatGeometry::AgState& ag,
                    std::vector<uint8_t>* buf) {
  buf->assign(geo.block_size, 0);
  InitBtreeHeader(geo, buf, ondisk::kBtcCrcMagic,
                  static_cast<uint16_t>(ag.free_extents.size()), ag.agno,
                  geo.cnt_block);
  // The cntbt is ordered by (blockcount, startblock).
  std::vector<FormatGeometry::FreeExtent> extents = ag.free_extents;
  std::sort(extents.begin(), extents.end(),
            [](const FormatGeometry::FreeExtent& a,
               const FormatGeometry::FreeExtent& b) {
              if (a.count != b.count) return a.count < b.count;
              return a.start < b.start;
            });
  uint8_t* rec = buf->data() + bt::kSHeaderLenV5;
  for (const auto& extent : extents) {
    PutBE32(rec, 0, extent.start);
    PutBE32(rec, 4, extent.count);
    rec += 8;
  }
  FinishBtreeCrc(geo, buf);
}

void BuildInobtRoot(const FormatGeometry& geo,
                    const FormatGeometry::AgState& ag,
                    std::vector<uint8_t>* buf) {
  buf->assign(geo.block_size, 0);
  uint16_t numrecs = ag.has_inodes ? 1 : 0;
  InitBtreeHeader(geo, buf, ondisk::kIbtCrcMagic, numrecs, ag.agno,
                  geo.ibt_block);
  if (ag.has_inodes) {
    uint8_t* rec = buf->data() + bt::kSHeaderLenV5;
    uint32_t startino = ag.chunk_agbno << geo.inopblog;
    PutBE32(rec, 0, startino);       // ir_startino
    // Root directory plus rt bitmap/summary inodes are allocated.
    if (geo.sparse_inodes) {
      PutBE16(rec, bt::kInobtRecHolemask, bt::kInobtHolemaskFull);
      rec[bt::kInobtRecCount] = 64;  // ir_count
      rec[bt::kInobtRecSparseFreecount] = 61;
    } else {
      PutBE32(rec, bt::kInobtRecFreecount, 61);
    }
    PutBE64(rec, 8, ~7ULL);          // ir_free: bits 0-2 used
  }
  FinishBtreeCrc(geo, buf);
}

void BuildRmapRoot(const FormatGeometry& geo,
                   const FormatGeometry::AgState& ag,
                   std::vector<uint8_t>* buf) {
  struct Obs {
    uint32_t start;
    uint32_t len;
    uint64_t owner;
  };
  std::vector<Obs> records;
  const uint32_t static_len = geo.agfl_block + 1;
  records.push_back({0, static_len, kRmapOwnerFs});
  records.push_back({geo.bno_block, 1, kRmapOwnerAg});
  records.push_back({geo.cnt_block, 1, kRmapOwnerAg});
  records.push_back({geo.ibt_block, 1, kRmapOwnerInobt});
  if (geo.finobt) records.push_back({geo.fibt_block, 1, kRmapOwnerInobt});
  if (geo.rmapbt) records.push_back({geo.rmap_block, 1, kRmapOwnerAg});
  if (geo.reflink) {
    records.push_back({geo.refcount_block, 1, kRmapOwnerRefc});
  }

  // Active AGFL entries are owned by OWN_AG.  The formatter fills them from
  // the start of free space, so they are contiguous; group anyway.
  if (!ag.agfl.empty()) {
    std::vector<uint32_t> entries = ag.agfl;
    std::sort(entries.begin(), entries.end());
    size_t i = 0;
    while (i < entries.size()) {
      const uint32_t start = entries[i];
      size_t j = i + 1;
      while (j < entries.size() && entries[j] == entries[j - 1] + 1) ++j;
      records.push_back({start, static_cast<uint32_t>(j - i), kRmapOwnerAg});
      i = j;
    }
  }
  if (ag.has_inodes) {
    records.push_back({ag.chunk_agbno, ag.chunk_blocks, kRmapOwnerInodes});
  }
  if (ag.agno == geo.log_agno) {
    const uint32_t mask = (1u << geo.agblklog) - 1u;
    records.push_back({static_cast<uint32_t>(geo.logstart) & mask,
                       geo.logblocks, kRmapOwnerLog});
  }
  std::sort(records.begin(), records.end(), [](const Obs& a, const Obs& b) {
    if (a.start != b.start) return a.start < b.start;
    return a.owner < b.owner;
  });
  // mkfs emits canonical records: adjacent special-owner ranges belong to the
  // same owner and are merged (all special owners have offset/flags zero).
  std::vector<Obs> merged;
  for (const Obs& r : records) {
    if (!merged.empty() && merged.back().owner == r.owner &&
        RmapNonInodeOwner(r.owner) &&
        static_cast<uint64_t>(merged.back().start) +
                merged.back().len ==
            r.start) {
      merged.back().len += r.len;
      continue;
    }
    merged.push_back(r);
  }
  records = std::move(merged);

  const uint32_t max_leaf =
      static_cast<uint32_t>((geo.block_size - bt::kSHeaderLenV5) /
                            bt::kRmapRecSize);
  buf->assign(geo.block_size, 0);
  InitBtreeHeader(geo, buf, ondisk::kRmapCrcMagic,
                  static_cast<uint16_t>(records.size()), ag.agno,
                  geo.rmap_block);
  uint8_t* rec = buf->data() + bt::kSHeaderLenV5;
  for (const Obs& r : records) {
    PutBE32(rec, 0, r.start);
    PutBE32(rec, 4, r.len);
    PutBE64(rec, 8, r.owner);
    PutBE64(rec, 16, 0);  // offset + flags: all formatter owners are special
    rec += bt::kRmapRecSize;
  }
  if (records.size() > max_leaf) {
    // The formatter profile only emits a single root leaf.  Refusing here is
    // safer than writing an invalid tree; the tested geometries are far below
    // this limit.
    buf->assign(geo.block_size, 0);
  }
  FinishBtreeCrc(geo, buf);
}

void BuildRefcountRoot(const FormatGeometry& geo,
                       const FormatGeometry::AgState& ag,
                       std::vector<uint8_t>* buf) {
  buf->assign(geo.block_size, 0);
  InitBtreeHeader(geo, buf, ondisk::kRefcountCrcMagic, 0, ag.agno,
                  geo.refcount_block);
  FinishBtreeCrc(geo, buf);
}

void BuildFinobtRoot(const FormatGeometry& geo,
                     const FormatGeometry::AgState& ag,
                     std::vector<uint8_t>* buf) {
  buf->assign(geo.block_size, 0);
  uint16_t numrecs = ag.has_inodes ? 1 : 0;
  InitBtreeHeader(geo, buf, ondisk::kFibtCrcMagic, numrecs, ag.agno,
                  geo.fibt_block);
  if (ag.has_inodes) {
    uint8_t* rec = buf->data() + bt::kSHeaderLenV5;
    uint32_t startino = ag.chunk_agbno << geo.inopblog;
    PutBE32(rec, 0, startino);
    if (geo.sparse_inodes) {
      PutBE16(rec, bt::kInobtRecHolemask, bt::kInobtHolemaskFull);
      rec[bt::kInobtRecCount] = 64;
      rec[bt::kInobtRecSparseFreecount] = 61;
    } else {
      PutBE32(rec, bt::kInobtRecFreecount, 61);
    }
    PutBE64(rec, 8, ~7ULL);
  }
  FinishBtreeCrc(geo, buf);
}

}  // namespace xfs::mkfs::internal
