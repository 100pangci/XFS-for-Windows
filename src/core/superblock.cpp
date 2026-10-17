// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/superblock.h"

#include <cstring>
#include <limits>
#include <vector>

#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/verifier.h"

namespace xfs {
namespace {

namespace sb = ondisk::sb;

using ondisk::kAgfMagic;
using ondisk::kAgflMagic;

// Geometry limits (libxfs xfs_types.h / xfs_fs.h).
constexpr uint32_t kMinBlockSize = 512;
constexpr uint32_t kMaxBlockSize = 65536;
constexpr uint32_t kMinCrcBlockSize = 1024;  // v5 requires >= 1k blocks
constexpr int kMinBlockLog = 9;
constexpr int kMaxBlockLog = 16;
constexpr uint32_t kMinSectorSize = 512;
constexpr uint32_t kMaxSectorSize = 32768;
constexpr int kMinSectorLog = 9;
constexpr int kMaxSectorLog = 15;
constexpr uint32_t kMinInodeSize = 256;
constexpr uint32_t kMaxInodeSize = 2048;
constexpr int kMinInodeLog = 8;
constexpr int kMaxInodeLog = 11;
constexpr uint64_t kMinAgBytes = 1ULL << 24;   // 16 MB
constexpr uint64_t kMaxAgBytes = 1ULL << 40;   // 1 TB
constexpr uint32_t kMinAgBlocks = 64;
constexpr uint64_t kMaxLogBlocks = 1024 * 1024;
constexpr uint64_t kMaxLogBytes = 2ULL * 1024 * 1024 * 1024 - 10ULL * 1024 * 1024;
constexpr uint32_t kMaxRecordBlockSize = 256 * 1024;  // XLOG_MAX_RECORD_BSIZE

// versionnum bits
constexpr uint16_t kVersionNumbits = 0x000f;
constexpr uint16_t kVersion5 = 5;
constexpr uint16_t kVersionAllFBits = 0xfff0;
constexpr uint16_t kVersionSharedBit = 0x0200;
constexpr uint16_t kVersionNlinkBit = 0x0020;
constexpr uint16_t kVersionAlignBit = 0x0080;
constexpr uint16_t kVersionDalignBit = 0x0100;
constexpr uint16_t kVersionLogV2Bit = 0x0400;
constexpr uint16_t kVersionSectorBit = 0x0800;
constexpr uint16_t kVersionExtflgBit = 0x1000;
constexpr uint16_t kVersionDirV2Bit = 0x2000;
constexpr uint16_t kVersionBorgBit = 0x4000;   // asciici
constexpr uint16_t kVersionMoreBitsBit = 0x8000;
constexpr uint16_t kVersionOkBits =
    (kVersionNumbits | kVersionAllFBits) & ~kVersionSharedBit;

// features2 bits
constexpr uint32_t kFeat2LazySbCount = 0x00000002;
constexpr uint32_t kFeat2Attr2 = 0x00000008;
constexpr uint32_t kFeat2Projid32 = 0x00000080;
constexpr uint32_t kFeat2Crc = 0x00000100;
constexpr uint32_t kFeat2Ftype = 0x00000200;
constexpr uint32_t kFeat2OkBits =
    kFeat2LazySbCount | kFeat2Attr2 | kFeat2Projid32 | kFeat2Ftype;

// required v5 bits
constexpr uint16_t kV5VersionFlags = kVersionNlinkBit | kVersionAlignBit |
                                     kVersionLogV2Bit | kVersionExtflgBit |
                                     kVersionDirV2Bit | kVersionMoreBitsBit;
constexpr uint32_t kV5Feat2Flags = kFeat2LazySbCount | kFeat2Attr2 |
                                   kFeat2Projid32 | kFeat2Crc;

constexpr uint16_t kQflagsOquotaEnfd = 0x0010;  // other (grp/prj) quota
constexpr uint16_t kQflagsOquotaChkd = 0x0020;

uint32_t HighBit32(uint32_t v) {
  // index of the highest set bit; fls(v) - 1 (v == 0 is never passed here)
  uint32_t r = 0;
  while (v >>= 1) ++r;
  return r;
}

std::string Hex32(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%x", v);
  return buf;
}

}  // namespace

std::string FormatLabel(const XfsSuperblock& sb) { return sb.label.data(); }

Status ParseSuperblockBuffer(const uint8_t* buf, size_t len, bool verify_crc,
                             XfsSuperblock* out) {
  XfsSuperblock sb;
  if (len < sb::kSize) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "superblock buffer too small")
        .Metadata("superblock")
        .Actual(len);
  }

  sb.magic = GetBE32(buf, sb::kMagic);
  if (sb.magic != ondisk::kSbMagic) {
    return XFS_ERR(XfsError::kBadMagic,
                   "not an XFS filesystem (bad superblock magic)")
        .Metadata("superblock")
        .Offset(sb::kMagic)
        .Expected(ondisk::kSbMagic)
        .Actual(sb.magic);
  }

  sb.versionnum = GetBE16(buf, sb::kVersionnum);
  if ((sb.versionnum & kVersionNumbits) != kVersion5) {
    return XFS_ERR(XfsError::kNotV5,
                   "XFS v4 (and older) is not supported by this build; "
                   "only v5 (crc=1) filesystems can be read")
        .Metadata("superblock")
        .Offset(sb::kVersionnum)
        .Actual(sb.versionnum & kVersionNumbits);
  }

  sb.blocksize = GetBE32(buf, sb::kBlocksize);
  sb.dblocks = GetBE64(buf, sb::kDblocks);
  sb.rblocks = GetBE64(buf, sb::kRblocks);
  sb.rextents = GetBE64(buf, sb::kRextents);
  std::memcpy(sb.uuid.data(), buf + sb::kUuid, 16);
  sb.logstart = GetBE64(buf, sb::kLogstart);
  sb.rootino = GetBE64(buf, sb::kRootino);
  sb.rbmino = GetBE64(buf, sb::kRbmino);
  sb.rsumino = GetBE64(buf, sb::kRsumino);
  sb.rextsize = GetBE32(buf, sb::kRextsize);
  sb.agblocks = GetBE32(buf, sb::kAgblocks);
  sb.agcount = GetBE32(buf, sb::kAgcount);
  sb.rbmblocks = GetBE32(buf, sb::kRbmblocks);
  sb.logblocks = GetBE32(buf, sb::kLogblocks);
  sb.sectsize = GetBE16(buf, sb::kSectsize);
  sb.inodesize = GetBE16(buf, sb::kInodesize);
  sb.inopblock = GetBE16(buf, sb::kInopblock);
  std::memcpy(sb.label.data(), buf + sb::kFname, 12);
  sb.label[12] = '\0';
  sb.blocklog = GetU8(buf, sb::kBlocklog);
  sb.sectlog = GetU8(buf, sb::kSectlog);
  sb.inodelog = GetU8(buf, sb::kInodelog);
  sb.inopblog = GetU8(buf, sb::kInopblog);
  sb.agblklog = GetU8(buf, sb::kAgblklog);
  sb.rextslog = GetU8(buf, sb::kRextslog);
  sb.inprogress = GetU8(buf, sb::kInprogress);
  sb.imax_pct = GetU8(buf, sb::kImaxPct);
  sb.icount = GetBE64(buf, sb::kIcount);
  sb.ifree = GetBE64(buf, sb::kIfree);
  sb.fdblocks = GetBE64(buf, sb::kFdblocks);
  sb.frextents = GetBE64(buf, sb::kFrextents);
  sb.uquotino = GetBE64(buf, sb::kUquotino);
  sb.gquotino = GetBE64(buf, sb::kGquotino);
  sb.qflags = GetBE16(buf, sb::kQflags);
  sb.flags = GetU8(buf, sb::kFlags);
  sb.shared_vn = GetU8(buf, sb::kSharedVn);
  sb.inoalignmt = GetBE32(buf, sb::kInoalignmt);
  sb.unit = GetBE32(buf, sb::kUnit);
  sb.width = GetBE32(buf, sb::kWidth);
  sb.dirblklog = GetU8(buf, sb::kDirblklog);
  sb.logsectlog = GetU8(buf, sb::kLogsectlog);
  sb.logsectsize = GetBE16(buf, sb::kLogsectsize);
  sb.logsunit = GetBE32(buf, sb::kLogsunit);
  sb.features2 = GetBE32(buf, sb::kFeatures2);
  uint32_t bad_features2 = GetBE32(buf, sb::kBadFeatures2);
  sb.features_compat = GetBE32(buf, sb::kFeaturesCompat);
  sb.features_ro_compat = GetBE32(buf, sb::kFeaturesRoCompat);
  sb.features_incompat = GetBE32(buf, sb::kFeaturesIncompat);
  sb.features_log_incompat = GetBE32(buf, sb::kFeaturesLogIncompat);
  sb.spino_align = GetBE32(buf, sb::kSpinoAlign);
  sb.pquotino = GetBE64(buf, sb::kPquotino);
  sb.lsn = GetBE64(buf, sb::kLsn);
  std::memcpy(sb.meta_uuid.data(), buf + sb::kMetaUuid, 16);

  // ---- v4 feature-area validation (xfs_sb_validate_v5_features) ----
  if (sb.versionnum & ~kVersionOkBits) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "unknown v4-era version bits set in a v5 superblock")
        .Metadata("superblock")
        .Offset(sb::kVersionnum)
        .Actual(sb.versionnum & ~kVersionOkBits);
  }
  if (sb.features2 & ~(kFeat2OkBits | kFeat2Crc)) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "unknown features2 bits set in a v5 superblock")
        .Metadata("superblock")
        .Offset(sb::kFeatures2)
        .Actual(sb.features2 & ~(kFeat2OkBits | kFeat2Crc));
  }
  if ((sb.versionnum & kV5VersionFlags) != kV5VersionFlags) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "required v5 version bits missing")
        .Metadata("superblock")
        .Offset(sb::kVersionnum)
        .Expected(kV5VersionFlags)
        .Actual(sb.versionnum & kV5VersionFlags);
  }
  if ((sb.features2 & kV5Feat2Flags) != kV5Feat2Flags) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "required v5 features2 bits missing")
        .Metadata("superblock")
        .Offset(sb::kFeatures2)
        .Expected(kV5Feat2Flags)
        .Actual(sb.features2 & kV5Feat2Flags);
  }
  if (bad_features2 != sb.features2) {
    // Older kernels read/wrote features2 inconsistently; the kernel keeps
    // them in sync by ignoring bad_features2 and warns. Treat as corruption:
    // it means something wrote a superblock this build cannot trust.
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "bad_features2 does not match features2")
        .Metadata("superblock")
        .Offset(sb::kBadFeatures2)
        .Expected(sb.features2)
        .Actual(bad_features2);
  }

  // Feature classification.
  XfsFeatures& f = sb.features;
  f.asciici = (sb.versionnum & kVersionBorgBit) != 0;
  f.ftype = (sb.features_incompat & ondisk::kIncompatFtype) != 0;
  f.spinodes = (sb.features_incompat & ondisk::kIncompatSpinodes) != 0;
  f.meta_uuid = (sb.features_incompat & ondisk::kIncompatMetaUuid) != 0;
  f.bigtime = (sb.features_incompat & ondisk::kIncompatBigtime) != 0;
  f.nrext64 = (sb.features_incompat & ondisk::kIncompatNrext64) != 0;
  f.exchrange = (sb.features_incompat & ondisk::kIncompatExchrange) != 0;
  f.finobt = (sb.features_ro_compat & ondisk::kRoCompatFinobt) != 0;
  f.rmapbt = (sb.features_ro_compat & ondisk::kRoCompatRmapbt) != 0;
  f.reflink = (sb.features_ro_compat & ondisk::kRoCompatReflink) != 0;
  f.inobtcnt = (sb.features_ro_compat & ondisk::kRoCompatInobtcnt) != 0;
  f.compat_unknown = sb.features_compat;  // all compat bits are "unknown"
  f.ro_compat_unknown = sb.features_ro_compat & ~ondisk::kRoCompatKnown;
  f.incompat_unknown = sb.features_incompat & ~(ondisk::kIncompatFtype |
                                                ondisk::kIncompatSpinodes |
                                                ondisk::kIncompatMetaUuid |
                                                ondisk::kIncompatBigtime |
                                                ondisk::kIncompatNeedsRepair |
                                                ondisk::kIncompatNrext64 |
                                                ondisk::kIncompatExchrange |
                                                ondisk::kIncompatParent |
                                                ondisk::kIncompatMetadir |
                                                ondisk::kIncompatZoned |
                                                ondisk::kIncompatZoneGaps);

  // meta_uuid: with META_UUID the metadata carries sb_meta_uuid; otherwise it
  // carries sb_uuid and meta_uuid is all zero.
  if (f.meta_uuid) {
    sb.meta_uuid = sb.meta_uuid;  // as read
  } else {
    sb.meta_uuid = sb.uuid;
  }

  // ---- geometry sanity (xfs_validate_sb_common) ----
  if (sb.blocksize < kMinCrcBlockSize) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "block size too small for a v5 filesystem")
        .Metadata("superblock")
        .Actual(sb.blocksize);
  }
  if (sb.blocksize < kMinBlockSize || sb.blocksize > kMaxBlockSize ||
      sb.blocklog < kMinBlockLog || sb.blocklog > kMaxBlockLog ||
      sb.blocksize != (1u << sb.blocklog)) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "invalid block size/log")
        .Metadata("superblock")
        .Actual(sb.blocksize);
  }
  if (sb.sectsize < kMinSectorSize || sb.sectsize > kMaxSectorSize ||
      sb.sectlog < kMinSectorLog || sb.sectlog > kMaxSectorLog ||
      sb.sectsize != (1u << sb.sectlog)) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "invalid sector size/log")
        .Metadata("superblock")
        .Actual(sb.sectsize);
  }
  if (sb.agcount == 0) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "agcount is zero")
        .Metadata("superblock");
  }
  if ((sb.blocklog + sb.dirblklog) > kMaxBlockLog) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "directory block size exceeds maximum block size")
        .Metadata("superblock");
  }
  if (sb.inodesize < kMinInodeSize || sb.inodesize > kMaxInodeSize ||
      sb.inodelog < kMinInodeLog || sb.inodelog > kMaxInodeLog ||
      sb.inodesize != (1u << sb.inodelog)) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "invalid inode size/log")
        .Metadata("superblock")
        .Actual(sb.inodesize);
  }
  uint32_t expected_inopblock = sb.blocksize / sb.inodesize;
  if (sb.inopblock != expected_inopblock ||
      (sb.blocklog - sb.inodelog) != sb.inopblog) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "inodes-per-block does not match block/inode size")
        .Metadata("superblock")
        .Expected(expected_inopblock)
        .Actual(sb.inopblock);
  }
  if (sb.inodesize != 256 && sb.inodesize != 512 && sb.inodesize != 1024 &&
      sb.inodesize != 2048) {
    return XFS_ERR(XfsError::kUnsupportedGeometry, "unsupported inode size")
        .Metadata("superblock")
        .Actual(sb.inodesize);
  }
  if (sb.agblocks == 0) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "agblocks is zero")
        .Metadata("superblock");
  }
  {
    uint64_t ag_bytes = static_cast<uint64_t>(sb.agblocks) << sb.blocklog;
    if (ag_bytes < kMinAgBytes) {
      return XFS_ERR(XfsError::kCorruptSuperblock,
                     "allocation group smaller than 16MB")
          .Metadata("superblock")
          .Actual(ag_bytes);
    }
    if (ag_bytes > kMaxAgBytes) {
      return XFS_ERR(XfsError::kCorruptSuperblock,
                     "allocation group larger than 1TB")
          .Metadata("superblock")
          .Actual(ag_bytes);
    }
  }
  if (sb.agblklog != static_cast<uint8_t>(HighBit32(sb.agblocks - 1) + 1)) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "agblklog mismatch")
        .Metadata("superblock")
        .Expected(HighBit32(sb.agblocks - 1) + 1)
        .Actual(sb.agblklog);
  }
  {
    uint64_t agcount = sb.dblocks / sb.agblocks;
    if (sb.dblocks % sb.agblocks) agcount++;
    if (agcount == 0 || agcount != sb.agcount) {
      return XFS_ERR(XfsError::kCorruptSuperblock,
                     "agcount does not match dblocks/agblocks")
          .Metadata("superblock")
          .Expected(agcount)
          .Actual(sb.agcount);
    }
  }
  if (sb.dblocks == 0) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "dblocks is zero")
        .Metadata("superblock");
  }
  uint64_t max_dblocks =
      static_cast<uint64_t>(sb.agcount) * sb.agblocks;
  uint64_t min_dblocks =
      static_cast<uint64_t>(sb.agcount - 1) * sb.agblocks + kMinAgBlocks;
  if (sb.dblocks > max_dblocks || sb.dblocks < min_dblocks) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "dblocks outside valid range for agcount/agblocks")
        .Metadata("superblock")
        .Actual(sb.dblocks);
  }
  if (sb.imax_pct > 100) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "invalid imax_pct")
        .Metadata("superblock")
        .Actual(sb.imax_pct);
  }
  if (sb.shared_vn != 0) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "shared_vn is nonzero")
        .Metadata("superblock");
  }
  if (sb.qflags & (kQflagsOquotaEnfd | kQflagsOquotaChkd)) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "v5 superblock has v4 OQUOTA bits set")
        .Metadata("superblock")
        .Actual(sb.qflags);
  }

  // Sparse inode alignment rules.
  if (f.spinodes) {
    uint32_t align =
        (ondisk::kInodesPerChunk * sb.inodesize) >> sb.blocklog;
    if (sb.inoalignmt != align) {
      return XFS_ERR(XfsError::kCorruptSuperblock,
                     "inode chunk alignment must match chunk size with "
                     "sparse inodes")
          .Metadata("superblock")
          .Expected(align)
          .Actual(sb.inoalignmt);
    }
    if (sb.spino_align && (sb.spino_align > sb.inoalignmt ||
                           sb.inoalignmt % sb.spino_align != 0)) {
      return XFS_ERR(XfsError::kCorruptSuperblock,
                     "invalid sparse inode alignment")
          .Metadata("superblock")
          .Actual(sb.spino_align);
    }
  } else if (sb.spino_align != 0) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "spino_align must be zero without sparse inodes")
        .Metadata("superblock");
  }

  // Log geometry.
  if (sb.logblocks > kMaxLogBlocks) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "log too large (blocks)")
        .Metadata("superblock")
        .Actual(sb.logblocks);
  }
  if ((static_cast<uint64_t>(sb.logblocks) << sb.blocklog) > kMaxLogBytes) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "log too large (bytes)")
        .Metadata("superblock")
        .Actual(static_cast<uint64_t>(sb.logblocks) << sb.blocklog);
  }
  if (sb.versionnum & kVersionSectorBit) {
    if (sb.logsectsize != (1u << sb.logsectlog)) {
      return XFS_ERR(XfsError::kCorruptSuperblock,
                     "log sector size and log2 do not match")
          .Metadata("superblock");
    }
  } else if (sb.logsectsize != 0 || sb.logsectlog != 0) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "log sector size must be zero without the sector bit")
        .Metadata("superblock");
  }
  if (sb.logsunit > 1) {
    if (sb.logsunit % sb.blocksize != 0 ||
        sb.logsunit > kMaxRecordBlockSize) {
      return XFS_ERR(XfsError::kCorruptSuperblock, "invalid log stripe unit")
          .Metadata("superblock")
          .Actual(sb.logsunit);
    }
  }

  // Realtime geometry sanity (we refuse rt devices later, but the fields must
  // still be structurally sound). mkfs writes a nonzero sb_rextsize even when
  // there is no realtime device, and libxfs validates this unconditionally.
  {
    uint64_t rext_bytes = static_cast<uint64_t>(sb.rextsize) << sb.blocklog;
    constexpr uint64_t kMaxRtExtBytes = 1024ULL * 1024 * 1024;
    constexpr uint64_t kMinRtExtBytes = 4096;
    if (rext_bytes > kMaxRtExtBytes || rext_bytes < kMinRtExtBytes) {
      return XFS_ERR(XfsError::kCorruptSuperblock,
                     "realtime extent size out of range")
          .Metadata("superblock")
          .Actual(rext_bytes);
    }
  }

  // Stripe geometry (xfs_validate_stripe_geometry with may_repair=false).
  {
    bool has_dalign = (sb.versionnum & kVersionDalignBit) != 0;
    uint64_t sunit = static_cast<uint64_t>(sb.unit) << sb.blocklog;
    uint64_t swidth = static_cast<uint64_t>(sb.width) << sb.blocklog;
    bool bad = ((sb.unit != 0) != has_dalign);
    if (!bad) {
      if (swidth > std::numeric_limits<int32_t>::max()) bad = true;
      else if (sunit > swidth) bad = true;
      else if (sunit && !swidth) bad = true;
      else if (!sunit && swidth) bad = true;
      else if (sunit && swidth % sunit) bad = true;
    }
    if (bad) {
      return XFS_ERR(XfsError::kCorruptSuperblock,
                     "stripe alignment sanity check failed")
          .Metadata("superblock")
          .Actual(sunit)
          .Expected(swidth);
    }
  }

  // CRC: computed over the whole sector, crc field zeroed.
  if (verify_crc) {
    if (len < sb.sectsize) {
      return XFS_ERR(XfsError::kCorruptSuperblock,
                     "superblock buffer smaller than the sector size")
          .Metadata("superblock")
          .Expected(sb.sectsize)
          .Actual(len);
    }
    Status s = VerifyChecksum(buf, sb.sectsize, sb::kCrc, "superblock");
    if (!s.ok()) return s;
    sb.crc_verified = true;
  }

  *out = std::move(sb);
  return OkStatus();
}

Result<XfsSuperblock> ReadSuperblock(BlockDevice& dev, uint64_t byte_offset,
                                     bool verify_crc) {
  uint8_t first[512];
  if (!dev.Read(byte_offset, first, sizeof(first))) {
    return XFS_ERR(XfsError::kIoError, "failed to read superblock sector")
        .Offset(byte_offset)
        .Metadata("superblock");
  }
  uint32_t magic = GetBE32(first, sb::kMagic);
  if (magic != ondisk::kSbMagic) {
    return XFS_ERR(XfsError::kBadMagic,
                   "no XFS superblock at this location")
        .Metadata("superblock")
        .Offset(byte_offset)
        .Expected(ondisk::kSbMagic)
        .Actual(magic);
  }
  uint16_t sectsize = GetBE16(first, sb::kSectsize);
  if (sectsize < 512 || sectsize > 32768 || !IsPowerOfTwo(sectsize)) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "superblock sector size is invalid")
        .Metadata("superblock")
        .Actual(sectsize);
  }

  std::vector<uint8_t> buf(sectsize);
  if (!dev.Read(byte_offset, buf.data(), sectsize)) {
    return XFS_ERR(XfsError::kIoError, "failed to read full superblock")
        .Offset(byte_offset)
        .Metadata("superblock");
  }
  XfsSuperblock out;
  Status s = ParseSuperblockBuffer(buf.data(), buf.size(), verify_crc, &out);
  if (!s.ok()) return s.error();
  return out;
}

XfsGeometry MakeGeometry(const XfsSuperblock& sb) {
  XfsGeometry geo;
  geo.block_size = sb.blocksize;
  geo.sector_size = sb.sectsize;
  geo.inode_size = sb.inodesize;
  geo.inopblock = sb.inopblock;
  geo.block_log = sb.blocklog;
  geo.sector_log = sb.sectlog;
  geo.inode_log = sb.inodelog;
  geo.inopblog = sb.inopblog;
  geo.agblklog = sb.agblklog;
  geo.agcount = sb.agcount;
  geo.agblocks = sb.agblocks;
  geo.dblocks = sb.dblocks;
  geo.logstart = sb.logstart;
  geo.logblocks = sb.logblocks;
  geo.rootino = sb.rootino;
  geo.dir_blk_log = sb.dirblklog;
  geo.dir_block_size = sb.blocksize << sb.dirblklog;
  geo.uuid = sb.uuid;
  geo.meta_uuid = sb.meta_uuid;
  geo.features = sb.features;
  geo.ino_align = sb.inoalignmt;
  geo.agno_log = static_cast<uint8_t>(HighBit32(sb.agcount - 1) + 1);
  if (sb.agcount == 1) geo.agno_log = 0;
  geo.agino_log = static_cast<uint8_t>(sb.agblklog + sb.inopblog);
  return geo;
}

Status CheckMountable(const XfsSuperblock& sb) {
  if (sb.inprogress) {
    return XFS_ERR(XfsError::kNeedsRepair,
                   "filesystem creation was interrupted (sb_inprogress set); "
                   "refusing to mount")
        .Metadata("superblock");
  }
  if (sb.features_incompat & ondisk::kIncompatNeedsRepair) {
    return XFS_ERR(XfsError::kNeedsRepair,
                   "filesystem is marked as needing repair "
                   "(NEEDSREPAIR feature); refusing to mount")
        .Metadata("superblock");
  }
  if (sb.features.incompat_unknown) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "superblock has unknown incompatible features " +
                       Hex32(sb.features.incompat_unknown) +
                       "; this filesystem cannot be safely read")
        .Metadata("superblock")
        .Actual(sb.features.incompat_unknown);
  }
  if (sb.features_incompat & ondisk::kIncompatParent) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "PARENT (parent pointers) filesystems are not supported "
                   "by this build")
        .Metadata("superblock");
  }
  if (sb.features_incompat & ondisk::kIncompatMetadir) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "METADIR (metadata directories) filesystems are not "
                   "supported by this build")
        .Metadata("superblock");
  }
  if (sb.features_incompat & (ondisk::kIncompatZoned | ondisk::kIncompatZoneGaps)) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "zoned realtime filesystems are not supported by this "
                   "build")
        .Metadata("superblock");
  }
  if (sb.features.asciici) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "case-insensitive (asciici) directories are not supported "
                   "by this build")
        .Metadata("superblock");
  }
  if (sb.rblocks != 0 || sb.rextents != 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "realtime devices are not supported by this build")
        .Metadata("superblock");
  }
  if (sb.logstart == 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "external log devices are not supported by this build")
        .Metadata("superblock");
  }
  if (sb.logblocks == 0) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "internal log location is out of range")
        .Metadata("superblock")
        .Actual(sb.logstart);
  }
  {
    // logstart is an fsblock number (AG number in the high bits).
    uint32_t agno = static_cast<uint32_t>(sb.logstart >> sb.agblklog);
    uint32_t agbno = static_cast<uint32_t>(sb.logstart &
                                           ((1u << sb.agblklog) - 1));
    uint64_t ag_first = static_cast<uint64_t>(agno) * sb.agblocks;
    if (agno >= sb.agcount || ag_first >= sb.dblocks) {
      return XFS_ERR(XfsError::kCorruptSuperblock,
                     "internal log starts outside the data device")
          .Metadata("superblock")
          .Actual(sb.logstart);
    }
    uint64_t aglen = sb.dblocks - ag_first;
    if (aglen > sb.agblocks) aglen = sb.agblocks;
    if (agbno >= aglen || sb.logblocks > aglen - agbno) {
      return XFS_ERR(XfsError::kCorruptSuperblock,
                     "internal log extends past its allocation group")
          .Metadata("superblock")
          .Actual(sb.logstart);
    }
  }
  return OkStatus();
}

bool HasLogIncompatFeatures(const XfsSuperblock& sb) {
  return sb.features_log_incompat != 0;
}

}  // namespace xfs
