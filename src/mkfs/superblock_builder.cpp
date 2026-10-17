#include <cstring>

#include "mkfs/mkfs_internal.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/ondisk.h"

namespace xfs::mkfs::internal {
namespace {

namespace sb = ondisk::sb;

// v5 versionnum/features2 bits written by mkfs for our fixed profile.
constexpr uint16_t kVersionNlinkBit = 0x0020;
constexpr uint16_t kVersionAlignBit = 0x0080;
constexpr uint16_t kVersionLogV2Bit = 0x0400;
constexpr uint16_t kVersionSectorBit = 0x0800;
constexpr uint16_t kVersionExtflgBit = 0x1000;
constexpr uint16_t kVersionDirV2Bit = 0x2000;
constexpr uint16_t kVersionMoreBitsBit = 0x8000;
constexpr uint32_t kFeat2LazySbCount = 0x00000002;
constexpr uint32_t kFeat2Attr2 = 0x00000008;
constexpr uint32_t kFeat2Projid32 = 0x00000080;
constexpr uint32_t kFeat2Crc = 0x00000100;

}  // namespace

void BuildSuperblockImage(const FormatGeometry& geo, std::vector<uint8_t>* buf) {
  buf->assign(geo.sector_size, 0);
  uint8_t* p = buf->data();

  PutBE32(p, sb::kMagic, ondisk::kSbMagic);
  PutBE32(p, sb::kBlocksize, geo.block_size);
  PutBE64(p, sb::kDblocks, geo.dblocks);
  PutBE64(p, sb::kRblocks, 0);
  PutBE64(p, sb::kRextents, 0);
  std::memcpy(p + sb::kUuid, geo.uuid.data(), 16);
  PutBE64(p, sb::kLogstart, geo.logstart);
  PutBE64(p, sb::kRootino, geo.rootino);
  // No realtime device: mkfs writes NULLFSINO for both rt inode fields.
  PutBE64(p, sb::kRbmino, 0xffffffffffffffffULL);
  PutBE64(p, sb::kRsumino, 0xffffffffffffffffULL);
  PutBE32(p, sb::kRextsize, 0);
  PutBE32(p, sb::kAgblocks, geo.agsize);
  PutBE32(p, sb::kAgcount, geo.agcount);
  PutBE32(p, sb::kRbmblocks, 0);
  PutBE32(p, sb::kLogblocks, geo.logblocks);

  uint16_t versionnum = kVersionNlinkBit | kVersionAlignBit |
                        kVersionLogV2Bit | kVersionExtflgBit |
                        kVersionDirV2Bit | kVersionMoreBitsBit | 5;
  if (geo.sector_size > 512) versionnum |= kVersionSectorBit;
  PutBE16(p, sb::kVersionnum, versionnum);

  PutBE16(p, sb::kSectsize, static_cast<uint16_t>(geo.sector_size));
  PutBE16(p, sb::kInodesize, static_cast<uint16_t>(geo.inode_size));
  PutBE16(p, sb::kInopblock, static_cast<uint16_t>(geo.block_size / geo.inode_size));
  std::memcpy(p + sb::kFname, geo.label.data(), geo.label.size());  // zero padded

  p[sb::kBlocklog] = geo.blocklog;
  p[sb::kSectlog] = geo.sectlog;
  p[sb::kInodelog] = geo.inodelog;
  p[sb::kInopblog] = geo.inopblog;
  p[sb::kAgblklog] = geo.agblklog;
  p[sb::kRextslog] = 0;
  p[sb::kInprogress] = 0;
  p[sb::kImaxPct] = static_cast<uint8_t>(geo.imax_pct);

  // One inode chunk (64 inodes), one of which is the allocated root inode.
  PutBE64(p, sb::kIcount, 64);
  PutBE64(p, sb::kIfree, 63);
  PutBE64(p, sb::kFdblocks, geo.sb_fdblocks);
  PutBE64(p, sb::kFrextents, 0);

  PutBE64(p, sb::kUquotino, 0);
  PutBE64(p, sb::kGquotino, 0);
  PutBE16(p, sb::kQflags, 0);
  p[sb::kFlags] = 0;
  p[sb::kSharedVn] = 0;
  PutBE32(p, sb::kInoalignmt, geo.ino_align);
  PutBE32(p, sb::kUnit, 0);
  PutBE32(p, sb::kWidth, 0);
  p[sb::kDirblklog] = 0;
  if (geo.sector_size > 512) {
    p[sb::kLogsectlog] = geo.sectlog;
    PutBE16(p, sb::kLogsectsize, static_cast<uint16_t>(geo.sector_size));
  }
  PutBE32(p, sb::kLogsunit, 0);

  uint32_t features2 =
      kFeat2LazySbCount | kFeat2Attr2 | kFeat2Projid32 | kFeat2Crc;
  PutBE32(p, sb::kFeatures2, features2);
  PutBE32(p, sb::kBadFeatures2, features2);
  PutBE32(p, sb::kFeaturesCompat, 0);
  PutBE32(p, sb::kFeaturesRoCompat, 0);
  PutBE32(p, sb::kFeaturesIncompat, ondisk::kIncompatFtype);
  PutBE32(p, sb::kFeaturesLogIncompat, 0);
  PutBE32(p, sb::kSpinoAlign, 0);
  PutBE64(p, sb::kPquotino, 0);
  PutBE64(p, sb::kLsn, 0);
  std::memcpy(p + sb::kMetaUuid, geo.meta_uuid.data(), 16);

  UpdateXfsChecksum(p, geo.sector_size, sb::kCrc);
}

}  // namespace xfs::mkfs::internal
