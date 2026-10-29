// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Namespace mutation: symlink creation.
//
// Owner: xfs_symlink. The child inode (inline target or remote v5 symlink
// blocks), forward dirent, parent pointer and parent timestamps commit in one
// transaction; remote payload blocks are written before the record commits.

#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "xfs/ag.h"
#include "xfs/alloc.h"
#include "xfs/bmap.h"
#include "xfs/crash.h"
#include "xfs/crc32c.h"
#include "xfs/dir_write.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/ialloc.h"
#include "xfs/log.h"
#include "xfs/log_format.h"
#include "xfs/ondisk.h"
#include "xfs/parent.h"
#include "xfs/write.h"
#include "write/namespace/namespace_internal.h"

namespace xfs {

namespace di = ondisk::inode;
namespace d = ondisk::dir;
namespace sym = ondisk::symlink;
namespace lf = logfmt;
using namespace_detail::ParentChange;
using namespace_detail::ApplyParentChange;
using namespace_detail::BuildNewInode;
using namespace_detail::ChooseForkRegions;
using namespace_detail::NextGeneration;
using namespace_detail::ParseChildImage;
using namespace_detail::StoreModifiedTimes;

// ---------------------------------------------------------------------------
// Symlink creation (xfs_symlink / xfs_symlink_write_target)
// ---------------------------------------------------------------------------

namespace {

// Remote symlink capacity per block: the v5 xfs_dsymlink_hdr eats 56 bytes
// (XFS_SYMLINK_BUF_SPACE).
uint32_t SymlinkBlocks(uint32_t block_size, uint32_t pathlen) {
  const uint32_t per_block =
      block_size > sym::kHeaderSize ? block_size - sym::kHeaderSize : 0;
  if (per_block == 0) return 0;
  return (pathlen + per_block - 1) / per_block;
}

}  // namespace

Result<uint64_t> XfsFilesystem::CreateSymlink(uint64_t parent_ino,
                                              std::string_view name,
                                              std::string_view target,
                                              uint16_t mode, uint32_t uid,
                                              uint32_t gid) {
  std::unique_lock<std::shared_mutex> namespace_barrier(write_barrier_);
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume, "filesystem is mounted read-only")
        .Metadata("symlink");
  }
  // xfs_symlink: pathlen >= XFS_SYMLINK_MAXLEN is ENAMETOOLONG; a NUL is
  // impossible through the syscall interface and rejected by xfs_repair, so
  // never store one.
  if (target.empty() || target.size() >= ondisk::kSymlinkMaxLen ||
      target.find('\0') != std::string_view::npos) {
    return XFS_ERR(XfsError::kNameTooLong, "invalid symlink target length")
        .Actual(target.size())
        .Metadata("symlink");
  }
  mode = static_cast<uint16_t>((mode & 07777) | kSIflnk);
  Status capacity = EnsureInodeCapacity(geometry_.InoAgno(parent_ino));
  if (!capacity.ok()) return capacity.error();

  uint64_t child_ino = 0;
  Status status = RunTransaction([&](XfsTransaction& txn) -> Status {
    auto parent = OpenInode(parent_ino);
    if (!parent.ok()) return parent.error();
    if (!parent.value().IsDir()) {
      return XFS_ERR(XfsError::kNotDirectory,
                     "symlink parent is not a directory")
          .Ino(parent_ino)
          .Metadata("symlink");
    }
    if ((parent.value().flags & di::kDiFlagImmutable) != 0) {
      return XFS_ERR(XfsError::kPermissionDenied, "parent is immutable")
          .Ino(parent_ino)
          .Metadata("symlink");
    }
    if ((parent.value().flags & di::kDiFlagNosymlinks) != 0) {
      // xfs_symlink: XFS_DIFLAG_NOSYMLINKS forbids new symlinks (EPERM).
      return XFS_ERR(XfsError::kPermissionDenied,
                     "directory does not allow symlinks")
          .Ino(parent_ino)
          .Metadata("symlink");
    }
    auto existing = LookupName(parent_ino, name);
    if (existing.ok()) {
      return XFS_ERR(XfsError::kAlreadyExists, "name already exists")
          .Ino(parent_ino)
          .Metadata("symlink");
    }
    if (existing.error().code != XfsError::kNotFound) {
      return existing.error();
    }

    BlockAllocator blocks(&txn, device_.get(), &geometry_);
    InodeAllocator inodes(&txn, &blocks, device_.get(), &superblock_,
                          &geometry_);
    Status s = inodes.AllocateInode(geometry_.InoAgno(parent_ino),
                                    geometry_.InoAgino(parent_ino), &child_ino);
    if (!s.ok()) return s;
    const XfsTimestamp now = CurrentTimestamp();
    const uint32_t gen = NextGeneration();
    std::vector<uint8_t> child = BuildNewInode(geometry_, child_ino, mode, uid,
                                               gid, gen, now, nullptr);
    if (child.empty()) {
      return XFS_ERR(XfsError::kInternal, "cannot build the new inode")
          .Ino(child_ino);
    }

    uint32_t child_attr_fields = 0;
    std::vector<uint8_t> child_attr_region;
    auto finish_child = [&](uint32_t data_fields,
                            std::vector<uint8_t> data_region) -> Status {
      uint32_t effective_data = data_fields;
      std::vector<uint8_t> effective_region = std::move(data_region);
      if (geometry_.features.parent) {
        XfsInode parsed;
        Status parse = ParseChildImage(geometry_, child_ino, child, &parsed);
        if (!parse.ok()) return parse;
        XattrFork empty_fork;
        empty_fork.format = XattrForkFormat::kNone;
        ParentChange change;
        change.add = ParentPointer{parent_ino, parent.value().gen,
                                   std::string(name)};
        auto prepared = ApplyParentChange(txn, blocks, *device_, geometry_,
                                          parsed, empty_fork,
                                          std::move(child), change);
        if (!prepared.ok()) return prepared.error();
        child = std::move(prepared.value().image);
        if (prepared.value().data_fields != 0 ||
            !prepared.value().data_region.empty()) {
          effective_data = prepared.value().data_fields;
          effective_region = prepared.value().data_region;
        }
        child_attr_fields = prepared.value().attr_fields;
        child_attr_region = prepared.value().attr_region;
      }
      return LogPreparedInode(txn, child_ino, std::move(child), effective_data,
                              effective_region, child_attr_fields,
                              child_attr_region);
    };

    const size_t local_capacity =
        static_cast<size_t>(geometry_.inode_size) - di::kCoreSizeV3;
    if (target.size() <= local_capacity) {
      // xfs_symlink_write_target: inline in the literal area (LOCAL fork).
      child[di::kFormat] = di::kFormatLocal;
      PutBE64(child.data(), di::kSize, target.size());
      std::memcpy(child.data() + di::kCoreSizeV3, target.data(), target.size());
      std::vector<uint8_t> fork_region(target.begin(), target.end());
      s = finish_child(lf::kILogDData, std::move(fork_region));
      if (!s.ok()) return s;
    } else {
      // Remote: one contiguous run of xfs_symlink_blocks() blocks (the
      // allocation is refused before any write when the AG cannot satisfy
      // it), one v5 xfs_dsymlink_hdr, the target bytes.
      const uint32_t block_size = geometry_.block_size;
      const uint32_t need =
          SymlinkBlocks(block_size, static_cast<uint32_t>(target.size()));
      if (need == 0) {
        return XFS_ERR(XfsError::kUnsupportedGeometry,
                       "block size cannot hold a symlink header")
            .Ino(child_ino);
      }
      Allocation alloc;
      s = blocks.AllocateOwned(
          geometry_.InoAgno(parent_ino), need,
          RmapOwner::Data(child_ino, 0, false), &alloc);
      if (!s.ok()) return s;
      ondisk::BmbtRec rec;
      rec.startoff = 0;
      rec.startblock = alloc.fsb;
      rec.blockcount = alloc.len;
      rec.unwritten = false;
      uint64_t l0 = 0, l1 = 0;
      ondisk::EncodeBmbtRec(rec, &l0, &l1);
      std::vector<uint8_t> fork_region(16, 0);
      PutBE64(fork_region.data(), 0, l0);
      PutBE64(fork_region.data(), 8, l1);
      child[di::kFormat] = di::kFormatExtents;
      SetInodeDataExtentCount(child.data(), 1);
      PutBE64(child.data(), di::kNblocks, alloc.len);
      PutBE64(child.data(), di::kSize, target.size());
      std::memcpy(child.data() + di::kCoreSizeV3, fork_region.data(), 16);
      s = finish_child(lf::kILogDExt, std::move(fork_region));
      if (!s.ok()) return s;

      // The symlink payload buffer: header + target bytes, one header per
      // contiguous extent (xfs_symlink_hdr_set).
      std::vector<uint8_t> image(static_cast<size_t>(alloc.len) * block_size,
                                 0);
      PutBE32(image.data(), sym::kMagic, ondisk::kSymlinkMagic);
      PutBE32(image.data(), sym::kOffset, 0);
      PutBE32(image.data(), sym::kBytes,
              static_cast<uint32_t>(target.size()));
      std::memcpy(image.data() + sym::kUuid, geometry_.meta_uuid.data(), 16);
      PutBE64(image.data(), sym::kOwner, child_ino);
      PutBE64(image.data(), sym::kBlkno, geometry_.FsbToByte(alloc.fsb) / 512);
      std::memcpy(image.data() + sym::kHeaderSize, target.data(),
                  target.size());
      s = txn.AddMetadataBuffer(geometry_.FsbToByte(alloc.fsb),
                                std::move(image), lf::kBlftSymlink, sym::kLsn,
                                sym::kCrc, "symlink");
      if (!s.ok()) return s;
    }

    // Parent: dirent + timestamps (no link count change for a symlink).
    DataForkEditor fork(&txn, &blocks, device_.get(), &geometry_);
    s = fork.Load(parent.value());
    if (!s.ok()) return s;
    DirectoryEditor dir(&txn, &blocks, device_.get(), &geometry_, &fork);
    s = dir.Load(parent.value());
    if (!s.ok()) return s;
    s = dir.Insert(DirEntryWrite{std::string(name), child_ino, d::kFtSymlink});
    if (!s.ok()) return s;
    s = dir.LogBlocks();
    if (!s.ok()) return s;
    auto raw = ReadRawInode(parent_ino);
    if (!raw.ok()) return raw.error();
    std::vector<uint8_t> image = std::move(raw.value());
    uint32_t ilog_fields = 0;
    std::vector<uint8_t> fork_region;
    s = dir.FinalizeInode(&image, &ilog_fields, &fork_region);
    if (!s.ok()) return s;
    s = StoreModifiedTimes(image.data(), now);
    if (!s.ok()) return s;
    s = LogPreparedInode(txn, parent_ino, std::move(image), ilog_fields,
                         fork_region);
    if (!s.ok()) return s;

    s = blocks.Commit();
    if (!s.ok()) return s;
    return inodes.Commit();
  });
  if (!status.ok()) return status.error();

  CacheDirectoryAdd(parent_ino, name, child_ino, d::kFtSymlink);
  InvalidateCaches(CacheInvalidation::Inode(parent_ino));
  InvalidateCaches(CacheInvalidation::Inode(child_ino));
  XFS_DEBUG("symlink '" << name << "' in " << parent_ino << " -> " << child_ino
                        << " target=" << target.size() << " bytes");
  return child_ino;
}


}  // namespace xfs
