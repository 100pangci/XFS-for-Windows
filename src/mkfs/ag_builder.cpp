#include <cstring>
#include <algorithm>

#include "mkfs/mkfs_internal.h"
#include "xfs/ag.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/ondisk.h"

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
    PutBE32(rec, 4, 61);             // ir_freecount (full record format)
    PutBE64(rec, 8, ~7ULL);          // ir_free: bits 0-2 used
  }
  FinishBtreeCrc(geo, buf);
}

}  // namespace xfs::mkfs::internal
