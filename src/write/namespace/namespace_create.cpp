// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Namespace mutation: create and mkdir.
//
// Owner: inode allocation and the create/mkdir namespace transaction.
// The child inode, forward dirent, reverse parent pointer, parent timestamps
// and (for mkdir) the parent link count commit in one record.

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
namespace lf = logfmt;
using namespace_detail::ParentChange;
using namespace_detail::ApplyParentChange;
using namespace_detail::BuildNewDirShortform;
using namespace_detail::BuildNewInode;
using namespace_detail::ChooseForkRegions;
using namespace_detail::NextGeneration;
using namespace_detail::ParseChildImage;
using namespace_detail::StoreModifiedTimes;

// ---------------------------------------------------------------------------
// Inode capacity for create/mkdir (two-phase chunk allocation)
// ---------------------------------------------------------------------------

// Allocating a chunk inside the create transaction (what xfs_create does
// through xfs_dialloc) carries the block allocator's bnobt/cntbt/AGF/AGFL
// buffers, the inobt insert and the icreate item; added to the directory work
// that can exceed one log record. When no AG has a free inode, allocate the
// chunk in its own transaction first, so the create transaction only takes an
// inode out of the new chunk. A crash between the two transactions leaves a
// legal filesystem with one extra all-free chunk - exactly the state
// xfs_ialloc_ag_alloc leaves before any inode is handed out - never an
// allocated inode that is unreachable.
Status XfsFilesystem::EnsureInodeCapacity(uint32_t preferred_agno) {
  for (uint32_t i = 0; i < geometry_.agcount; ++i) {
    auto agi = ReadAgi(*device_, geometry_, i);
    if (!agi.ok()) return agi.error();
    if (agi.value().freecount > 0) return OkStatus();
  }
  if (preferred_agno >= geometry_.agcount) preferred_agno = 0;
  Status last = XFS_ERR(XfsError::kNoSpace,
                        "no free inode and no space for a new inode chunk")
                   .Metadata("allocator");
  for (uint32_t i = 0; i < geometry_.agcount; ++i) {
    const uint32_t agno = (preferred_agno + i) % geometry_.agcount;
    Status status = RunTransaction([&](XfsTransaction& txn) -> Status {
      BlockAllocator blocks(&txn, device_.get(), &geometry_);
      InodeAllocator inodes(&txn, &blocks, device_.get(), &superblock_,
                            &geometry_);
      Status s = inodes.PreallocateChunk(agno);
      if (!s.ok()) return s;
      s = blocks.Commit();
      if (!s.ok()) return s;
      return inodes.Commit();
    });
    if (status.ok()) return OkStatus();
    if (status.error().code != XfsError::kNoSpace) return status;
    last = status;
  }
  return last;
}

// ---------------------------------------------------------------------------
// Create a regular file
// ---------------------------------------------------------------------------

Result<uint64_t> XfsFilesystem::CreateRegularFile(uint64_t parent_ino,
                                           std::string_view name,
                                           uint16_t mode, uint32_t uid,
                                           uint32_t gid) {
  std::unique_lock<std::shared_mutex> namespace_barrier(write_barrier_);
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume, "filesystem is mounted read-only")
        .Metadata("create");
  }
  if ((mode & kSIfmt) == 0) mode = static_cast<uint16_t>(mode | kSIfreg);
  if ((mode & kSIfmt) != kSIfreg) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "CreateFile only creates regular files")
        .Actual(mode)
        .Metadata("create");
  }
  // Make sure the create transaction does not have to carry a chunk
  // allocation (see EnsureInodeCapacity).
  Status capacity =
      EnsureInodeCapacity(geometry_.InoAgno(parent_ino));
  if (!capacity.ok()) return capacity.error();
  uint64_t child_ino = 0;
  Status status = RunTransaction([&](XfsTransaction& txn) -> Status {
    auto parent = OpenInode(parent_ino);
    if (!parent.ok()) return parent.error();
    if (!parent.value().IsDir()) {
      return XFS_ERR(XfsError::kNotDirectory, "parent is not a directory")
          .Ino(parent_ino)
          .Metadata("create");
    }
    if ((parent.value().flags & di::kDiFlagImmutable) != 0) {
      return XFS_ERR(XfsError::kPermissionDenied, "parent is immutable")
          .Ino(parent_ino)
          .Metadata("create");
    }
    auto existing = LookupName(parent_ino, name);
    if (existing.ok()) {
      return XFS_ERR(XfsError::kAlreadyExists, "name already exists")
          .Ino(parent_ino)
          .Metadata("create");
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
    std::vector<uint8_t> child = BuildNewInode(
        geometry_, child_ino, mode, uid, gid, gen, now, nullptr);
    if (child.empty()) {
      return XFS_ERR(XfsError::kInternal, "cannot build the new inode")
          .Ino(child_ino);
    }
    uint32_t child_data_fields = 0;
    std::vector<uint8_t> child_data_region;
    uint32_t child_attr_fields = 0;
    std::vector<uint8_t> child_attr_region;
    if (geometry_.features.parent) {
      XfsInode parsed;
      s = ParseChildImage(geometry_, child_ino, child, &parsed);
      if (!s.ok()) return s;
      XattrFork empty_fork;
      empty_fork.format = XattrForkFormat::kNone;
      ParentChange change;
      change.add = ParentPointer{parent_ino, parent.value().gen,
                                 std::string(name)};
      auto prepared = ApplyParentChange(txn, blocks, *device_, geometry_,
                                        parsed, empty_fork, std::move(child),
                                        change);
      if (!prepared.ok()) return prepared.error();
      child = std::move(prepared.value().image);
      ChooseForkRegions(prepared.value(), &child_data_fields,
                        &child_data_region, &child_attr_fields,
                        &child_attr_region);
    }
    s = LogPreparedInode(txn, child_ino, std::move(child), child_data_fields,
                         child_data_region, child_attr_fields,
                         child_attr_region);
    if (!s.ok()) return s;

    DataForkEditor fork(&txn, &blocks, device_.get(), &geometry_);
    s = fork.Load(parent.value());
    if (!s.ok()) return s;
    DirectoryEditor dir(&txn, &blocks, device_.get(), &geometry_, &fork);
    s = dir.Load(parent.value());
    if (!s.ok()) return s;
    s = dir.Insert(
        DirEntryWrite{std::string(name), child_ino, d::kFtRegFile});
    if (!s.ok()) return s;
    s = dir.LogBlocks();
    if (!s.ok()) return s;

    auto raw = ReadRawInode(parent_ino);
    if (!raw.ok()) return raw.error();
    std::vector<uint8_t> raw_inode = std::move(raw.value());
    uint32_t ilog_fields = 0;
    std::vector<uint8_t> fork_region;
    s = dir.FinalizeInode(&raw_inode, &ilog_fields, &fork_region);
    if (!s.ok()) return s;
    s = StoreModifiedTimes(raw_inode.data(), now);
    if (!s.ok()) return s;
    s = LogPreparedInode(txn, parent_ino, std::move(raw_inode), ilog_fields,
                         fork_region);
    if (!s.ok()) return s;
    s = blocks.Commit();
    if (!s.ok()) return s;
    return inodes.Commit();
  });
  if (!status.ok()) return status.error();
  CacheDirectoryAdd(parent_ino, name, child_ino, d::kFtRegFile);
  InvalidateCaches(CacheInvalidation::Inode(parent_ino));
  InvalidateCaches(CacheInvalidation::Inode(child_ino));
  XFS_DEBUG("created '" << name << "' in " << parent_ino << " -> "
                        << child_ino);
  return child_ino;
}

// ---------------------------------------------------------------------------
// mkdir
// ---------------------------------------------------------------------------

Status XfsFilesystem::Mkdir(uint64_t parent_ino, std::string_view name,
                            uint16_t mode, uint32_t uid, uint32_t gid) {
  std::unique_lock<std::shared_mutex> namespace_barrier(write_barrier_);
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume, "filesystem is mounted read-only")
        .Metadata("mkdir");
  }
  mode = static_cast<uint16_t>((mode & 07777) | kSIfdir);
  // Same two-phase choice as CreateFile: outside a transaction the chunk
  // allocation is invisible to a crash except as an all-free chunk.
  Status capacity = EnsureInodeCapacity(geometry_.InoAgno(parent_ino));
  if (!capacity.ok()) return capacity.error();
  uint64_t child_ino = 0;
  Status status = RunTransaction([&](XfsTransaction& txn) -> Status {
    auto parent = OpenInode(parent_ino);
    if (!parent.ok()) return parent.error();
    if (!parent.value().IsDir()) {
      return XFS_ERR(XfsError::kNotDirectory, "parent is not a directory")
          .Ino(parent_ino)
          .Metadata("mkdir");
    }
    if ((parent.value().flags & di::kDiFlagImmutable) != 0) {
      return XFS_ERR(XfsError::kPermissionDenied, "parent is immutable")
          .Ino(parent_ino)
          .Metadata("mkdir");
    }
    auto existing = LookupName(parent_ino, name);
    if (existing.ok()) {
      return XFS_ERR(XfsError::kAlreadyExists, "name already exists")
          .Ino(parent_ino)
          .Metadata("mkdir");
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
    std::vector<uint8_t> sf = BuildNewDirShortform(parent_ino);
    std::vector<uint8_t> child =
        BuildNewInode(geometry_, child_ino, mode, uid, gid, gen, now, &sf);
    if (child.empty()) {
      return XFS_ERR(XfsError::kInternal, "cannot build the new directory")
          .Ino(child_ino);
    }
    uint32_t child_data_fields = lf::kILogDData;
    std::vector<uint8_t> child_data_region = sf;
    uint32_t child_attr_fields = 0;
    std::vector<uint8_t> child_attr_region;
    if (geometry_.features.parent) {
      XfsInode parsed;
      s = ParseChildImage(geometry_, child_ino, child, &parsed);
      if (!s.ok()) return s;
      XattrFork empty_fork;
      empty_fork.format = XattrForkFormat::kNone;
      ParentChange change;
      change.add = ParentPointer{parent_ino, parent.value().gen,
                                 std::string(name)};
      auto prepared = ApplyParentChange(txn, blocks, *device_, geometry_,
                                        parsed, empty_fork, std::move(child),
                                        change);
      if (!prepared.ok()) return prepared.error();
      child = std::move(prepared.value().image);
      ChooseForkRegions(prepared.value(), &child_data_fields,
                        &child_data_region, &child_attr_fields,
                        &child_attr_region);
    }
    s = LogPreparedInode(txn, child_ino, std::move(child), child_data_fields,
                         child_data_region, child_attr_fields,
                         child_attr_region);
    if (!s.ok()) return s;

    DataForkEditor fork(&txn, &blocks, device_.get(), &geometry_);
    s = fork.Load(parent.value());
    if (!s.ok()) return s;
    DirectoryEditor dir(&txn, &blocks, device_.get(), &geometry_, &fork);
    s = dir.Load(parent.value());
    if (!s.ok()) return s;
    s = dir.Insert(DirEntryWrite{std::string(name), child_ino, d::kFtDir});
    if (!s.ok()) return s;
    s = dir.LogBlocks();
    if (!s.ok()) return s;

    auto raw = ReadRawInode(parent_ino);
    if (!raw.ok()) return raw.error();
    std::vector<uint8_t> raw_inode = std::move(raw.value());
    uint32_t ilog_fields = 0;
    std::vector<uint8_t> fork_region;
    s = dir.FinalizeInode(&raw_inode, &ilog_fields, &fork_region);
    if (!s.ok()) return s;
    // A subdirectory adds one link to the parent (".." in the child).
    PutBE32(raw_inode.data(), di::kNlink,
            GetBE32(raw_inode.data(), di::kNlink) + 1);
    s = StoreModifiedTimes(raw_inode.data(), now);
    if (!s.ok()) return s;
    s = LogPreparedInode(txn, parent_ino, std::move(raw_inode), ilog_fields,
                         fork_region);
    if (!s.ok()) return s;
    s = blocks.Commit();
    if (!s.ok()) return s;
    return inodes.Commit();
  });
  if (!status.ok()) return status;
  CacheDirectoryAdd(parent_ino, name, child_ino, d::kFtDir);
  InvalidateCaches(CacheInvalidation::Inode(parent_ino));
  InvalidateCaches(CacheInvalidation::Inode(child_ino));
  XFS_DEBUG("mkdir '" << name << "' in " << parent_ino << " -> " << child_ino);
  return OkStatus();
}

}  // namespace xfs
