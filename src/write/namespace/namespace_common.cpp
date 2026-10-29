// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Namespace mutation: module-private helpers.
//
// This file owns new-inode image construction, timestamp encoding and the
// application of reverse parent-pointer changes. It does not own directory
// entries, link counts or the transaction that publishes them.
#include "write/namespace/namespace_internal.h"

#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/ialloc.h"
#include "xfs/inode.h"
#include "xfs/log_format.h"
#include "xfs/ondisk.h"
#include "xfs/write.h"

namespace xfs {

namespace di = ondisk::inode;
namespace d = ondisk::dir;
namespace namespace_detail {

uint32_t NextGeneration() {
  static std::mt19937 rng(std::random_device{}());
  return rng();
}

// XFS_DIR3_FT_* for an inode mode (xfs_mode_to_ftype).
uint8_t FtypeForMode(uint16_t mode) {
  switch (mode & kSIfmt) {
    case kSIfreg: return d::kFtRegFile;
    case kSIfdir: return d::kFtDir;
    case kSIflnk: return d::kFtSymlink;
    case kSIfchr: return d::kFtChrdev;
    case kSIfblk: return d::kFtBlkdev;
    case kSIfifo: return d::kFtFifo;
    case kSIfsock: return d::kFtSock;
    default: return d::kFtUnknown;
  }
}

Status StoreTimestamp(uint8_t* inode, size_t field_offset,
                      const XfsTimestamp& ts) {
  return StoreInodeTimestamp(inode, field_offset, ts);
}

Status StoreModifiedTimes(uint8_t* inode, const XfsTimestamp& now) {
  Status s = StoreTimestamp(inode, di::kMtime, now);
  if (!s.ok()) return s;
  return StoreTimestamp(inode, di::kCtime, now);
}

// Builds a complete new v3 inode image. For a directory `sf` is the initial
// shortform image (header with the parent, no entries); regular files start
// with an empty extent fork. CRC and di_lsn are stamped by LogPreparedInode.
std::vector<uint8_t> BuildNewInode(const XfsGeometry& geo, uint64_t ino,
                                   uint16_t mode, uint32_t uid, uint32_t gid,
                                   uint32_t gen, const XfsTimestamp& now,
                                   const std::vector<uint8_t>* sf) {
  std::vector<uint8_t> image(geo.inode_size, 0);
  uint8_t* b = image.data();
  const bool is_dir = (mode & kSIfmt) == kSIfdir;
  PutBE16(b, di::kMagic, ondisk::kDinodeMagic);
  PutBE16(b, di::kMode, mode);
  b[di::kVersion] = 3;
  b[di::kFormat] = is_dir ? di::kFormatLocal : di::kFormatExtents;
  PutBE16(b, di::kOnlink, 0);
  PutBE32(b, di::kUid, uid);
  PutBE32(b, di::kGid, gid);
  PutBE32(b, di::kNlink, is_dir ? 2 : 1);
  PutBE32(b, di::kGen, gen);
  PutBE32(b, di::kNextUnlinked, kNullAgino);
  PutBE64(b, di::kChangecount, 1);  // inode_set_iversion(inode, 1)
  b[di::kForkoff] = 0;
  // xfs_dinode_verify/xfs_repair require a valid attr-fork format whenever
  // di_forkoff is zero: EXTENTS, not the zero-filled FMT_DEV value.
  b[di::kAformat] = di::kFormatExtents;
  // New inodes inherit the filesystem's modern inode flags exactly like
  // xfs_ialloc_setup_geometry's new_diflags2.
  uint64_t flags2 = 0;
  if (geo.features.bigtime) flags2 |= di::kDiFlag2Bigtime;
  if (geo.features.nrext64) flags2 |= di::kDiFlag2Nrext64;
  PutBE64(b, di::kFlags2, flags2);
  const bool bigtime = (flags2 & di::kDiFlag2Bigtime) != 0;
  const size_t stamp_fields[] = {di::kAtime, di::kMtime, di::kCtime,
                                 di::kCrtime};
  for (size_t field : stamp_fields) {
    if (!EncodeTimestamp(b + field, now, bigtime).ok()) return {};
  }
  PutBE64(b, di::kIno, ino);
  std::memcpy(b + di::kUuid, geo.meta_uuid.data(), 16);
  if (is_dir) {
    if (sf == nullptr || sf->empty()) return {};
    const size_t literal = geo.inode_size - di::kCoreSizeV3;
    if (sf->size() > literal) return {};
    std::memcpy(b + di::kCoreSizeV3, sf->data(), sf->size());
    PutBE64(b, di::kSize, sf->size());
  } else {
    PutBE64(b, di::kSize, 0);
  }
  PutBE64(b, di::kNblocks, 0);
  SetInodeDataExtentCount(b, 0);
  SetInodeAttrExtentCount(b, 0);
  PutBE32(b, di::kExtsize, 0);
  PutBE32(b, di::kCowextsize, 0);
  return image;
}

std::vector<uint8_t> BuildNewDirShortform(uint64_t parent_ino) {
  const bool wide = parent_ino > 0xffffffffull;
  std::vector<uint8_t> sf(wide ? d::kSfHdrSizeFixed : 6, 0);
  sf[d::kSfHdrCount] = 0;
  sf[d::kSfHdrI8Count] = wide ? 1 : 0;
  if (wide) {
    PutBE64(sf.data(), d::kSfHdrParent, parent_ino);
  } else {
    PutBE32(sf.data(), d::kSfHdrParent, static_cast<uint32_t>(parent_ino));
  }
  return sf;
}

uint32_t RawNextUnlinked(const std::vector<uint8_t>& raw) {
  return GetBE32(raw.data(), di::kNextUnlinked);
}

Result<PreparedInode> ApplyParentChange(
    XfsTransaction& txn, BlockAllocator& blocks, BlockDevice& device,
    const XfsGeometry& geo, const XfsInode& child, const XattrFork& fork,
    std::vector<uint8_t> raw, const ParentChange& change,
    bool missing_ok) {
  PreparedInode out;
  if (change.empty()) {
    out.image = std::move(raw);
    return out;
  }
  if (!geo.features.parent) {
    return XFS_ERR(XfsError::kInternal,
                   "parent-pointer mutation on a filesystem without the "
                   "feature")
        .Ino(child.ino)
        .Metadata("parent pointer");
  }
  if (change.remove.has_value() && change.add.has_value()) {
    return PrepareParentPointerReplace(txn, blocks, device, geo, child, fork,
                                       std::move(raw), *change.remove,
                                       *change.add, missing_ok);
  }
  if (change.add.has_value()) {
    return PrepareParentPointerAdd(txn, blocks, device, geo, child, fork,
                                   std::move(raw), *change.add);
  }
  return PrepareParentPointerRemove(txn, blocks, device, geo, child, fork,
                                    std::move(raw), *change.remove, missing_ok);
}

// Reads the child's attr fork for an existing inode.  New inodes that are
// still only in memory pass a default-constructed XattrFork instead.
Result<XattrFork> ReadChildAttrFork(BlockDevice& device,
                                    const XfsGeometry& geo,
                                    const XfsInode& child) {
  return ReadXattrFork(device, geo, child);
}

Status ParseChildImage(const XfsGeometry& geo, uint64_t ino,
                       const std::vector<uint8_t>& raw, XfsInode* out) {
  // The caller may already have changed link counts or the attr fork in the
  // in-memory image; the verifier must see a valid CRC.  LogPreparedInode
  // stamps the final CRC again before the image reaches the device.
  std::vector<uint8_t> image = raw;
  UpdateXfsChecksum(image.data(), image.size(), di::kCrc);
  return ParseInode(image.data(), image.size(), geo, ino, out);
}

// Combines an attr-fork preparation with the data-fork region the operation
// already prepared (a new directory/symlink), preferring the data region the
// attr writer emitted when a forkoff change forced it to relayout the fork.
void ChooseForkRegions(const PreparedInode& prepared, uint32_t* data_fields,
                       std::vector<uint8_t>* data_region,
                       uint32_t* attr_fields,
                       std::vector<uint8_t>* attr_region) {
  if (prepared.data_fields != 0 || !prepared.data_region.empty()) {
    *data_fields = prepared.data_fields;
    *data_region = prepared.data_region;
  }
  *attr_fields = prepared.attr_fields;
  *attr_region = prepared.attr_region;
}

}  // namespace namespace_detail
}  // namespace xfs
