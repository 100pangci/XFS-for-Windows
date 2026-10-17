#include "xfs/symlink.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "xfs/endian.h"
#include "xfs/extent.h"
#include "xfs/ondisk.h"
#include "xfs/verifier.h"

namespace xfs {
namespace {

namespace di = ondisk::inode;
namespace sym = ondisk::symlink;

}  // namespace

Result<std::string> ReadSymlinkTarget(BlockDevice& dev, const XfsGeometry& geo,
                                      const XfsInode& inode) {
  if (!inode.IsSymlink()) {
    return XFS_ERR(XfsError::kNotSymlink, "inode is not a symlink")
        .Ino(inode.ino);
  }
  if (inode.size > ondisk::kSymlinkMaxLen) {
    return XFS_ERR(XfsError::kCorruptInode, "symlink target too long")
        .Ino(inode.ino)
        .Actual(inode.size);
  }

  std::string target(static_cast<size_t>(inode.size), '\0');

  if (inode.format == di::kFormatLocal) {
    if (inode.size > inode.data_fork.size()) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "inline symlink target exceeds the data fork")
          .Ino(inode.ino);
    }
    std::memcpy(target.data(), inode.data_fork.data(),
                static_cast<size_t>(inode.size));
    return target;
  }
  if (inode.format != di::kFormatExtents &&
      inode.format != di::kFormatBtree) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "unsupported symlink data fork format")
        .Ino(inode.ino)
        .Actual(inode.format);
  }

  std::vector<XfsExtent> extents;
  Status s = LoadDataForkExtents(dev, geo, inode, &extents);
  if (!s.ok()) return s.error();

  const uint64_t hdr_size = sym::kHeaderSize;
  const uint64_t block_size = geo.block_size;
  uint64_t remaining = inode.size;
  uint64_t target_offset = 0;

  for (const XfsExtent& e : extents) {
    if (remaining == 0) break;
    uint64_t extent_bytes = e.blockcount * block_size;
    if (extent_bytes <= hdr_size) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "symlink extent is smaller than its header")
          .Ino(inode.ino);
    }
    uint64_t chunk = std::min(remaining, extent_bytes - hdr_size);
    if (e.unwritten) {
      return XFS_ERR(XfsError::kCorruptInode, "symlink extent is unwritten")
          .Ino(inode.ino);
    }

    uint64_t dev_off = geo.FsbToByte(e.startblock);
    std::vector<uint8_t> extent_buf(static_cast<size_t>(extent_bytes));
    if (!dev.Read(dev_off, extent_buf.data(), extent_buf.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to read symlink extent")
          .Ino(inode.ino)
          .Offset(dev_off);
    }

    // Header checks (magic, uuid, block number, owner, lengths).
    if (GetBE32(extent_buf.data(), sym::kMagic) != ondisk::kSymlinkMagic) {
      return XFS_ERR(XfsError::kBadMagic, "symlink magic mismatch")
          .Ino(inode.ino)
          .Metadata("symlink")
          .Actual(GetBE32(extent_buf.data(), sym::kMagic));
    }
    uint32_t sl_offset = GetBE32(extent_buf.data(), sym::kOffset);
    uint32_t sl_bytes = GetBE32(extent_buf.data(), sym::kBytes);
    uint64_t sl_owner = GetBE64(extent_buf.data(), sym::kOwner);
    uint64_t sl_blkno = GetBE64(extent_buf.data(), sym::kBlkno);
    if (sl_offset != hdr_size || sl_bytes != chunk || sl_owner != inode.ino) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "symlink header does not match (offset/length/owner)")
          .Ino(inode.ino)
          .Metadata("symlink")
          .Expected(inode.ino)
          .Actual(sl_owner);
    }
    if (sl_blkno != dev_off / 512) {
      return XFS_ERR(XfsError::kBadBlockNo, "symlink blkno mismatch")
          .Ino(inode.ino)
          .Metadata("symlink")
          .Expected(dev_off / 512)
          .Actual(sl_blkno);
    }
    if (std::memcmp(extent_buf.data() + sym::kUuid, geo.meta_uuid.data(),
                    16) != 0) {
      return XFS_ERR(XfsError::kBadUuid, "symlink uuid mismatch").Ino(inode.ino);
    }
    Status crc = VerifyChecksum(extent_buf.data(), extent_buf.size(),
                                sym::kCrc, "symlink");
    if (!crc.ok()) {
      auto err = crc.error();
      err.ino = inode.ino;
      return err;
    }

    std::memcpy(target.data() + target_offset,
                extent_buf.data() + hdr_size, static_cast<size_t>(chunk));
    target_offset += chunk;
    remaining -= chunk;
  }
  if (remaining != 0) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "symlink data is shorter than the inode size")
        .Ino(inode.ino);
  }
  return target;
}

}  // namespace xfs
