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
        // Timestamps are (seconds, nanoseconds) pairs: convert each half.
        if (field.offset == di::kAtime || field.offset == di::kMtime ||
            field.offset == di::kCtime || field.offset == di::kCrtime) {
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

// Stores a timestamp into the legacy 32:32 on-disk encoding (big-endian) or
// the log dinode's host-order copy of the same pair.
void StoreTimestampBE(uint8_t* field, const XfsTimestamp& ts) {
  StoreBE32(field, static_cast<uint32_t>(ts.sec));
  StoreBE32(field + 4, ts.nsec);
}
void StoreTimestampLE(uint8_t* field, const XfsTimestamp& ts) {
  StoreLE32(field, static_cast<uint32_t>(ts.sec));
  StoreLE32(field + 4, ts.nsec);
}

}  // namespace

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
  XfsInode parsed;
  Status s =
      ParseInode(inode.data(), inode.size(), geometry_, ino, &parsed);
  if (!s.ok()) return s;
  if (parsed.bigtime) {
    // The timestamp encoding differs (64-bit nanoseconds); bigtime is refused
    // for read-write mounts, so this is a bug or a policy hole, not a case to
    // guess about.
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "cannot update timestamps of a bigtime inode")
        .Ino(ino)
        .Metadata("inode");
  }

  // Log item: one format header region plus the inode core region, exactly the
  // two operations a Linux inode item is written as. The named buffer is the
  // inode cluster (like xfs_imap): replay reads it through the inode buffer
  // verifier and applies the logged core at ilf_boffset inside it.
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
    // The kernel's xfs_imap refuses this as well ("inode cluster out of
    // range"): the last cluster of an AG must not run past its end.
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "inode cluster buffer is out of range")
        .Ino(ino)
        .Ag(agno)
        .Metadata("inode");
  }

  std::vector<uint8_t> log_dinode;
  CopyCoreToLogDinode(inode.data(), inode.size(), &log_dinode);
  StoreTimestampLE(log_dinode.data() + di::kAtime, atime);
  StoreTimestampLE(log_dinode.data() + di::kMtime, mtime);
  StoreTimestampLE(log_dinode.data() + di::kCtime, ctime);

  std::vector<uint8_t> item_header(lf::kInodeLogFormatSize, 0);
  PutLE16(&item_header, lf::kIlfType, lf::kItemInode);
  // Number of logged regions of this item: the format header and the core
  // (recovery uses it as ri_total, not as a byte or word count).
  PutLE16(&item_header, lf::kIlfSize, 2);
  PutLE32(&item_header, lf::kIlfFields, lf::kILogCore);
  PutLE16(&item_header, lf::kIlfAsize, 0);
  PutLE16(&item_header, lf::kIlfDsize, 0);
  PutLE64(&item_header, lf::kIlfIno, ino);
  PutLE64(&item_header, lf::kIlfBlkno,
          static_cast<uint64_t>(cluster_start) / 512);  // daddr of the cluster
  PutLE32(&item_header, lf::kIlfLen,
          static_cast<uint32_t>(cluster_bytes / 512));
  PutLE32(&item_header, lf::kIlfBoffset, static_cast<uint32_t>(boffset));
  txn.AddLogOp(std::move(item_header));
  txn.AddLogOp(std::move(log_dinode));

  // On-disk inode: same core fields with the new times, the transaction LSN
  // and a fresh CRC. Forks are preserved because the buffer is the raw inode.
  StoreTimestampBE(inode.data() + di::kAtime, atime);
  StoreTimestampBE(inode.data() + di::kMtime, mtime);
  StoreTimestampBE(inode.data() + di::kCtime, ctime);
  PutBE64(inode.data(), di::kLsn, txn.lsn());
  UpdateXfsChecksum(inode.data(), inode.size(), di::kCrc);

  TransactionWrite write;
  write.offset = inode_offset;
  write.data = std::move(inode);
  write.ino = ino;
  write.inode_metadata = true;
  write.what = "inode";
  txn.AddWrite(std::move(write));
  return OkStatus();
}

}  // namespace xfs
