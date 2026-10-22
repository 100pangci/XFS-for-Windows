// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/inode.h"

#include <chrono>
#include <cstring>
#include <vector>

#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/verifier.h"

namespace xfs {
namespace {

namespace di = ondisk::inode;

uint64_t Litino(const XfsGeometry& geo) {
  return geo.inode_size - di::kCoreSizeV3;
}

uint64_t ForkoffBytes(const XfsInode& inode) {
  return static_cast<uint64_t>(inode.forkoff) * 8;
}

Status DecodeAndCheckTimestamps(const uint8_t* buf, XfsInode* out) {
  out->atime = DecodeTimestamp(buf + di::kAtime, out->bigtime);
  out->mtime = DecodeTimestamp(buf + di::kMtime, out->bigtime);
  out->ctime = DecodeTimestamp(buf + di::kCtime, out->bigtime);
  out->crtime = DecodeTimestamp(buf + di::kCrtime, out->bigtime);
  return OkStatus();
}

}  // namespace

XfsTimestamp DecodeTimestamp(const uint8_t* field, bool bigtime) {
  XfsTimestamp ts;
  if (bigtime) {
    // Unsigned 64-bit nanoseconds since the legacy minimum timestamp
    // (1901-12-13, i.e. Unix epoch + 2^31 seconds).
    const uint64_t raw = GetBE64(field);
    const uint64_t secs = raw / kNsecPerSec;
    ts.nsec = static_cast<uint32_t>(raw % kNsecPerSec);
    ts.sec = static_cast<int64_t>(secs) - kBigtimeEpochOffset;
  } else {
    ts.sec = static_cast<int32_t>(GetBE32(field));
    ts.nsec = GetBE32(field + 4);
  }
  return ts;
}

Status EncodeTimestamp(uint8_t* field, const XfsTimestamp& ts, bool bigtime) {
  if (ts.nsec >= kNsecPerSec) {
    return XFS_ERR(XfsError::kOutOfRange,
                   "timestamp nanoseconds outside [0, 1e9)")
        .Metadata("inode")
        .Actual(ts.nsec);
  }
  if (bigtime) {
    if (ts.sec < kBigtimeTimeMinUnix || ts.sec > kBigtimeTimeMaxUnix) {
      return XFS_ERR(XfsError::kOutOfRange,
                     "timestamp outside the XFS bigtime range")
          .Metadata("inode")
          .Actual(static_cast<uint64_t>(ts.sec));
    }
    const uint64_t ondisk_seconds =
        static_cast<uint64_t>(ts.sec + kBigtimeEpochOffset);
    StoreBE64(field, ondisk_seconds * kNsecPerSec + ts.nsec);
    return OkStatus();
  }
  if (ts.sec < kLegacyTimeMin || ts.sec > kLegacyTimeMax) {
    return XFS_ERR(XfsError::kOutOfRange,
                   "timestamp outside the legacy 32-bit XFS range")
        .Metadata("inode")
        .Actual(static_cast<uint64_t>(ts.sec));
  }
  StoreBE32(field, static_cast<uint32_t>(ts.sec));
  StoreBE32(field + 4, ts.nsec);
  return OkStatus();
}

bool InodeImageHasBigtime(const uint8_t* inode) {
  return (GetBE64(inode, di::kFlags2) & di::kDiFlag2Bigtime) != 0;
}

bool InodeImageHasNrext64(const uint8_t* inode) {
  return (GetBE64(inode, di::kFlags2) & di::kDiFlag2Nrext64) != 0;
}

Status StoreInodeTimestamp(uint8_t* inode, size_t field_offset,
                           const XfsTimestamp& ts) {
  return EncodeTimestamp(inode + field_offset, ts,
                         InodeImageHasBigtime(inode));
}

Status UpgradeInodeTimestampsToBigtime(uint8_t* inode, size_t inode_size,
                                       const XfsGeometry& geo) {
  if (inode_size < di::kCoreSizeV3 || !geo.features.bigtime ||
      InodeImageHasBigtime(inode)) {
    return OkStatus();
  }
  // Decode with the legacy encoding before changing the flag.
  const size_t fields[] = {di::kAtime, di::kMtime, di::kCtime, di::kCrtime};
  XfsTimestamp ts[4];
  for (size_t i = 0; i < 4; ++i) ts[i] = DecodeTimestamp(inode + fields[i], false);

  PutBE64(inode, di::kFlags2,
          GetBE64(inode, di::kFlags2) | di::kDiFlag2Bigtime);
  for (size_t i = 0; i < 4; ++i) {
    Status s = EncodeTimestamp(inode + fields[i], ts[i], true);
    if (!s.ok()) return s;
  }
  return OkStatus();
}

uint64_t InodeDataExtentCount(const uint8_t* inode) {
  if (InodeImageHasNrext64(inode)) return GetBE64(inode, di::kBigNextents);
  return GetBE32(inode, di::kNextents);
}

uint32_t InodeAttrExtentCount(const uint8_t* inode) {
  if (InodeImageHasNrext64(inode)) return GetBE32(inode, di::kBigAnextents);
  return GetBE16(inode, di::kAnextents);
}

void SetInodeDataExtentCount(uint8_t* inode, uint64_t count) {
  if (InodeImageHasNrext64(inode)) {
    PutBE64(inode, di::kBigNextents, count);
  } else {
    PutBE32(inode, di::kNextents, static_cast<uint32_t>(count));
  }
}

void SetInodeAttrExtentCount(uint8_t* inode, uint32_t count) {
  if (InodeImageHasNrext64(inode)) {
    PutBE32(inode, di::kBigAnextents, count);
    PutBE16(inode, di::kAnextents, 0);  // di_nrext64_pad
  } else {
    PutBE16(inode, di::kAnextents, static_cast<uint16_t>(count));
  }
}

XfsTimestamp CurrentTimestamp() {
  using namespace std::chrono;
  const auto since_epoch = system_clock::now().time_since_epoch();
  const auto secs = duration_cast<seconds>(since_epoch);
  XfsTimestamp ts;
  ts.sec = secs.count();
  ts.nsec = static_cast<uint32_t>(
      duration_cast<nanoseconds>(since_epoch - secs).count());
  return ts;
}

Status ParseInode(const uint8_t* buf, size_t len, const XfsGeometry& geo,
                  uint64_t ino, XfsInode* out, bool allow_free_inode) {
  if (len < geo.inode_size) {
    return XFS_ERR(XfsError::kCorruptInode, "inode buffer too small")
        .Ino(ino)
        .Metadata("inode");
  }
  if (GetBE16(buf, di::kMagic) != ondisk::kDinodeMagic) {
    return XFS_ERR(XfsError::kBadMagic, "inode magic mismatch")
        .Ino(ino)
        .Metadata("inode")
        .Expected(ondisk::kDinodeMagic)
        .Actual(GetBE16(buf, di::kMagic));
  }

  XfsInode inode;
  inode.ino = ino;
  inode.mode = GetBE16(buf, di::kMode);
  inode.version = GetU8(buf, di::kVersion);
  inode.format = GetU8(buf, di::kFormat);
  inode.uid = GetBE32(buf, di::kUid);
  inode.gid = GetBE32(buf, di::kGid);
  inode.nlink = GetBE32(buf, di::kNlink);
  inode.projid = (static_cast<uint32_t>(GetBE16(buf, di::kProjidHi)) << 16) |
                 GetBE16(buf, di::kProjidLo);
  inode.forkoff = GetU8(buf, di::kForkoff);
  inode.aformat = static_cast<uint8_t>(GetS8(buf, di::kAformat));
  inode.dmevmask = GetBE32(buf, di::kDmevmask);
  inode.dmstate = GetBE16(buf, di::kDmstate);
  inode.flags = GetBE16(buf, di::kFlags);
  inode.gen = GetBE32(buf, di::kGen);
  inode.next_unlinked = GetBE32(buf, di::kNextUnlinked);
  inode.changecount = GetBE64(buf, di::kChangecount);
  inode.lsn = GetBE64(buf, di::kLsn);
  inode.flags2 = GetBE64(buf, di::kFlags2);
  inode.cowextsize = GetBE32(buf, di::kCowextsize);
  inode.extsize = GetBE32(buf, di::kExtsize);
  inode.size = GetBE64(buf, di::kSize);
  inode.nblocks = GetBE64(buf, di::kNblocks);

  if (inode.version != 3) {
    return XFS_ERR(XfsError::kNotV5,
                   "v5 filesystems require v3 inodes; found version " +
                       std::to_string(static_cast<int>(inode.version)))
        .Ino(ino)
        .Metadata("inode");
  }

  inode.bigtime = (inode.flags2 & di::kDiFlag2Bigtime) != 0;
  inode.nrext64 = (inode.flags2 & di::kDiFlag2Nrext64) != 0;
  if (inode.bigtime && !geo.features.bigtime) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "inode has bigtime flag but the filesystem does not")
        .Ino(ino)
        .Metadata("inode");
  }
  if (inode.nrext64 && !geo.features.nrext64) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "inode has NREXT64 flag but the filesystem does not")
        .Ino(ino)
        .Metadata("inode");
  }

  // Unknown flags2 bits are not safe to ignore.
  constexpr uint64_t kFlags2Any = di::kDiFlag2Dax | di::kDiFlag2Reflink |
                                  di::kDiFlag2Cowextsize |
                                  di::kDiFlag2Bigtime | di::kDiFlag2Nrext64 |
                                  di::kDiFlag2Metadata;
  if (inode.flags2 & ~kFlags2Any) {
    return XFS_ERR(XfsError::kCorruptInode, "unknown inode flags2 bits set")
        .Ino(ino)
        .Metadata("inode")
        .Actual(inode.flags2);
  }
  if ((inode.flags2 & di::kDiFlag2Metadata) != 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "metadata inode encountered (metadir feature)")
        .Ino(ino)
        .Metadata("inode");
  }

  if (inode.nrext64) {
    const uint64_t big_nextents = GetBE64(buf, di::kBigNextents);
    if ((big_nextents >> 32) != 0) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "inode has more data extents than this build can address")
          .Ino(ino)
          .Metadata("inode")
          .Actual(big_nextents);
    }
    inode.nextents = static_cast<uint32_t>(big_nextents);
    inode.anextents = GetBE32(buf, di::kBigAnextents);
    // xfs_dinode_verify_nrext64: the unused counter slot must be zero.
    if (GetBE16(buf, di::kAnextents) != 0) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "NREXT64 inode has a nonzero di_nrext64_pad")
          .Ino(ino)
          .Metadata("inode")
          .Actual(GetBE16(buf, di::kAnextents));
    }
  } else {
    inode.nextents = GetBE32(buf, di::kNextents);
    inode.anextents = GetBE16(buf, di::kAnextents);
    if (inode.version >= 3 && GetBE64(buf, di::kV3Pad) != 0) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "v3 inode without NREXT64 has nonzero padding")
          .Ino(ino)
          .Metadata("inode");
    }
  }

  Status s = DecodeAndCheckTimestamps(buf, &inode);
  if (!s.ok()) return s;

  // The v3 inode CRC covers the entire inode (sb_inodesize), including the
  // data/attr fork areas, not just the 176-byte core.
  s = VerifyChecksum(buf, geo.inode_size, di::kCrc, "inode");
  if (!s.ok()) {
    auto err = s.error();
    err.ino = ino;
    return err;
  }

  // di_ino / di_uuid identity.
  uint64_t di_ino = GetBE64(buf, di::kIno);
  if (di_ino != ino) {
    return XFS_ERR(XfsError::kCorruptInode, "inode number in di_ino mismatch")
        .Ino(ino)
        .Metadata("inode")
        .Expected(ino)
        .Actual(di_ino);
  }
  if (std::memcmp(buf + di::kUuid, geo.meta_uuid.data(), 16) != 0) {
    return XFS_ERR(XfsError::kBadUuid, "inode uuid mismatch")
        .Ino(ino)
        .Metadata("inode");
  }

  // A free inode is a legal on-disk state: xfs_difree zeroes the core (mode 0,
  // nlink 0) and the kernel's xfs_dinode_verify only applies the mode/type,
  // extent-count, fork-offset and realtime checks when di_mode is non-zero
  // (libxfs/xfs_inode_buf.c). Recovery replays freed inodes (unlink, rmdir,
  // rename-replace) and asks for them here; the identity fields validated above
  // (magic, version, checksum, di_ino, uuid) are the ones that hold for every
  // inode, and there are no forks to decode. Readers keep refusing mode 0: a
  // freed inode is not a file.
  if (allow_free_inode && inode.mode == 0) return OkStatus();

  // Mode sanity.
  uint16_t type = inode.mode & kSIfmt;
  switch (type) {
    case kSIfreg:
    case kSIfdir:
    case kSIflnk:
    case kSIfchr:
    case kSIfblk:
    case kSIfifo:
    case kSIfsock:
      break;
    default:
      return XFS_ERR(XfsError::kCorruptInode, "invalid mode/type in inode")
          .Ino(ino)
          .Metadata("inode")
          .Actual(inode.mode);
  }

  if (inode.nblocks > geo.dblocks) {
    return XFS_ERR(XfsError::kCorruptInode, "inode nblocks exceeds filesystem")
        .Ino(ino)
        .Metadata("inode")
        .Actual(inode.nblocks);
  }

  // Fork layout.
  const uint64_t litino = Litino(geo);
  const uint64_t forkoff_bytes = ForkoffBytes(inode);
  if (inode.forkoff != 0 && forkoff_bytes >= litino) {
    return XFS_ERR(XfsError::kCorruptInode, "forkoff out of range")
        .Ino(ino)
        .Metadata("inode")
        .Expected(litino)
        .Actual(forkoff_bytes);
  }
  const size_t data_fork_size =
      static_cast<size_t>(inode.forkoff ? forkoff_bytes : litino);
  const size_t attr_fork_size =
      static_cast<size_t>(inode.forkoff ? litino - forkoff_bytes : 0);

  const uint8_t* dptr = buf + di::kCoreSizeV3;
  inode.data_fork.assign(dptr, dptr + data_fork_size);
  if (attr_fork_size) {
    inode.attr_fork.assign(dptr + forkoff_bytes,
                           dptr + forkoff_bytes + attr_fork_size);
  }

  // Data fork format validation.
  auto check_fork = [&](uint8_t fmt, uint32_t nextents, size_t fork_size,
                        const char* name) -> Status {
    switch (fmt) {
      case di::kFormatDev:
      case di::kFormatLocal:
        break;
      case di::kFormatExtents:
        if (static_cast<uint64_t>(nextents) * 16 > fork_size) {
          return XFS_ERR(XfsError::kCorruptInode,
                         std::string("extent count exceeds ") + name +
                             " fork size")
              .Ino(ino)
              .Metadata("inode")
              .Actual(nextents);
        }
        break;
      case di::kFormatBtree:
        if (fork_size < ondisk::btree::kBmdrSize) {
          return XFS_ERR(XfsError::kCorruptInode,
                         std::string(name) + " btree fork too small")
              .Ino(ino)
              .Metadata("inode");
        }
        break;
      case di::kFormatUuid:
        return XFS_ERR(XfsError::kCorruptInode,
                       std::string("unused UUID ") + name + " fork format")
            .Ino(ino)
            .Metadata("inode");
      case di::kFormatMetaBtree:
        return XFS_ERR(XfsError::kUnsupportedFeature,
                       std::string("metadir ") + name + " fork format")
            .Ino(ino)
            .Metadata("inode");
      default:
        return XFS_ERR(XfsError::kCorruptInode,
                       std::string("invalid ") + name + " fork format")
            .Ino(ino)
            .Metadata("inode")
            .Actual(fmt);
    }
    return OkStatus();
  };

  s = check_fork(inode.format, inode.nextents, data_fork_size, "data");
  if (!s.ok()) return s;
  if (inode.format == di::kFormatBtree) {
    // The embedded bmbt root: level/numrecs must fit the fork.
    uint16_t level = GetBE16(inode.data_fork.data(), 0);
    uint16_t numrecs = GetBE16(inode.data_fork.data(), 2);
    size_t maxrecs = (data_fork_size - ondisk::btree::kBmdrSize) /
                     (ondisk::btree::kBmbtKeySize + ondisk::btree::kBmbtPtrSize);
    if (level > ondisk::btree::kMaxTraversalLevels || numrecs > maxrecs) {
      return XFS_ERR(XfsError::kCorruptInode, "bmbt root out of range")
          .Ino(ino)
          .Metadata("inode")
          .Expected(maxrecs)
          .Actual(numrecs);
    }
  }

  if (inode.forkoff != 0) {
    s = check_fork(inode.aformat, inode.anextents, attr_fork_size, "attr");
    if (!s.ok()) return s;
    if (inode.aformat == di::kFormatBtree) {
      uint16_t level = GetBE16(inode.attr_fork.data(), 0);
      uint16_t numrecs = GetBE16(inode.attr_fork.data(), 2);
      size_t maxrecs = (attr_fork_size - ondisk::btree::kBmdrSize) /
                       (ondisk::btree::kBmbtKeySize + ondisk::btree::kBmbtPtrSize);
      if (level > ondisk::btree::kMaxTraversalLevels || numrecs > maxrecs) {
        return XFS_ERR(XfsError::kCorruptInode, "attr bmbt root out of range")
            .Ino(ino)
            .Metadata("inode");
      }
    }
  } else if (inode.aformat != 0) {
    // aformat is meaningless without an attr fork; mkfs leaves it zero.
    if (inode.anextents != 0) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "attr extent count without attr fork")
          .Ino(ino)
          .Metadata("inode");
    }
  }

  // LOCAL format data must fit.
  if (inode.format == di::kFormatLocal && inode.size > data_fork_size) {
    return XFS_ERR(XfsError::kCorruptInode, "local data fork exceeds fork size")
        .Ino(ino)
        .Metadata("inode")
        .Expected(data_fork_size)
        .Actual(inode.size);
  }
  if (inode.forkoff != 0 && inode.aformat == di::kFormatLocal) {
    // Shortform attr list: first 2 bytes hold totsize (including header).
    if (inode.attr_fork.size() < 2) {
      return XFS_ERR(XfsError::kCorruptInode, "shortform attr fork too small")
          .Ino(ino)
          .Metadata("inode");
    }
    uint16_t totsize = GetBE16(inode.attr_fork.data(), 0);
    if (totsize > attr_fork_size) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "shortform attr totsize exceeds fork size")
          .Ino(ino)
          .Metadata("inode")
          .Expected(attr_fork_size)
          .Actual(totsize);
    }
  }

  *out = std::move(inode);
  return OkStatus();
}

Result<XfsInode> ReadInode(BlockDevice& dev, const XfsGeometry& geo,
                           uint64_t ino) {
  if (!geo.InoValid(ino)) {
    return XFS_ERR(XfsError::kOutOfRange, "inode number out of range")
        .Ino(ino)
        .Metadata("inode");
  }
  uint64_t off = geo.InoToByteOffset(ino);
  std::vector<uint8_t> buf(geo.inode_size);
  if (!dev.Read(off, buf.data(), buf.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read inode")
        .Ino(ino)
        .Offset(off)
        .Metadata("inode");
  }
  XfsInode out;
  Status s = ParseInode(buf.data(), buf.size(), geo, ino, &out);
  if (!s.ok()) return s.error();
  return out;
}

}  // namespace xfs
