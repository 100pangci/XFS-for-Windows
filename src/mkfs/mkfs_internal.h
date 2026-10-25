// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - formatter internals (shared between mkfs modules).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/formatter.h"
#include "xfs/superblock.h"

namespace xfs::mkfs::internal {

// Final on-disk geometry of the filesystem being formatted.
struct FormatGeometry {
  uint32_t block_size = 4096;
  uint32_t sector_size = 512;
  uint32_t inode_size = 512;
  uint8_t blocklog = 12;
  uint8_t sectlog = 9;
  uint8_t inodelog = 9;
  uint8_t inopblog = 3;
  uint8_t agblklog = 0;
  uint8_t agno_log = 0;
  uint8_t agino_log = 0;

  uint64_t dblocks = 0;    // total data blocks (device size / block size)
  uint32_t agcount = 0;
  uint32_t agsize = 0;     // size of all AGs except possibly the last

  uint32_t prealloc_blocks = 0;  // per-AG blocks [0, prealloc): hdrs + roots
  uint32_t min_freelist = 0;     // AGFL target length (entries)
  uint32_t bno_block = 0;        // AG-relative block numbers
  uint32_t cnt_block = 0;
  uint32_t ibt_block = 0;
  uint32_t fibt_block = 0;
  uint32_t rmap_block = 0;
  uint32_t refcount_block = 0;
  uint32_t agfl_block = 0;

  bool finobt = false;
  bool inobtcount = false;
  bool sparse_inodes = false;
  bool bigtime = false;
  bool nrext64 = false;
  bool rmapbt = false;
  bool reflink = false;

  uint64_t logstart = 0;   // absolute fsb
  uint32_t logblocks = 0;
  uint32_t log_agno = 0;

  uint32_t ino_align = 0;    // sb_inoalignmt (blocks)
  uint32_t spino_align = 0;  // sb_spino_align (0 without sparse inodes)
  uint32_t chunk_blocks = 0; // inode chunk size in blocks
  uint64_t first_chunk_fsb = 0;  // absolute fsb of the AG0 inode chunk
  uint32_t rootino_agino = 0;    // agino of the root inode within AG0
  uint64_t rootino = 0;

  uint32_t imax_pct = 25;

  uint64_t sb_fdblocks = 0;  // final sum of per-AG free blocks

  Uuid uuid{};
  Uuid meta_uuid{};
  std::string label;

  uint64_t AgFirstFsb(uint32_t agno) const {
    return static_cast<uint64_t>(agno) << agblklog;
  }
  uint64_t AgByteOffset(uint32_t agno) const {
    return static_cast<uint64_t>(agno) * agsize << blocklog;
  }
  // fsb numbers encode the AG in the high bits; byte offsets are contiguous.
  uint64_t FsbToByte(uint64_t fsb) const {
    const uint32_t agno = static_cast<uint32_t>(fsb >> agblklog);
    const uint32_t agbno = static_cast<uint32_t>(fsb & ((1ULL << agblklog) - 1));
    return (static_cast<uint64_t>(agno) * agsize + agbno) << blocklog;
  }

  // Free-space state of one allocation group after formatting.
  struct FreeExtent {
    uint32_t start = 0;
    uint32_t count = 0;
  };
  struct AgState {
    uint32_t agno = 0;
    uint32_t agsize = 0;
    std::vector<FreeExtent> free_extents;
    std::vector<uint32_t> agfl;  // AGFL entries (agbno)
    bool has_inodes = false;     // AG0 only: inode chunk present
    uint32_t chunk_agbno = 0;
    uint32_t chunk_blocks = 0;
  };
  std::vector<AgState> ags;
};

// Computes the complete geometry; fails with a clear error when the device is
// too small for this build's fixed profile.
Status ComputeFormatGeometry(const FormatOptions& options, uint64_t device_size,
                             uint32_t device_sector_size,
                             FormatGeometry* out);

// Builds a full superblock sector image (crc included). The same image is
// written to every AG (secondaries are exact copies).
void BuildSuperblockImage(const FormatGeometry& geo, std::vector<uint8_t>* buf);

// AG header images (one sector each, crc included).
void BuildAgfImage(const FormatGeometry& geo, const FormatGeometry::AgState& ag,
                   std::vector<uint8_t>* buf);
void BuildAgiImage(const FormatGeometry& geo, const FormatGeometry::AgState& ag,
                   std::vector<uint8_t>* buf);
void BuildAgflImage(const FormatGeometry& geo, const FormatGeometry::AgState& ag,
                    std::vector<uint8_t>* buf);

// Free-space btree root blocks (one fs block each).
void BuildBnobtRoot(const FormatGeometry& geo,
                    const FormatGeometry::AgState& ag,
                    std::vector<uint8_t>* buf);
void BuildCntbtRoot(const FormatGeometry& geo,
                    const FormatGeometry::AgState& ag,
                    std::vector<uint8_t>* buf);
// Inode btree root (empty for AGs without inodes; one record for AG0).
void BuildInobtRoot(const FormatGeometry& geo,
                    const FormatGeometry::AgState& ag,
                    std::vector<uint8_t>* buf);
void BuildFinobtRoot(const FormatGeometry& geo,
                     const FormatGeometry::AgState& ag,
                     std::vector<uint8_t>* buf);
// Reverse-mapping btree root (single leaf; the formatter's AG metadata fits
// comfortably in one block).
void BuildRmapRoot(const FormatGeometry& geo,
                   const FormatGeometry::AgState& ag,
                   std::vector<uint8_t>* buf);
// Reference-count btree root (empty single leaf; a freshly formatted
// filesystem has no shared extents or CoW staging records).
void BuildRefcountRoot(const FormatGeometry& geo,
                       const FormatGeometry::AgState& ag,
                       std::vector<uint8_t>* buf);

// Writes (and returns) the inode chunk contents for AG0: 64 v3 inode cores
// with the root inode fully populated. `buf` receives chunk_blocks * block_size
// bytes.
void BuildInodeChunk(const FormatGeometry& geo, uint32_t gen,
                     std::vector<uint8_t>* buf);

// Builds the clean-log record (record header + unmount record, 1024 bytes)
// that is written at the start of an otherwise zeroed internal log. Uses the
// same record writer as the read-write path.
Result<std::vector<uint8_t>> BuildLogRecord(const FormatGeometry& geo);

// Root inode number calculation (matches xfs_ialloc_calc_rootino).
uint64_t CalcRootIno(const FormatGeometry& geo, uint32_t first_chunk_agbno);

}  // namespace xfs::mkfs::internal
