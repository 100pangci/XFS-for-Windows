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
  UpdateXfsChecksum(core, di::kCoreSizeV3, di::kCrc);
}

// Root directory shortform content: "." and ".." entries.
struct SfDirContent {
  std::vector<uint8_t> data;
  uint64_t size = 0;
};

SfDirContent BuildRootDirSf(const FormatGeometry& geo) {
  // Entry offsets use the "data block" offset space (xfs_dir2 geometry).
  uint32_t data_first_offset =
      static_cast<uint32_t>(d::kData3HeaderSize) +
      static_cast<uint32_t>(d::DataEntrySize(1, true)) +
      static_cast<uint32_t>(d::DataEntrySize(2, true));

  SfDirContent out;
  uint8_t hdr[6] = {};
  hdr[d::kSfHdrCount] = 2;
  hdr[d::kSfHdrI8Count] = 0;
  PutBE32(hdr, d::kSfHdrParent, static_cast<uint32_t>(geo.rootino));
  out.data.assign(hdr, hdr + 6);

  auto add_entry = [&](const char* name, uint8_t namelen, uint16_t offset) {
    uint8_t ent[16];
    std::memset(ent, 0, sizeof(ent));
    ent[d::kSfEntNamelen] = namelen;
    PutBE16(ent, d::kSfEntOffset, offset);
    std::memcpy(ent + d::kSfEntName, name, namelen);
    ent[d::kSfEntName + namelen] = d::kFtDir;  // ftype
    PutBE32(ent, d::kSfEntName + namelen + 1,
            static_cast<uint32_t>(geo.rootino));
    size_t entsize = d::kSfEntName + namelen + 1 + 4;
    out.data.insert(out.data.end(), ent, ent + entsize);
  };
  add_entry(".", 1, static_cast<uint16_t>(data_first_offset));
  add_entry("..", 2,
            static_cast<uint16_t>(data_first_offset +
                                  d::DataEntrySize(1, true)));
  out.size = out.data.size();
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
  core[di::kAformat] = 0;
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

  UpdateXfsChecksum(core, di::kCoreSizeV3, di::kCrc);

  // Data fork: shortform directory.
  std::memcpy(core + di::kCoreSizeV3, sf.data.data(), sf.data.size());
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
}

}  // namespace xfs::mkfs::internal
