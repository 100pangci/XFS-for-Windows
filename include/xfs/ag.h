// XFS for Windows - allocation group headers (AGF / AGI / AGFL).
#pragma once

#include <cstdint>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/superblock.h"

namespace xfs {

struct XfsAgf {
  uint32_t seqno = 0;
  uint32_t length = 0;  // AG length in fs blocks

  uint32_t bno_root = 0, cnt_root = 0, rmap_root = 0, refcount_root = 0;
  uint32_t bno_level = 0, cnt_level = 0, rmap_level = 0, refcount_level = 0;

  uint32_t flfirst = 0, fllast = 0, flcount = 0;
  uint32_t freeblks = 0, longest = 0, btreeblks = 0;
  uint32_t rmap_blocks = 0, refcount_blocks = 0;
  uint64_t lsn = 0;
};

struct XfsAgi {
  uint32_t seqno = 0;
  uint32_t length = 0;
  uint32_t count = 0;      // allocated inodes in this AG
  uint32_t root = 0;       // inobt root block
  uint32_t level = 0;      // inobt levels
  uint32_t freecount = 0;  // free inodes
  uint32_t newino = 0;
  uint32_t dirino = 0;
  uint32_t unlinked[64] = {};
  uint32_t free_root = 0;  // finobt root
  uint32_t free_level = 0; // finobt levels
  uint32_t iblocks = 0;    // inobt blocks used (inobtcnt feature)
  uint32_t fblocks = 0;    // finobt blocks used
  uint64_t lsn = 0;
};

struct XfsAgfl {
  uint32_t seqno = 0;
  std::vector<uint32_t> entries;  // each is an AG block number
};

struct XfsAgHeaders {
  XfsAgf agf;
  XfsAgi agi;
};

constexpr uint32_t kAgflHeaderSize = 36;

// Byte offsets of the AG headers within an allocation group.
inline uint64_t AgfByteOffset(const XfsGeometry& geo) { return geo.sector_size; }
inline uint64_t AgiByteOffset(const XfsGeometry& geo) {
  return 2ull * geo.sector_size;
}
inline uint64_t AgflByteOffset(const XfsGeometry& geo) {
  return 3ull * geo.sector_size;
}
// Number of AGFL entries for a v5 filesystem (one sector, 36-byte header).
inline uint32_t AgflSize(const XfsGeometry& geo) {
  return (geo.sector_size - kAgflHeaderSize) / 4;
}

// Reads + verifies AGF/AGI/AGFL of one allocation group.
Result<XfsAgf> ReadAgf(BlockDevice& dev, const XfsGeometry& geo, uint32_t agno);
Result<XfsAgi> ReadAgi(BlockDevice& dev, const XfsGeometry& geo, uint32_t agno);
Result<XfsAgfl> ReadAgfl(BlockDevice& dev, const XfsGeometry& geo, uint32_t agno,
                         uint32_t flcount);
Result<XfsAgHeaders> ReadAgHeaders(BlockDevice& dev, const XfsGeometry& geo,
                                   uint32_t agno);

}  // namespace xfs
