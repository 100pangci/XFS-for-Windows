// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - inode metadata writes (RW-1).
//
// The first metadata mutation is deliberately small: update atime/mtime/ctime
// of one inode through a transaction. It exercises the whole machinery without
// an allocator:
//
//   inode log item (xfs_inode_log_format + xfs_log_dinode, host byte order)
//     -> log record (commit op)
//       -> flushed
//         -> on-disk inode rewritten (times + di_lsn + CRC), flushed
//
// The on-disk inode is read raw and written back raw so the data/attr forks
// are preserved byte for byte; only the core fields we own are touched.
#include <chrono>
#include <cstring>

#include "xfs/crc32c.h"
#include "xfs/crash.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/log.h"
#include "xfs/log_format.h"
#include "xfs/ondisk.h"
#include "xfs/transaction.h"

namespace xfs {
namespace {

namespace di = ondisk::inode;
namespace lf = logfmt;

// Size of the inode cluster buffer the inode item names, exactly as the kernel
// computes it (xfs_ialloc.c, xfs_inode_cluster_size / xfs_imap):
//
//   raw = XFS_INODE_BIG_CLUSTER_SIZE (8 KiB); on a v3 filesystem it scales
//   with the inode size (inode_size / XFS_DINODE_MIN_SIZE) when the inode
//   chunk alignment allows it, then it is floored to whole filesystem blocks
//   (and to one block if it would be smaller).
//
// The item must describe the same buffer the kernel's own xfs_inode_item_format
// describes: replay reads it, applies the logged core at ilf_boffset and writes
// the buffer back.
uint64_t InodeClusterBytes(const XfsGeometry& geo) {
  constexpr uint64_t kBigClusterSize = 8192;   // XFS_INODE_BIG_CLUSTER_SIZE
  constexpr uint64_t kMinInodeSize = 256;      // XFS_DINODE_MIN_SIZE
  uint64_t raw = kBigClusterSize;
  const uint64_t scaled = raw * (geo.inode_size / kMinInodeSize);
  if (geo.ino_align >= scaled / geo.block_size) raw = scaled;
  if (raw <= geo.block_size) return geo.block_size;
  return (raw / geo.block_size) * geo.block_size;
}

// Field layout shared by the on-disk v3 inode core and the log dinode: same
// offsets, different byte order (disk big-endian, log dinode host order).
struct CoreField {
  size_t offset;
  size_t size;
};
constexpr CoreField kCoreFields[] = {
    {di::kMagic, 2},      {di::kMode, 2},      {di::kVersion, 1},
    {di::kFormat, 1},     {di::kOnlink, 2},    {di::kUid, 4},
    {di::kGid, 4},        {di::kNlink, 4},     {di::kProjidLo, 2},
    {di::kProjidHi, 2},   {di::kBigNextents, 8}, {di::kAtime, 8},
    {di::kMtime, 8},      {di::kCtime, 8},     {di::kSize, 8},
    {di::kNblocks, 8},    {di::kExtsize, 4},   {di::kNextents, 4},
    {di::kAnextents, 2},  {di::kForkoff, 1},   {di::kAformat, 1},
    {di::kDmevmask, 4},   {di::kDmstate, 2},   {di::kFlags, 2},
    {di::kGen, 4},        {di::kNextUnlinked, 4}, {di::kChangecount, 8},
    {di::kFlags2, 8},     {di::kCowextsize, 4}, {di::kCrtime, 8},
    {di::kIno, 8},
};

// Copies the inode core from on-disk (big-endian) into the log dinode (host
// order). The CRC and LSN fields are intentionally left zero: recovery ignores
// the logged LSN and recomputes the CRC.
void CopyCoreToLogDinode(const uint8_t* disk, size_t inode_size,
                         std::vector<uint8_t>* log_dinode) {
  log_dinode->assign(lf::kLogDinodeSize, 0);
  uint8_t* out = log_dinode->data();
  const bool bigtime = InodeImageHasBigtime(disk);
  for (const CoreField& field : kCoreFields) {
    if (field.offset + field.size > inode_size) continue;
    const uint8_t* src = disk + field.offset;
    uint8_t* dst = out + field.offset;
    switch (field.size) {
      case 1:
        dst[0] = src[0];
        break;
      case 2:
        StoreLE16(dst, LoadBE16(src));
        break;
      case 4:
        StoreLE32(dst, LoadBE32(src));
        break;
      case 8: {
        const bool timestamp =
            field.offset == di::kAtime || field.offset == di::kMtime ||
            field.offset == di::kCtime || field.offset == di::kCrtime;
        if (timestamp && bigtime) {
          // Bigtime is a single host-order nanoseconds value in the log
          // dinode (xfs_inode_to_log_dinode_ts).
          StoreLE64(dst, LoadBE64(src));
        } else if (timestamp) {
          // Legacy timestamps are (seconds, nanoseconds) pairs; both halves
          // are 32-bit host-order fields.
          StoreLE32(dst, LoadBE32(src));
          StoreLE32(dst + 4, LoadBE32(src + 4));
        } else {
          StoreLE32(dst, static_cast<uint32_t>(LoadBE64(src) & 0xffffffffu));
          StoreLE32(dst + 4, static_cast<uint32_t>(LoadBE64(src) >> 32));
        }
        break;
      }
      default:
        break;
    }
  }
  // di_uuid is a raw byte field, copied as-is when present.
  if (di::kUuid + 16 <= inode_size && di::kUuid + 16 <= lf::kLogDinodeSize) {
    std::memcpy(out + di::kUuid, disk + di::kUuid, 16);
  }
}

}  // namespace

Result<std::vector<uint8_t>> XfsFilesystem::ReadRawInode(uint64_t ino) const {
  if (!geometry_.InoValid(ino)) {
    return XFS_ERR(XfsError::kOutOfRange, "inode number out of range")
        .Ino(ino)
        .Metadata("inode");
  }
  const uint64_t inode_offset = geometry_.InoToByteOffset(ino);
  std::vector<uint8_t> inode(geometry_.inode_size, 0);
  if (!device_->Read(inode_offset, inode.data(), inode.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read inode")
        .Ino(ino)
        .Offset(inode_offset)
        .Metadata("inode");
  }
  return inode;
}

Status XfsFilesystem::UpdateInodeInTransaction(
    XfsTransaction& txn, uint64_t ino, uint32_t ilog_fields,
    const std::vector<uint8_t>& fork_region,
    const std::function<Status(uint8_t* raw)>& mutate) {
  auto raw = ReadRawInode(ino);
  if (!raw.ok()) return raw.error();
  std::vector<uint8_t> inode = std::move(raw.value());
  XfsInode parsed;
  Status s = ParseInode(inode.data(), inode.size(), geometry_, ino, &parsed);
  if (!s.ok()) return s;
  // Any inode core update upgrades a legacy-timestamp inode on a bigtime
  // filesystem (xfs_inode_item_precommit).
  s = UpgradeInodeTimestampsToBigtime(inode.data(), inode.size(), geometry_);
  if (!s.ok()) return s;
  s = mutate(inode.data());
  if (!s.ok()) return s;
  return LogPreparedInode(txn, ino, std::move(inode), ilog_fields, fork_region);
}

Status XfsFilesystem::LogPreparedInode(XfsTransaction& txn, uint64_t ino,
                                       std::vector<uint8_t> inode,
                                       uint32_t ilog_fields,
                                       const std::vector<uint8_t>& fork_region) {
  if (!geometry_.InoValid(ino)) {
    return XFS_ERR(XfsError::kOutOfRange, "inode number out of range")
        .Ino(ino)
        .Metadata("inode");
  }
  if (inode.size() != geometry_.inode_size) {
    return XFS_ERR(XfsError::kInternal, "inode image has the wrong size")
        .Ino(ino)
        .Metadata("inode");
  }
  const uint64_t inode_offset = geometry_.InoToByteOffset(ino);
  // The named buffer is the inode *cluster* (what xfs_imap does): replay reads
  // it through the inode buffer verifier and applies the logged core at
  // ilf_boffset inside it.
  const uint64_t cluster_bytes = InodeClusterBytes(geometry_);
  const uint64_t cluster_start = inode_offset - (inode_offset % cluster_bytes);
  const uint64_t boffset = inode_offset - cluster_start;
  const uint32_t agno = geometry_.InoAgno(ino);
  const uint32_t cluster_agbno = geometry_.InoAgbno(ino) -
                                 static_cast<uint32_t>(boffset /
                                                       geometry_.block_size);
  const uint32_t cluster_blocks =
      static_cast<uint32_t>(cluster_bytes / geometry_.block_size);
  if (cluster_blocks == 0 ||
      cluster_agbno + cluster_blocks > geometry_.AgBlocksFor(agno)) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "inode cluster buffer is out of range")
        .Ino(ino)
        .Ag(agno)
        .Metadata("inode");
  }

  // Every inode item logs the core, so a bigtime filesystem upgrades this
  // inode's timestamps now even when the caller only changed a fork.
  Status upgrade =
      UpgradeInodeTimestampsToBigtime(inode.data(), inode.size(), geometry_);
  if (!upgrade.ok()) return upgrade;

  // The legacy extent counters are 31/15 bits in the kernel (the high bit is
  // reserved); the large counters are the only way past them.
  if (!InodeImageHasNrext64(inode.data())) {
    if (InodeDataExtentCount(inode.data()) > 0x7fffffffull ||
        InodeAttrExtentCount(inode.data()) > 0x7fffu) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "extent count requires NREXT64 but the inode does not "
                     "use large counters")
          .Ino(ino)
          .Metadata("inode");
    }
  }

  std::vector<uint8_t> log_dinode;
  CopyCoreToLogDinode(inode.data(), inode.size(), &log_dinode);

  // The kernel's recovery checks the logged region against
  // roundup(ilf_dsize, 4) (xlog_calc_iovec_len), so pad a fork region that is
  // not a multiple of 4 with zeros and log the padded length. The bytes past
  // the real fork data are ignored by the inode verifiers.
  std::vector<uint8_t> logged_region = fork_region;
  if (!logged_region.empty() && (logged_region.size() % 4) != 0) {
    logged_region.resize((logged_region.size() + 3) & ~size_t{3}, 0);
  }
  const bool with_region = !logged_region.empty();
  uint32_t fields = lf::kILogCore | ilog_fields;
  if (!with_region) {
    // No data fork region to log (for example an empty extent list): drop the
    // fork flag, exactly like xfs_inode_item_format does.
    fields &= ~(lf::kILogDExt | lf::kILogDBRoot | lf::kILogDData);
  }
  std::vector<uint8_t> item_header(lf::kInodeLogFormatSize, 0);
  PutLE16(&item_header, lf::kIlfType, lf::kItemInode);
  PutLE16(&item_header, lf::kIlfSize,
          static_cast<uint16_t>(with_region ? 3 : 2));
  PutLE32(&item_header, lf::kIlfFields, fields);
  PutLE16(&item_header, lf::kIlfAsize, 0);
  PutLE16(&item_header, lf::kIlfDsize,
          static_cast<uint16_t>(logged_region.size()));
  PutLE64(&item_header, lf::kIlfIno, ino);
  PutLE64(&item_header, lf::kIlfBlkno, cluster_start / 512);
  PutLE32(&item_header, lf::kIlfLen,
          static_cast<uint32_t>(cluster_bytes / 512));
  PutLE32(&item_header, lf::kIlfBoffset, static_cast<uint32_t>(boffset));
  txn.AddLogOp(std::move(item_header));
  txn.AddLogOp(std::move(log_dinode));
  if (with_region) txn.AddLogOp(std::move(logged_region));

  // On-disk inode: the prepared image, with the transaction LSN and a fresh
  // CRC (the fork bytes were written by the caller).
  PutBE64(inode.data(), di::kLsn, txn.lsn());
  UpdateXfsChecksum(inode.data(), inode.size(), di::kCrc);

  TransactionWrite write;
  write.offset = inode_offset;
  write.data = std::move(inode);
  write.ino = ino;
  write.inode_metadata = true;
  write.data_fork_changed =
      (ilog_fields & (lf::kILogDExt | lf::kILogDBRoot | lf::kILogDData)) != 0;
  write.what = "inode";
  txn.AddWrite(std::move(write));
  return OkStatus();
}

Result<XfsFileInfo> XfsFilesystem::SetFileTimes(uint64_t ino,
                                                const XfsTimestamp& atime,
                                                const XfsTimestamp& mtime,
                                                const XfsTimestamp& ctime) {
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume,
                   "filesystem is mounted read-only")
        .Ino(ino)
        .Metadata("inode");
  }
  std::lock_guard<std::mutex> lock(mutation_mutex_);
  auto txn = BeginTransaction();
  if (!txn.ok()) return txn.error();
  Status status =
      UpdateTimesInTransaction(txn.value(), ino, atime, mtime, ctime);
  if (!status.ok()) return status.error();
  status = CommitTransaction(txn.value());
  if (!status.ok()) return status.error();
  return GetFileInfo(ino);
}

Status XfsFilesystem::UpdateTimesInTransaction(XfsTransaction& txn, uint64_t ino,
                                               const XfsTimestamp& atime,
                                               const XfsTimestamp& mtime,
                                               const XfsTimestamp& ctime) {
  return UpdateInodeInTransaction(
      txn, ino, 0, {},
      [&](uint8_t* raw) -> Status {
        Status s = StoreInodeTimestamp(raw, di::kAtime, atime);
        if (!s.ok()) return s;
        s = StoreInodeTimestamp(raw, di::kMtime, mtime);
        if (!s.ok()) return s;
        return StoreInodeTimestamp(raw, di::kCtime, ctime);
      });
}

}  // namespace xfs
