#include <cstring>
#include <ctime>

#include "mkfs/mkfs_internal.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/ondisk.h"

namespace xfs::mkfs::internal {
namespace {

namespace di = ondisk::inode;
namespace d = ondisk::dir;
namespace bt = ondisk::btree;

constexpr uint32_t kNullAgino = 0xffffffffu;
constexpr uint32_t kInodesPerChunk = 64;
constexpr uint64_t kBigClusterBytes = 8192;

// Writes the bare v3 inode core that xfs_ialloc_inode_init() leaves behind:
// magic, version, gen, next_unlinked, ino, uuid, crc; everything else zero.
void InitFreeInodeCore(const FormatGeometry& geo, uint8_t* core, uint64_t ino,
                       uint32_t gen) {
  PutBE16(core, di::kMagic, ondisk::kDinodeMagic);
  core[di::kVersion] = 3;
  PutBE32(core, di::kGen, gen);
  PutBE32(core, di::kNextUnlinked, kNullAgino);
  PutBE64(core, di::kIno, ino);
  std::memcpy(core + di::kUuid, geo.meta_uuid.data(), 16);
  UpdateXfsChecksum(core, geo.inode_size, di::kCrc);
}

// Root directory shortform content: "." and ".." entries.
struct SfDirContent {
  std::vector<uint8_t> data;
  uint64_t size = 0;
};

SfDirContent BuildRootDirSf(const FormatGeometry& geo) {
  // XFS shortform directories do not store "." or "..": the parent inode is
  // part of the shortform header and "." is the inode itself. The root
  // directory therefore contains just the 6-byte header.
  SfDirContent out;
  uint8_t hdr[6] = {};
  hdr[d::kSfHdrCount] = 0;
  hdr[d::kSfHdrI8Count] = 0;
  PutBE32(hdr, d::kSfHdrParent, static_cast<uint32_t>(geo.rootino));
  out.data.assign(hdr, hdr + 6);
  out.size = 6;
  return out;
}

void InitRootInode(const FormatGeometry& geo, uint8_t* core, uint32_t gen,
                   const SfDirContent& sf) {
  std::memset(core, 0, geo.inode_size);

  PutBE16(core, di::kMagic, ondisk::kDinodeMagic);
  PutBE16(core, di::kMode, 0x41ED);  // S_IFDIR | 0755
  core[di::kVersion] = 3;
  core[di::kFormat] = di::kFormatLocal;
  PutBE32(core, di::kUid, 0);
  PutBE32(core, di::kGid, 0);
  PutBE32(core, di::kNlink, 2);
  PutBE32(core, di::kNextents, 0);
  PutBE16(core, di::kAnextents, 0);
  core[di::kForkoff] = 0;
  // No attribute fork, but the attribute format is still EXTENTS (like mkfs).
  core[di::kAformat] = di::kFormatExtents;
  PutBE32(core, di::kGen, gen);
  PutBE32(core, di::kNextUnlinked, kNullAgino);
  PutBE64(core, di::kSize, sf.size);

  // Timestamps: now, in the legacy 32:32 encoding (bigtime is not enabled).
  int64_t now = static_cast<int64_t>(std::time(nullptr));
  uint64_t ts = (static_cast<uint64_t>(static_cast<uint32_t>(now)) << 32);
  PutBE64(core, di::kAtime, ts);
  PutBE64(core, di::kMtime, ts);
  PutBE64(core, di::kCtime, ts);
  PutBE64(core, di::kChangecount, 0);
  PutBE64(core, di::kLsn, 0);
  PutBE64(core, di::kFlags2, 0);
  PutBE32(core, di::kCowextsize, 0);
  PutBE64(core, di::kCrtime, ts);
  PutBE64(core, di::kIno, geo.rootino);
  std::memcpy(core + di::kUuid, geo.meta_uuid.data(), 16);

  // Data fork: shortform directory. The inode CRC covers the whole inode, so
  // the fork content must be in place before the checksum is computed.
  std::memcpy(core + di::kCoreSizeV3, sf.data.data(), sf.data.size());
  UpdateXfsChecksum(core, geo.inode_size, di::kCrc);
}

// Allocated metadata file with an empty data fork: the realtime bitmap and
// summary inodes. mkfs creates them as S_IFREG inodes with nlink 1 even when
// the filesystem has no realtime device; the bitmap inode carries
// XFS_DIFLAG_NEWRTBM and a zeroed atime until the bitmap is first written.
void InitRtMetadataInode(const FormatGeometry& geo, uint8_t* core, uint64_t ino,
                         uint32_t gen, bool bitmap) {
  std::memset(core, 0, geo.inode_size);

  PutBE16(core, di::kMagic, ondisk::kDinodeMagic);
  PutBE16(core, di::kMode, 0x8000);  // S_IFREG (mkfs passes only S_IFREG)
  core[di::kVersion] = 3;
  core[di::kFormat] = di::kFormatExtents;
  PutBE32(core, di::kNlink, 1);
  core[di::kForkoff] = 0;
  core[di::kAformat] = di::kFormatExtents;
  PutBE32(core, di::kGen, gen);
  PutBE32(core, di::kNextUnlinked, kNullAgino);
  PutBE64(core, di::kSize, 0);

  int64_t now = static_cast<int64_t>(std::time(nullptr));
  uint64_t ts = (static_cast<uint64_t>(static_cast<uint32_t>(now)) << 32);
  if (!bitmap) {
    PutBE64(core, di::kAtime, ts);
  }
  PutBE64(core, di::kMtime, ts);
  PutBE64(core, di::kCtime, ts);
  PutBE64(core, di::kCrtime, ts);
  if (bitmap) {
    PutBE16(core, di::kFlags, 0x0004);  // XFS_DIFLAG_NEWRTBM
  }
  PutBE64(core, di::kIno, ino);
  std::memcpy(core + di::kUuid, geo.meta_uuid.data(), 16);
  UpdateXfsChecksum(core, geo.inode_size, di::kCrc);
}

}  // namespace

void BuildInodeChunk(const FormatGeometry& geo, uint32_t gen,
                     std::vector<uint8_t>* buf) {
  const uint64_t chunk_bytes =
      static_cast<uint64_t>(geo.chunk_blocks) * geo.block_size;
  buf->assign(static_cast<size_t>(chunk_bytes), 0);

  // All inodes of the chunk start zeroed except their v3 core (magic,
  // version, generation, unlinked pointer, inode number, uuid, crc).
  uint64_t chunk_start_ino = static_cast<uint64_t>(geo.rootino_agino);
  uint64_t ino = chunk_start_ino;
  for (uint64_t off = 0; off + geo.inode_size <= chunk_bytes;
       off += geo.inode_size) {
    InitFreeInodeCore(geo, buf->data() + off, ino, gen);
    ++ino;
  }

  // The root inode is the first inode of the chunk; overwrite it fully.
  SfDirContent sf = BuildRootDirSf(geo);
  uint64_t root_off = (geo.rootino - chunk_start_ino) * geo.inode_size;
  InitRootInode(geo, buf->data() + root_off, gen, sf);

  // Real-time bitmap (rootino+1) and summary (rootino+2) inodes follow the
  // root inode and are always allocated, even without a realtime device.
  InitRtMetadataInode(geo, buf->data() + root_off + geo.inode_size,
                      geo.rootino + 1, gen, /*bitmap=*/true);
  InitRtMetadataInode(geo, buf->data() + root_off + 2 * geo.inode_size,
                      geo.rootino + 2, gen, /*bitmap=*/false);
}

}  // namespace xfs::mkfs::internal
