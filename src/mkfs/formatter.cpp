// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/formatter.h"

#include <algorithm>
#include <cstring>

#include "mkfs/mkfs_internal.h"
#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"

namespace xfs::mkfs {

// (helper aliases)
using internal::ComputeFormatGeometry;
using internal::FormatGeometry;

Result<FormatReport> Format(BlockDevice& device, const FormatOptions& options) {
  if (!device.Info().writable) {
    return XFS_ERR(XfsError::kReadOnlyVolume,
                   "block device was opened read-only")
        .Metadata("formatter");
  }

  internal::FormatGeometry geo;
  Status s = internal::ComputeFormatGeometry(options, device.Size(),
                                             device.SectorSize(), &geo);
  if (!s.ok()) return s.error();

  if (!options.quiet) {
    XFS_MKFS_INFO("formatting " << device.Info().description);
    XFS_MKFS_INFO("blocksize=" << geo.block_size
                               << " sectorsize=" << geo.sector_size
                               << " inodesize=" << geo.inode_size);
    XFS_MKFS_INFO("dblocks=" << geo.dblocks << " agcount=" << geo.agcount
                             << " agsize=" << geo.agsize);
    XFS_MKFS_INFO("logstart=" << geo.logstart
                              << " logblocks=" << geo.logblocks
                              << " (AG" << geo.log_agno << ")");
    XFS_MKFS_INFO("uuid=" << FormatUuid(geo.uuid)
                          << " rootino=" << geo.rootino);
  }

  // Deterministic non-zero inode generation number derived from the uuid.
  uint32_t gen = 0;
  for (int i = 0; i < 16; ++i) {
    gen = gen * 31 + geo.uuid[i];
  }
  if (gen == 0) gen = 1;

  const uint64_t device_size = device.Size();

  // 1. Whack the first and last 64 KiB (obliterates old filesystem
  //    signatures, like mkfs does).
  {
    constexpr size_t kWhack = 64 * 1024;
    std::vector<uint8_t> zeros(std::min<uint64_t>(kWhack, device_size), 0);
    if (!zeros.empty()) {
      if (!device.Write(0, zeros.data(), zeros.size())) {
        return XFS_ERR(XfsError::kIoError, "failed to zero start of device")
            .Metadata("formatter");
      }
      if (device_size > zeros.size()) {
        if (!device.Write(device_size - zeros.size(), zeros.data(),
                          zeros.size())) {
          return XFS_ERR(XfsError::kIoError,
                         "failed to zero end of device")
              .Metadata("formatter");
        }
      }
    }
  }

  // 2. Per-AG headers and btree roots.
  std::vector<uint8_t> sb_image;
  internal::BuildSuperblockImage(geo, &sb_image);
  std::vector<uint8_t> agf_image, agi_image, agfl_image;
  std::vector<uint8_t> block_image;

  for (const auto& ag : geo.ags) {
    const uint64_t ag_byte = geo.AgByteOffset(ag.agno);

    // Zero the pre-allocated header/root area.
    std::vector<uint8_t> hdr_zeros(
        static_cast<size_t>(geo.prealloc_blocks) * geo.block_size, 0);
    if (!device.Write(ag_byte, hdr_zeros.data(), hdr_zeros.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to zero AG header area")
          .Ag(ag.agno)
          .Metadata("formatter");
    }

    // Superblock (primary AG0 and secondaries alike).
    if (!device.Write(ag_byte, sb_image.data(), sb_image.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to write superblock")
          .Ag(ag.agno)
          .Metadata("formatter");
    }

    internal::BuildAgfImage(geo, ag, &agf_image);
    if (!device.Write(ag_byte + geo.sector_size, agf_image.data(),
                      agf_image.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to write AGF")
          .Ag(ag.agno)
          .Metadata("formatter");
    }
    internal::BuildAgiImage(geo, ag, &agi_image);
    if (!device.Write(ag_byte + 2ull * geo.sector_size, agi_image.data(),
                      agi_image.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to write AGI")
          .Ag(ag.agno)
          .Metadata("formatter");
    }
    internal::BuildAgflImage(geo, ag, &agfl_image);
    if (!device.Write(ag_byte + 3ull * geo.sector_size, agfl_image.data(),
                      agfl_image.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to write AGFL")
          .Ag(ag.agno)
          .Metadata("formatter");
    }
    internal::BuildBnobtRoot(geo, ag, &block_image);
    if (!device.Write(ag_byte + uint64_t(geo.bno_block) * geo.block_size,
                      block_image.data(), block_image.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to write bnobt root")
          .Ag(ag.agno)
          .Metadata("formatter");
    }
    internal::BuildCntbtRoot(geo, ag, &block_image);
    if (!device.Write(ag_byte + uint64_t(geo.cnt_block) * geo.block_size,
                      block_image.data(), block_image.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to write cntbt root")
          .Ag(ag.agno)
          .Metadata("formatter");
    }
    internal::BuildInobtRoot(geo, ag, &block_image);
    if (!device.Write(ag_byte + uint64_t(geo.ibt_block) * geo.block_size,
                      block_image.data(), block_image.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to write inobt root")
          .Ag(ag.agno)
          .Metadata("formatter");
    }
  }

  // 3. Inode chunk with the root inode (AG0).
  {
    std::vector<uint8_t> chunk;
    internal::BuildInodeChunk(geo, gen, &chunk);
    uint64_t offset = geo.FsbToByte(geo.first_chunk_fsb);
    if (!device.Write(offset, chunk.data(), chunk.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to write inode chunk")
          .Metadata("formatter")
          .Offset(offset);
    }
  }

  // 4. The internal log: zero the whole region in chunks, then write the
  //    clean unmount record at the start.
  {
    constexpr size_t kZeroChunk = 4 * 1024 * 1024;
    std::vector<uint8_t> zeros(kZeroChunk, 0);
    uint64_t offset = geo.FsbToByte(geo.logstart);
    uint64_t remaining = static_cast<uint64_t>(geo.logblocks) << geo.blocklog;
    while (remaining > 0) {
      size_t chunk =
          static_cast<size_t>(std::min<uint64_t>(kZeroChunk, remaining));
      if (!device.Write(offset, zeros.data(), chunk)) {
        return XFS_ERR(XfsError::kIoError, "failed to zero internal log")
            .Metadata("formatter")
            .Offset(offset);
      }
      offset += chunk;
      remaining -= chunk;
    }
    std::vector<uint8_t> record;
    internal::BuildLogRecord(geo, &record);
    if (!device.Write(geo.FsbToByte(geo.logstart), record.data(),
                      record.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to write log record")
          .Metadata("formatter")
          .Offset(geo.FsbToByte(geo.logstart));
    }
  }

  if (!device.Flush()) {
    return XFS_ERR(XfsError::kIoError, "failed to flush formatted device")
        .Metadata("formatter");
  }

  FormatReport report;
  report.block_size = geo.block_size;
  report.sector_size = geo.sector_size;
  report.inode_size = geo.inode_size;
  report.data_blocks = geo.dblocks;
  report.ag_count = geo.agcount;
  report.ag_size_blocks = geo.agsize;
  report.log_start = geo.logstart;
  report.log_blocks = geo.logblocks;
  report.root_ino = geo.rootino;
  report.inode_chunks = 1;
  report.uuid = geo.uuid;
  report.summary =
      "XFS v5, crc=1, " + std::to_string(geo.agcount) + " AGs x " +
      std::to_string(geo.agsize) + " blocks, inode size " +
      std::to_string(geo.inode_size) + ", log " +
      std::to_string(geo.logblocks) + " blocks";
  return report;
}

}  // namespace xfs::mkfs
