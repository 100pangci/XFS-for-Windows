#include "mkfs/mkfs_internal.h"

#include <algorithm>
#include <ctime>

#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"

namespace xfs::mkfs::internal {
namespace {

// libxfs constants (xfs_fs.h / xfs_multidisk.h / xfs_alloc.c).
constexpr uint64_t kMinAgBytes = 1ULL << 24;   // 16 MiB
constexpr uint64_t kMaxAgBytes = 1ULL << 40;   // 1 TiB
constexpr uint32_t kMinAgBlocks = 64;
constexpr uint32_t kMinLogBlocks = 512;
constexpr uint64_t kMaxLogBlocks = 1024 * 1024;
constexpr uint64_t kMaxLogBytes = 2ULL * 1024 * 1024 * 1024;
constexpr uint64_t kMinRealisticLogBytes = 64ULL * 1024 * 1024;  // v5 minimum
constexpr uint32_t kMultidiskAgLog = 5;
constexpr uint32_t kNoMultidiskAgLog = 2;
constexpr uint32_t kInodesPerChunk = 64;
constexpr uint32_t kBigClusterBytes = 8192;

uint32_t HighBit32(uint32_t v) {
  uint32_t r = 0;
  while (v >>= 1) ++r;
  return r;
}

// calc_default_ag_geometry (libxfs/topology.c) with multidisk = 0.
void CalcDefaultAgGeometry(uint8_t blocklog, uint64_t dblocks, uint64_t* agsize,
                           uint64_t* agcount) {
  auto terabytes = [&](uint64_t count) {
    return count << (40 - blocklog);
  };
  auto megabytes = [&](uint64_t count) {
    return count << (20 - blocklog);
  };
  uint64_t max_ag_blocks = (kMaxAgBytes - 1) >> blocklog;

  uint64_t blocks = 0;
  int shift = 0;
  if (dblocks >= terabytes(32)) {
    blocks = max_ag_blocks;
  } else if (dblocks >= terabytes(4)) {
    blocks = max_ag_blocks;
  } else if (dblocks >= megabytes(128)) {
    shift = kNoMultidiskAgLog;
    goto calc_blocks;
  } else {
    shift = kMultidiskAgLog;
    if (dblocks <= (512ULL << (30 - blocklog))) shift--;
    if (dblocks <= (8ULL << (30 - blocklog))) shift--;
    if (dblocks < megabytes(128)) shift--;
    if (dblocks < megabytes(64)) shift--;
    if (dblocks < megabytes(32)) shift--;
    goto calc_blocks;
  }
  goto done;

calc_blocks:
  blocks = dblocks >> shift;
  if (dblocks & ((1ULL << shift) - 1)) {
    if (blocks < max_ag_blocks) blocks++;
  }
done:
  *agsize = blocks;
  *agcount = dblocks / blocks + (dblocks % blocks != 0);
}

}  // namespace

Status ComputeFormatGeometry(const FormatOptions& options, uint64_t device_size,
                             uint32_t device_sector_size,
                             FormatGeometry* out) {
  FormatGeometry geo;

  // Sector size: device logical sector (512..4096).
  uint32_t sector = device_sector_size;
  if (sector < 512) sector = 512;
  if (sector > 4096) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "device logical sector size above 4096 is not supported "
                   "by this build")
        .Actual(sector);
  }
  if (options.sector_size) {
    if (options.sector_size != sector) {
      return XFS_ERR(XfsError::kUnsupportedGeometry,
                     "explicit sector size does not match the device")
          .Expected(sector)
          .Actual(options.sector_size);
    }
  }
  geo.sector_size = sector;

  uint32_t block_size = options.block_size ? options.block_size : 4096;
  if (block_size < sector) block_size = sector;
  if (block_size < 1024) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "v5 filesystems need at least 1 KiB blocks")
        .Actual(block_size);
  }
  if (block_size > 65536 || !IsPowerOfTwo(block_size)) {
    return XFS_ERR(XfsError::kUnsupportedGeometry, "invalid block size")
        .Actual(block_size);
  }
  geo.block_size = block_size;
  geo.blocklog = static_cast<uint8_t>(Log2(block_size));
  geo.sectlog = static_cast<uint8_t>(Log2(sector));

  uint32_t inode_size = options.inode_size ? options.inode_size : 512;
  if (inode_size != 256 && inode_size != 512 && inode_size != 1024 &&
      inode_size != 2048) {
    return XFS_ERR(XfsError::kUnsupportedGeometry, "invalid inode size")
        .Actual(inode_size);
  }
  if (inode_size * 2 > block_size) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "inode size must not exceed half the block size")
        .Actual(inode_size);
  }
  geo.inode_size = inode_size;
  geo.inodelog = static_cast<uint8_t>(Log2(inode_size));
  geo.inopblog = static_cast<uint8_t>(geo.blocklog - geo.inodelog);

  // Device block count.
  uint64_t dblocks = device_size >> geo.blocklog;
  if (dblocks == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "device is too small")
        .Actual(device_size);
  }
  geo.dblocks = dblocks;

  // AG layout.
  uint64_t agsize = 0, agcount = 0;
  CalcDefaultAgGeometry(geo.blocklog, dblocks, &agsize, &agcount);
  if (agsize * geo.block_size < kMinAgBytes) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "allocation group size would be below 16 MiB; the device "
                   "is too small for this build's formatter")
        .Actual(agsize * geo.block_size);
  }
  if (agsize > ((kMaxAgBytes - 1) >> geo.blocklog)) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "allocation group size exceeds the maximum")
        .Actual(agsize);
  }
  for (uint64_t agno = 0; agno < agcount; ++agno) {
    uint64_t first = agno * agsize;
    uint64_t remaining = dblocks - first;
    uint64_t this_ag = std::min<uint64_t>(remaining, agsize);
    if (this_ag < kMinAgBlocks) {
      return XFS_ERR(XfsError::kUnsupportedGeometry,
                     "last allocation group would be too small")
          .Ag(static_cast<uint32_t>(agno))
          .Actual(this_ag);
    }
  }
  geo.agsize = static_cast<uint32_t>(agsize);
  geo.agcount = static_cast<uint32_t>(agcount);
  geo.agblklog = static_cast<uint8_t>(HighBit32(geo.agsize - 1) + 1);
  geo.agno_log = static_cast<uint8_t>(agcount <= 1
                                          ? 0
                                          : HighBit32(geo.agcount - 1) + 1);
  geo.agino_log = static_cast<uint8_t>(geo.agblklog + geo.inopblog);

  // Pre-allocated per-AG metadata blocks: block 0 (sb [+ agf/agi/agfl]) then
  // bno root, cnt root and inobt root.
  uint32_t agfl_block =
      (3 * geo.sector_size) >> geo.blocklog;  // byte 3*sectors -> block
  geo.agfl_block = agfl_block;
  geo.bno_block = agfl_block + 1;
  geo.cnt_block = agfl_block + 2;
  geo.ibt_block = agfl_block + 3;
  geo.prealloc_blocks = geo.ibt_block + 1;

  // Minimum AGFL length for a 1-level bno+cnt btree pair:
  // per tree: min(level+1, maxlevels)*2 - 2 = 2; two trees -> 4.
  geo.min_freelist = 4;

  // Inode chunk geometry.
  geo.ino_align = std::max<uint32_t>(1, (kBigClusterBytes *
                                          (geo.inode_size / 256)) >>
                                             geo.blocklog);
  geo.chunk_blocks =
      static_cast<uint32_t>((kInodesPerChunk * geo.inode_size +
                             geo.block_size - 1) >>
                            geo.blocklog);
  if (geo.chunk_blocks == 0) geo.chunk_blocks = 1;

  // Internal log: middle AG, immediately after the pre-allocated metadata.
  uint32_t log_agno = geo.agcount / 2;
  uint32_t this_agsize = geo.agsize;
  if (log_agno == geo.agcount - 1) {
    this_agsize = static_cast<uint32_t>(dblocks - uint64_t(log_agno) * agsize);
  }
  geo.log_agno = log_agno;

  // Sizing (mkfs without explicit -l size, no concurrency tuning).
  uint64_t logblocks = (dblocks << geo.blocklog) / 2048 >> geo.blocklog;
  uint64_t min_logblocks =
      std::max<uint64_t>(kMinLogBlocks, kMinRealisticLogBytes >> geo.blocklog);
  logblocks = std::max(logblocks, min_logblocks);
  uint64_t max_fit = this_agsize - geo.prealloc_blocks;
  uint64_t max_logblocks = std::min<uint64_t>(kMaxLogBlocks, max_fit);
  if ((logblocks << geo.blocklog) > kMaxLogBytes) {
    logblocks = kMaxLogBytes >> geo.blocklog;
  }
  if (logblocks > max_logblocks) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "device is too small for the minimum 64 MiB internal log "
                   "of a v5 filesystem created by this build; use a larger "
                   "device (or mkfs.xfs on Linux for very small filesystems)")
        .Expected(max_logblocks)
        .Actual(logblocks);
  }
  if (logblocks < min_logblocks) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "internal log would be below the v5 minimum")
        .Actual(logblocks);
  }
  geo.logblocks = static_cast<uint32_t>(logblocks);
  geo.logstart = (static_cast<uint64_t>(log_agno) << geo.agblklog) +
                 geo.prealloc_blocks;

  // Inode chunk placement (matches xfs_ialloc_calc_rootino for AG0).
  uint64_t first_bno = geo.prealloc_blocks + geo.min_freelist;
  if (log_agno == 0) first_bno += geo.logblocks;
  first_bno = RoundUp64(first_bno, geo.ino_align);
  geo.first_chunk_fsb = (0ULL << geo.agblklog) + first_bno;
  geo.rootino_agino = static_cast<uint32_t>(first_bno << geo.inopblog);
  geo.rootino = static_cast<uint64_t>(geo.rootino_agino);

  geo.imax_pct = 25;
  geo.uuid = options.uuid;
  if (IsNullUuid(geo.uuid)) {
    // Deterministic-ish fallback: derive from device size+time so repeated
    // runs do not collide. Real callers pass a random uuid.
    geo.uuid = {};
    uint64_t seed = device_size ^ (static_cast<uint64_t>(std::time(nullptr)) << 20);
    for (int i = 0; i < 16; ++i) {
      seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
      geo.uuid[i] = static_cast<uint8_t>(seed >> 56);
    }
    geo.uuid[6] = static_cast<uint8_t>((geo.uuid[6] & 0x0F) | 0x40);
    geo.uuid[8] = static_cast<uint8_t>((geo.uuid[8] & 0x3F) | 0x80);
  }
  geo.meta_uuid = geo.uuid;
  geo.label = options.label.substr(0, 12);

  // Per-AG final state.
  uint64_t total_free = 0;
  geo.ags.clear();
  for (uint32_t agno = 0; agno < geo.agcount; ++agno) {
    FormatGeometry::AgState ag;
    ag.agno = agno;
    uint64_t first = uint64_t(agno) * geo.agsize;
    uint64_t remaining = dblocks - first;
    ag.agsize = static_cast<uint32_t>(std::min<uint64_t>(remaining, geo.agsize));

    uint32_t free_start = geo.prealloc_blocks;
    if (agno == geo.log_agno) free_start += geo.logblocks;
    // AGFL blocks are taken from the start of the free extent.
    for (uint32_t i = 0; i < geo.min_freelist; ++i) {
      ag.agfl.push_back(free_start + i);
    }
    free_start += geo.min_freelist;

    if (agno == 0) {
      ag.has_inodes = true;
      ag.chunk_agbno = static_cast<uint32_t>(RoundUp64(
          free_start, geo.ino_align));
      ag.chunk_blocks = geo.chunk_blocks;
      // The alignment gap (if any) stays free; the chunk splits the free
      // space into up to two extents.
      if (ag.chunk_agbno > free_start) {
        ag.free_extents.push_back({free_start, ag.chunk_agbno - free_start});
      }
      uint32_t after = ag.chunk_agbno + ag.chunk_blocks;
      if (after < ag.agsize) {
        ag.free_extents.push_back({after, ag.agsize - after});
      }
    } else if (free_start < ag.agsize) {
      ag.free_extents.push_back({free_start, ag.agsize - free_start});
    }
    for (const auto& extent : ag.free_extents) {
      total_free += extent.count;
    }
    geo.ags.push_back(std::move(ag));
  }

  geo.sb_fdblocks = total_free;
  *out = std::move(geo);
  return OkStatus();
}

}  // namespace xfs::mkfs::internal
