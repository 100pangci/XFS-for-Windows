// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "support/fixture_image.h"

#include <algorithm>
#include <cstring>
#include <ctime>

#include "xfs/ag.h"
#include "xfs/btree.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/formatter.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"

using namespace xfs;

namespace xfstest {
namespace {

namespace agf = ondisk::agf;
namespace agi = ondisk::agi;
namespace bt = ondisk::btree;
namespace d = ondisk::dir;
namespace di = ondisk::inode;
namespace sb = ondisk::sb;

constexpr uint32_t kNullAgino = 0xffffffffu;
// xfs_dir2_data_first_offset for 4 KiB dir3 blocks: data header (64) plus the
// notional "." and ".." entries (16 bytes each). Linux's xfs_dir2_sf_verify
// requires stored entry offsets to be >= this and non-decreasing.
constexpr uint16_t kShortformFirstOffset = 96;

struct FreeExtent {
  uint32_t start = 0;
  uint32_t count = 0;
};

uint64_t NowTimestamp() {
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  return static_cast<uint64_t>(static_cast<uint32_t>(now)) << 32;
}

size_t PopCount64(uint64_t v) {
  size_t n = 0;
  while (v) {
    v &= v - 1;
    ++n;
  }
  return n;
}

// Reads an AG btree root and requires it to be a leaf (fresh filesystems only
// have root-level records; the fixture builder does not implement splits).
Status ReadLeafRoot(BlockDevice& device, const XfsGeometry& geo, uint32_t agno,
                    uint32_t agbno, uint32_t magic, const char* what,
                    std::vector<uint8_t>* block,
                    std::vector<FreeExtent>* records) {
  const uint64_t fsb = geo.AgFsb(agno, agbno);
  block->assign(geo.block_size, 0);
  if (!device.Read(geo.FsbToByte(fsb), block->data(), block->size())) {
    return XFS_ERR(XfsError::kIoError, std::string("cannot read ") + what)
        .Ag(agno)
        .Metadata(what);
  }
  const uint8_t* p = block->data();
  if (GetBE32(p, bt::kMagic) != magic) {
    return XFS_ERR(XfsError::kBadMagic, std::string("bad ") + what + " magic")
        .Ag(agno)
        .Metadata(what)
        .Expected(magic)
        .Actual(GetBE32(p, bt::kMagic));
  }
  if (GetBE16(p, bt::kLevel) != 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   std::string("fixture builder expects a leaf-level ") + what +
                       " (fresh filesystem with one record)")
        .Ag(agno)
        .Metadata(what)
        .Actual(GetBE16(p, bt::kLevel));
  }
  const uint16_t numrecs = GetBE16(p, bt::kNumrecs);
  const size_t maxrecs = (geo.block_size - bt::kSHeaderLenV5) / bt::kAllocRecSize;
  if (numrecs > maxrecs) {
    return XFS_ERR(XfsError::kCorruptBtree, std::string("bad ") + what +
                                               " record count")
        .Ag(agno)
        .Metadata(what)
        .Expected(maxrecs)
        .Actual(numrecs);
  }
  records->clear();
  for (uint16_t i = 0; i < numrecs; ++i) {
    FreeExtent rec;
    rec.start = GetBE32(p, bt::kSHeaderLenV5 + i * bt::kAllocRecSize);
    rec.count = GetBE32(p, bt::kSHeaderLenV5 + i * bt::kAllocRecSize + 4);
    records->push_back(rec);
  }
  return OkStatus();
}

Status WriteBtreeRoot(BlockDevice& device, const XfsGeometry& geo,
                      uint32_t agno, uint32_t agbno, uint32_t magic,
                      const char* what, std::vector<uint8_t>* block,
                      const std::vector<FreeExtent>& records) {
  uint8_t* p = block->data();
  PutBE32(p, bt::kMagic, magic);
  PutBE16(p, bt::kLevel, 0);
  PutBE16(p, bt::kNumrecs, static_cast<uint16_t>(records.size()));
  const size_t maxrecs = (geo.block_size - bt::kSHeaderLenV5) / bt::kAllocRecSize;
  if (records.size() > maxrecs) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   std::string("fixture would need a deeper ") + what)
        .Ag(agno)
        .Metadata(what)
        .Expected(maxrecs)
        .Actual(records.size());
  }
  uint8_t* rec = p + bt::kSHeaderLenV5;
  for (const auto& entry : records) {
    PutBE32(rec, 0, entry.start);
    PutBE32(rec, 4, entry.count);
    rec += bt::kAllocRecSize;
  }
  UpdateXfsChecksum(p, geo.block_size, bt::kSCrc);
  const uint64_t fsb = geo.AgFsb(agno, agbno);
  if (!device.Write(geo.FsbToByte(fsb), p, geo.block_size)) {
    return XFS_ERR(XfsError::kIoError, std::string("cannot write ") + what)
        .Ag(agno)
        .Fsb(fsb)
        .Metadata(what);
  }
  return OkStatus();
}

void BuildRegularFileInode(const XfsGeometry& geo, uint8_t* core, uint64_t ino,
                           uint32_t gen, uint64_t size, uint64_t nblocks,
                           const ondisk::BmbtRec& extent, uint64_t timestamp) {
  std::memset(core, 0, geo.inode_size);
  PutBE16(core, di::kMagic, ondisk::kDinodeMagic);
  PutBE16(core, di::kMode, 0100644);  // S_IFREG | 0644
  core[di::kVersion] = 3;
  core[di::kFormat] = di::kFormatExtents;
  PutBE32(core, di::kNlink, 1);
  PutBE32(core, di::kNextents, 1);
  PutBE16(core, di::kAnextents, 0);
  core[di::kForkoff] = 0;
  core[di::kAformat] = di::kFormatExtents;
  PutBE64(core, di::kSize, size);
  PutBE64(core, di::kNblocks, nblocks);
  PutBE64(core, di::kAtime, timestamp);
  PutBE64(core, di::kMtime, timestamp);
  PutBE64(core, di::kCtime, timestamp);
  PutBE64(core, di::kCrtime, timestamp);
  PutBE32(core, di::kGen, gen);
  PutBE32(core, di::kNextUnlinked, kNullAgino);
  PutBE64(core, di::kIno, ino);
  std::memcpy(core + di::kUuid, geo.meta_uuid.data(), 16);

  uint64_t l0 = 0, l1 = 0;
  ondisk::EncodeBmbtRec(extent, &l0, &l1);
  PutBE64(core, di::kCoreSizeV3, l0);
  PutBE64(core, di::kCoreSizeV3 + 8, l1);
  UpdateXfsChecksum(core, geo.inode_size, di::kCrc);
}

// Rewrites the root directory as a shortform directory with a single entry.
Status SetRootDirectoryEntry(const XfsGeometry& geo, uint8_t* core,
                             const std::string& name, uint64_t child_ino,
                             uint64_t timestamp) {
  if (name.empty() || name.size() > 255) {
    return XFS_ERR(XfsError::kNameTooLong, "invalid fixture file name");
  }
  std::vector<uint8_t> fork;
  fork.assign(6, 0);
  fork[d::kSfHdrCount] = 1;
  fork[d::kSfHdrI8Count] = 0;
  PutBE32(fork.data(), d::kSfHdrParent, static_cast<uint32_t>(geo.rootino));

  const size_t off = fork.size();
  const size_t namelen = name.size();
  fork.resize(off + d::kSfEntName + namelen + 1 + 4, 0);
  fork[off + d::kSfEntNamelen] = static_cast<uint8_t>(namelen);
  PutBE16(fork.data(), off + d::kSfEntOffset, kShortformFirstOffset);
  std::memcpy(fork.data() + off + d::kSfEntName, name.data(), namelen);
  fork[off + d::kSfEntName + namelen] = d::kFtRegFile;
  PutBE32(fork.data(), off + d::kSfEntName + namelen + 1,
          static_cast<uint32_t>(child_ino));

  const size_t literal = geo.inode_size - di::kCoreSizeV3;
  if (fork.size() > literal) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "fixture directory does not fit the inode literal area")
        .Expected(literal)
        .Actual(fork.size());
  }
  core[di::kFormat] = di::kFormatLocal;
  PutBE64(core, di::kSize, fork.size());
  PutBE64(core, di::kMtime, timestamp);
  PutBE64(core, di::kCtime, timestamp);
  std::memset(core + di::kCoreSizeV3, 0, literal);
  std::memcpy(core + di::kCoreSizeV3, fork.data(), fork.size());
  UpdateXfsChecksum(core, geo.inode_size, di::kCrc);
  return OkStatus();
}

}  // namespace

Status BuildFixtureImage(BlockDevice& device, const FixtureOptions& options,
                         FixtureInfo* info) {
  if (info) *info = FixtureInfo{};
  if (!device.Info().writable) {
    return XFS_ERR(XfsError::kReadOnlyVolume,
                   "fixture builder needs a writable device");
  }

  // 1. A real, fully consistent filesystem.
  mkfs::FormatOptions format_options;
  format_options.quiet = options.quiet;
  format_options.uuid = options.uuid;
  auto report = mkfs::Format(device, format_options);
  if (!report.ok()) return report.error();

  auto sb_result = ReadSuperblock(device, 0, /*verify_crc=*/true);
  if (!sb_result.ok()) return sb_result.error();
  const XfsSuperblock superblock = sb_result.value();
  const XfsGeometry geo = MakeGeometry(superblock);

  if (geo.block_size != 4096 || geo.inode_size != 512 ||
      geo.sector_size != 512 || geo.dir_blk_log != 0) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "fixture builder expects the 4 KiB/512-byte profile")
        .Expected(4096)
        .Actual(geo.block_size);
  }
  const uint32_t agno = 0;
  auto ag_headers = ReadAgHeaders(device, geo, agno);
  if (!ag_headers.ok()) return ag_headers.error();
  XfsAgf ag_hdr = ag_headers.value().agf;
  XfsAgi agi_hdr = ag_headers.value().agi;
  if (agi_hdr.count == 0) {
    return XFS_ERR(XfsError::kCorruptAgHeader,
                   "fixture builder expects one inode chunk in AG0")
        .Ag(agno)
        .Metadata("AGI");
  }

  // 2. Take one contiguous extent from the largest free extent in AG0.
  std::vector<uint8_t> bno_block, cnt_block;
  std::vector<FreeExtent> free_records;
  Status s = ReadLeafRoot(device, geo, agno, ag_hdr.bno_root,
                          ondisk::kBtbCrcMagic, "bnobt root", &bno_block,
                          &free_records);
  if (!s.ok()) return s;
  if (free_records.empty()) {
    return XFS_ERR(XfsError::kCorruptBtree, "AG0 has no free space")
        .Ag(agno)
        .Metadata("bnobt");
  }
  auto best = std::max_element(
      free_records.begin(), free_records.end(),
      [](const FreeExtent& a, const FreeExtent& b) { return a.count < b.count; });
  if (best->count < options.file_blocks) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "fixture image is too small for the file extent")
        .Ag(agno)
        .Expected(options.file_blocks)
        .Actual(best->count);
  }
  const uint32_t data_start = best->start + best->count - options.file_blocks;
  const uint32_t before = best->start;
  const uint32_t after = best->count - options.file_blocks;
  free_records.erase(best);
  if (before != 0) {
    FreeExtent head;
    head.start = before;
    head.count = after;
    // The record only shrinks, so it keeps its position in the start-ordered
    // bnobt; keep the vector sorted by start anyway.
    free_records.push_back(head);
  }
  std::sort(free_records.begin(), free_records.end(),
            [](const FreeExtent& a, const FreeExtent& b) {
              return a.start < b.start;
            });

  // 3. Update AGF counters (freeblks excludes the AGFL, which is untouched).
  uint32_t free_blocks = 0, longest = 0;
  for (const auto& rec : free_records) {
    free_blocks += rec.count;
    longest = std::max(longest, rec.count);
  }
  {
    const uint64_t ag_byte = geo.AgByteOffset(agno);
    ag_hdr.freeblks = free_blocks;
    ag_hdr.longest = longest;
    std::vector<uint8_t> agf_buf(geo.sector_size, 0);
    if (!device.Read(ag_byte + AgfByteOffset(geo), agf_buf.data(),
                     agf_buf.size())) {
      return XFS_ERR(XfsError::kIoError, "cannot read AGF").Ag(agno);
    }
    PutBE32(agf_buf.data(), agf::kFreeblks, ag_hdr.freeblks);
    PutBE32(agf_buf.data(), agf::kLongest, ag_hdr.longest);
    UpdateXfsChecksum(agf_buf.data(), agf_buf.size(), agf::kCrc);
    if (!device.Write(ag_byte + AgfByteOffset(geo), agf_buf.data(),
                      agf_buf.size())) {
      return XFS_ERR(XfsError::kIoError, "cannot write AGF").Ag(agno);
    }

    // bnobt (start-ordered) and cntbt ((count, start)-ordered).
    s = WriteBtreeRoot(device, geo, agno, ag_hdr.bno_root,
                       ondisk::kBtbCrcMagic, "bnobt root", &bno_block,
                       free_records);
    if (!s.ok()) return s;
    std::vector<FreeExtent> cnt_records;
    s = ReadLeafRoot(device, geo, agno, ag_hdr.cnt_root, ondisk::kBtcCrcMagic,
                     "cntbt root", &cnt_block, &cnt_records);
    if (!s.ok()) return s;
    cnt_records = free_records;
    std::sort(cnt_records.begin(), cnt_records.end(),
              [](const FreeExtent& a, const FreeExtent& b) {
                if (a.count != b.count) return a.count < b.count;
                return a.start < b.start;
              });
    s = WriteBtreeRoot(device, geo, agno, ag_hdr.cnt_root,
                       ondisk::kBtcCrcMagic, "cntbt root", &cnt_block,
                       cnt_records);
    if (!s.ok()) return s;
  }

  // 4. Allocate inode 67: clear its inobt free bit, then update AGI/sb counts.
  const uint64_t file_ino = geo.rootino + 3;
  uint32_t inobt_startino = 0;
  {
    std::vector<uint8_t> inobt_buf;
    std::vector<FreeExtent> unused;
    s = ReadLeafRoot(device, geo, agno, agi_hdr.root, ondisk::kIbtCrcMagic,
                     "inobt root", &inobt_buf, &unused);
    if (!s.ok()) return s;
    if (GetBE16(inobt_buf.data(), bt::kNumrecs) != 1) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "fixture builder expects exactly one inode chunk")
          .Ag(agno)
          .Metadata("inobt");
    }
    inobt_startino = GetBE32(inobt_buf.data(), bt::kSHeaderLenV5);
    if (file_ino < inobt_startino || file_ino >= inobt_startino + 64) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "fixture inode is outside the inode chunk")
          .Ino(file_ino)
          .Expected(inobt_startino);
    }
    uint64_t free_mask = GetBE64(inobt_buf.data(), bt::kSHeaderLenV5 + 8);
    const uint32_t bit = static_cast<uint32_t>(file_ino - inobt_startino);
    if ((free_mask & (1ull << bit)) == 0) {
      return XFS_ERR(XfsError::kAlreadyExists,
                     "fixture inode is already allocated")
          .Ino(file_ino);
    }
    free_mask &= ~(1ull << bit);
    PutBE64(inobt_buf.data(), bt::kSHeaderLenV5 + 8, free_mask);
    PutBE32(inobt_buf.data(), bt::kSHeaderLenV5 + 4,
            static_cast<uint32_t>(PopCount64(free_mask)));
    UpdateXfsChecksum(inobt_buf.data(), geo.block_size, bt::kSCrc);
    if (!device.Write(geo.FsbToByte(geo.AgFsb(agno, agi_hdr.root)),
                      inobt_buf.data(), inobt_buf.size())) {
      return XFS_ERR(XfsError::kIoError, "cannot write inobt root").Ag(agno);
    }

    const uint64_t ag_byte = geo.AgByteOffset(agno);
    std::vector<uint8_t> agi_buf(geo.sector_size, 0);
    if (!device.Read(ag_byte + AgiByteOffset(geo), agi_buf.data(),
                     agi_buf.size())) {
      return XFS_ERR(XfsError::kIoError, "cannot read AGI").Ag(agno);
    }
    const uint32_t freecount =
        static_cast<uint32_t>(PopCount64(free_mask));
    PutBE32(agi_buf.data(), agi::kFreecount, freecount);
    UpdateXfsChecksum(agi_buf.data(), agi_buf.size(), agi::kCrc);
    if (!device.Write(ag_byte + AgiByteOffset(geo), agi_buf.data(),
                      agi_buf.size())) {
      return XFS_ERR(XfsError::kIoError, "cannot write AGI").Ag(agno);
    }
  }

  // 5. Primary and secondary superblocks: inodes and free blocks change.
  {
    std::vector<uint8_t> sb_buf(geo.sector_size, 0);
    if (!device.Read(0, sb_buf.data(), sb_buf.size())) {
      return XFS_ERR(XfsError::kIoError, "cannot read superblock");
    }
    const uint64_t ifree = GetBE64(sb_buf.data(), sb::kIfree);
    const uint64_t fdblocks = GetBE64(sb_buf.data(), sb::kFdblocks);
    PutBE64(sb_buf.data(), sb::kIfree, ifree - 1);
    PutBE64(sb_buf.data(), sb::kFdblocks, fdblocks - options.file_blocks);
    UpdateXfsChecksum(sb_buf.data(), sb_buf.size(), sb::kCrc);
    // mkfs writes identical copies to every AG; keep that invariant so the
    // fixture looks like a freshly formatted filesystem that was then used.
    for (uint32_t ag = 0; ag < geo.agcount; ++ag) {
      if (!device.Write(geo.AgByteOffset(ag), sb_buf.data(), sb_buf.size())) {
        return XFS_ERR(XfsError::kIoError, "cannot write superblock copy")
            .Ag(ag);
      }
    }
  }

  // 6. Inode chunk: the new file inode and the updated root directory.
  const uint64_t timestamp = NowTimestamp();
  const uint64_t file_size =
      static_cast<uint64_t>(options.file_blocks) * geo.block_size;
  // The free records came from AG0, so data_start is an AG0-relative block.
  const uint64_t data_fsb = geo.AgFsb(agno, data_start);
  {
    const uint32_t chunk_agbno = inobt_startino >> geo.inopblog;
    const uint32_t chunk_blocks =
        static_cast<uint32_t>((ondisk::kInodesPerChunk * geo.inode_size +
                               geo.block_size - 1) >>
                              geo.block_log);
    const uint64_t chunk_offset = geo.FsbToByte(geo.AgFsb(agno, chunk_agbno));
    std::vector<uint8_t> chunk(static_cast<size_t>(chunk_blocks) * geo.block_size,
                               0);
    if (!device.Read(chunk_offset, chunk.data(), chunk.size())) {
      return XFS_ERR(XfsError::kIoError, "cannot read inode chunk").Ag(agno);
    }

    // The root inode already exists; only its shortform content changes.
    const size_t root_off =
        static_cast<size_t>((geo.rootino - inobt_startino) * geo.inode_size);
    s = SetRootDirectoryEntry(geo, chunk.data() + root_off, options.file_name,
                              file_ino, timestamp);
    if (!s.ok()) return s;

    // Generation of the other inodes in the chunk (mkfs writes the same value).
    const uint32_t gen = GetBE32(chunk.data() + root_off + di::kGen);
    const size_t file_off =
        static_cast<size_t>((file_ino - inobt_startino) * geo.inode_size);
    ondisk::BmbtRec extent;
    extent.startoff = 0;
    extent.startblock = data_fsb;
    extent.blockcount = options.file_blocks;
    extent.unwritten = false;
    // di_nblocks counts *filesystem* blocks (the kernel adds the bmap extent
    // lengths to i_nblocks directly and xfs_repair recomputes it the same way),
    // even though the on-disk comment says "512 byte blocks".
    BuildRegularFileInode(geo, chunk.data() + file_off, file_ino, gen,
                          file_size, options.file_blocks, extent, timestamp);

    if (!device.Write(chunk_offset, chunk.data(), chunk.size())) {
      return XFS_ERR(XfsError::kIoError, "cannot write inode chunk").Ag(agno);
    }
  }

  // 7. File data.
  {
    std::vector<uint8_t> data(static_cast<size_t>(file_size), 0);
    for (size_t i = 0; i < data.size(); ++i) {
      data[i] = FixturePatternByte(i);
    }
    if (!device.Write(geo.FsbToByte(data_fsb), data.data(), data.size())) {
      return XFS_ERR(XfsError::kIoError, "cannot write fixture file data")
          .Fsb(data_fsb);
    }
  }

  if (!device.Flush()) {
    return XFS_ERR(XfsError::kIoError, "cannot flush fixture image").Offset(0);
  }

  if (info) {
    info->block_size = geo.block_size;
    info->file_ino = file_ino;
    info->file_size = file_size;
    info->data_fsb = data_fsb;
    info->data_blocks = options.file_blocks;
  }
  return OkStatus();
}

}  // namespace xfstest
