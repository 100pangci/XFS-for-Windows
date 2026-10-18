// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - namespace mutation (RW-3): create, unlink, mkdir, rmdir.
//
// The transaction shapes follow Linux:
//
//   xfs_create   one transaction: xfs_dialloc + xfs_icreate (inode core) +
//                xfs_dir_createname + parent timestamps (+ parent nlink for a
//                directory). The unlinked-list and journal machinery make
//                every crash window either "no file" or "complete file".
//   xfs_remove   one transaction for the namespace update, the link count
//                transition and the AGI unlinked-list insertion; the data
//                blocks and the inode itself are released afterwards by the
//                equivalent of xfs_inactive/xfs_ifree (xfs_itruncate_extents
//                + xfs_difree + xfs_iunlink_remove), in rolling transactions.
//
// An inode with nlink == 0 is never reachable from the namespace but stays on
// the per-AG unlinked list until it is freed. That is exactly the state Linux
// leaves after a crash and `xlog_recover_process_iunlinks` cleans up, so the
// crash contract holds without inventing a Windows-only orphan mechanism.
//
// "Open but unlinked" (handles still open) is expressed by deferring the
// second phase: the caller keeps the zero-link inode and calls
// FreeUnlinkedInode when the last handle closes.
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "xfs/ag.h"
#include "xfs/alloc.h"
#include "xfs/bmap.h"
#include "xfs/crash.h"
#include "xfs/dir_write.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/ialloc.h"
#include "xfs/log.h"
#include "xfs/log_format.h"
#include "xfs/ondisk.h"
#include "xfs/write.h"

namespace xfs {
namespace {

namespace di = ondisk::inode;
namespace d = ondisk::dir;
namespace lf = logfmt;

constexpr uint32_t kNullAgino = 0xffffffffu;
constexpr size_t kMaxUnlinkedWalk = 1u << 20;

uint32_t NextGeneration() {
  static std::mt19937 rng(std::random_device{}());
  return rng();
}

void StoreTimestamp(uint8_t* field, const XfsTimestamp& ts) {
  PutBE32(field, 0, static_cast<uint32_t>(ts.sec));
  PutBE32(field, 4, ts.nsec);
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
  b[di::kAformat] = 0;
  StoreTimestamp(b + di::kAtime, now);
  StoreTimestamp(b + di::kMtime, now);
  StoreTimestamp(b + di::kCtime, now);
  StoreTimestamp(b + di::kCrtime, now);
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
  PutBE32(b, di::kNextents, 0);
  PutBE16(b, di::kAnextents, 0);
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

}  // namespace

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

Result<uint64_t> XfsFilesystem::CreateFile(uint64_t parent_ino,
                                           std::string_view name,
                                           uint16_t mode, uint32_t uid,
                                           uint32_t gid) {
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
    s = LogPreparedInode(txn, child_ino, std::move(child), 0, {});
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
    StoreTimestamp(raw_inode.data() + di::kMtime, now);
    StoreTimestamp(raw_inode.data() + di::kCtime, now);
    s = LogPreparedInode(txn, parent_ino, std::move(raw_inode), ilog_fields,
                         fork_region);
    if (!s.ok()) return s;
    s = blocks.Commit();
    if (!s.ok()) return s;
    return inodes.Commit();
  });
  if (!status.ok()) return status.error();
  InvalidateCaches(CacheInvalidation::Directory(parent_ino));
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
    s = LogPreparedInode(txn, child_ino, std::move(child), lf::kILogDData, sf);
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
    StoreTimestamp(raw_inode.data() + di::kMtime, now);
    StoreTimestamp(raw_inode.data() + di::kCtime, now);
    s = LogPreparedInode(txn, parent_ino, std::move(raw_inode), ilog_fields,
                         fork_region);
    if (!s.ok()) return s;
    s = blocks.Commit();
    if (!s.ok()) return s;
    return inodes.Commit();
  });
  if (!status.ok()) return status;
  InvalidateCaches(CacheInvalidation::Directory(parent_ino));
  InvalidateCaches(CacheInvalidation::Inode(parent_ino));
  InvalidateCaches(CacheInvalidation::Inode(child_ino));
  XFS_DEBUG("mkdir '" << name << "' in " << parent_ino << " -> " << child_ino);
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Unlink / rmdir
// ---------------------------------------------------------------------------

Status XfsFilesystem::RemoveNamespaceEntry(uint64_t parent_ino,
                                           std::string_view name,
                                           bool directory_expected,
                                           bool defer_inode_free,
                                           uint64_t* removed_ino) {
  uint64_t ino = 0;
  bool needs_free = false;
  Status status = RunTransaction([&](XfsTransaction& txn) -> Status {
    auto parent = OpenInode(parent_ino);
    if (!parent.ok()) return parent.error();
    if (!parent.value().IsDir()) {
      return XFS_ERR(XfsError::kNotDirectory, "parent is not a directory")
          .Ino(parent_ino);
    }
    if ((parent.value().flags & di::kDiFlagImmutable) != 0) {
      return XFS_ERR(XfsError::kPermissionDenied, "parent is immutable")
          .Ino(parent_ino);
    }
    auto found = LookupName(parent_ino, name);
    if (!found.ok()) return found.error();
    const XfsDirEntry entry = found.value();
    auto child = OpenInode(entry.ino);
    if (!child.ok()) return child.error();
    const bool is_dir = child.value().IsDir();
    if (directory_expected != is_dir) {
      return XFS_ERR(directory_expected ? XfsError::kNotDirectory
                                        : XfsError::kNotRegularFile,
                     directory_expected
                         ? "entry is not a directory"
                         : "cannot unlink a directory (use RemoveDirectory)")
          .Ino(entry.ino);
    }
    if ((child.value().flags & di::kDiFlagImmutable) != 0) {
      return XFS_ERR(XfsError::kPermissionDenied, "entry is immutable")
          .Ino(entry.ino);
    }
    if (is_dir) {
      // xfs_dir_remove_child: nlink must be 2 and the directory must contain
      // nothing but "." and "..".
      if (child.value().nlink != 2) {
        return XFS_ERR(XfsError::kDirectoryNotEmpty, "directory is not empty")
            .Ino(entry.ino)
            .Actual(child.value().nlink);
      }
      auto listing = ListDirectory(entry.ino);
      if (!listing.ok()) return listing.error();
      for (const XfsDirEntry& e : listing.value()) {
        if (e.name != "." && e.name != "..") {
          return XFS_ERR(XfsError::kDirectoryNotEmpty,
                         "directory is not empty")
              .Ino(entry.ino);
        }
      }
    }

    BlockAllocator blocks(&txn, device_.get(), &geometry_);
    InodeAllocator inodes(&txn, &blocks, device_.get(), &superblock_,
                          &geometry_);
    const XfsTimestamp now = CurrentTimestamp();
    const uint32_t agno = geometry_.InoAgno(entry.ino);
    const uint32_t agino = geometry_.InoAgino(entry.ino);
    const uint32_t bucket = agino % ondisk::kAgiUnlinkedBuckets;

    // 1. Remove the directory entry and update the parent.
    DataForkEditor fork(&txn, &blocks, device_.get(), &geometry_);
    Status s = fork.Load(parent.value());
    if (!s.ok()) return s;
    DirectoryEditor dir(&txn, &blocks, device_.get(), &geometry_, &fork);
    s = dir.Load(parent.value());
    if (!s.ok()) return s;
    auto removed = dir.Remove(name);
    if (!removed.ok()) return removed.error();
    s = dir.LogBlocks();
    if (!s.ok()) return s;

    auto raw_parent = ReadRawInode(parent_ino);
    if (!raw_parent.ok()) return raw_parent.error();
    std::vector<uint8_t> parent_image = std::move(raw_parent.value());
    uint32_t ilog_fields = 0;
    std::vector<uint8_t> fork_region;
    s = dir.FinalizeInode(&parent_image, &ilog_fields, &fork_region);
    if (!s.ok()) return s;
    if (is_dir) {
      // Drop the link from the child's "..".
      const uint32_t nlink = GetBE32(parent_image.data(), di::kNlink);
      if (nlink == 0) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "parent link count is already zero")
            .Ino(parent_ino);
      }
      PutBE32(parent_image.data(), di::kNlink, nlink - 1);
    }
    StoreTimestamp(parent_image.data() + di::kMtime, now);
    StoreTimestamp(parent_image.data() + di::kCtime, now);
    s = LogPreparedInode(txn, parent_ino, std::move(parent_image),
                         ilog_fields, fork_region);
    if (!s.ok()) return s;

    // 2. Link count and, when it reaches zero, the AGI unlinked list.
    auto raw_child = ReadRawInode(entry.ino);
    if (!raw_child.ok()) return raw_child.error();
    std::vector<uint8_t> child_image = std::move(raw_child.value());
    const uint32_t nlink = GetBE32(child_image.data(), di::kNlink);
    if (nlink == 0) {
      return XFS_ERR(XfsError::kCorruptInode, "inode already has no links")
          .Ino(entry.ino);
    }
    const uint32_t new_nlink = is_dir ? 0 : nlink - 1;
    PutBE32(child_image.data(), di::kNlink, new_nlink);
    if (new_nlink == 0) {
      auto head = inodes.UnlinkedBucketHead(agno, bucket);
      if (!head.ok()) return head.error();
      if (head.value() == agino) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "inode is already the head of its unlinked bucket")
            .Ino(entry.ino);
      }
      PutBE32(child_image.data(), di::kNextUnlinked, head.value());
      s = inodes.SetUnlinkedBucketHead(agno, bucket, agino);
      if (!s.ok()) return s;
      // xfs_dir_remove_child points an unlinked child directory's ".." at the
      // root so it never references an inode that may be freed first.
      if (is_dir && parent_ino != geometry_.rootino) {
        s = dir.RepointDotDot(geometry_.rootino);
        if (!s.ok()) return s;
      }
    } else {
      PutBE32(child_image.data(), di::kNextUnlinked,
              RawNextUnlinked(child_image));
    }
    StoreTimestamp(child_image.data() + di::kCtime, now);
    s = LogPreparedInode(txn, entry.ino, std::move(child_image), 0, {});
    if (!s.ok()) return s;

    s = blocks.Commit();
    if (!s.ok()) return s;
    s = inodes.Commit();
    if (!s.ok()) return s;
    ino = entry.ino;
    needs_free = new_nlink == 0;
    return OkStatus();
  });
  if (!status.ok()) return status;
  InvalidateCaches(CacheInvalidation::Directory(parent_ino));
  InvalidateCaches(CacheInvalidation::Inode(parent_ino));
  InvalidateCaches(CacheInvalidation::Extents(ino));
  if (removed_ino != nullptr) *removed_ino = ino;
  if (needs_free && !defer_inode_free) return FreeUnlinkedInode(ino);
  return OkStatus();
}

Status XfsFilesystem::Unlink(uint64_t parent_ino, std::string_view name,
                             bool defer_inode_free) {
  return RemoveNamespaceEntry(parent_ino, name, /*directory_expected=*/false,
                             defer_inode_free, nullptr);
}

Status XfsFilesystem::RemoveDirectory(uint64_t parent_ino,
                                      std::string_view name,
                                      bool defer_inode_free) {
  return RemoveNamespaceEntry(parent_ino, name, /*directory_expected=*/true,
                              defer_inode_free, nullptr);
}

// ---------------------------------------------------------------------------
// FreeUnlinkedInode: xfs_inactive (truncate) + xfs_ifree (difree, iunlink)
// ---------------------------------------------------------------------------

Status XfsFilesystem::FreeUnlinkedInode(uint64_t ino) {
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume, "filesystem is mounted read-only")
        .Ino(ino)
        .Metadata("ifree");
  }
  // xfs_itruncate_extents: release the data blocks first, in rolling
  // transactions. The inode is unreachable and on the unlinked list, so this
  // cannot race with namespace operations.
  Status s = TruncateForFree(ino);
  if (!s.ok()) return s;

  const uint32_t agno = geometry_.InoAgno(ino);
  const uint32_t agino = geometry_.InoAgino(ino);
  const uint32_t bucket = agino % ondisk::kAgiUnlinkedBuckets;
  const XfsTimestamp now = CurrentTimestamp();

  return RunTransaction([&](XfsTransaction& txn) -> Status {
    auto inode = OpenInode(ino);
    if (!inode.ok()) return inode.error();
    if (inode.value().nlink != 0) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "inode still has links; refusing to free it")
          .Ino(ino)
          .Actual(inode.value().nlink)
          .Metadata("ifree");
    }
    if (inode.value().format != di::kFormatLocal && inode.value().nextents != 0) {
      return XFS_ERR(XfsError::kInternal,
                     "inode data fork was not truncated before the free")
          .Ino(ino)
          .Actual(inode.value().nextents)
          .Metadata("ifree");
    }
    BlockAllocator blocks(&txn, device_.get(), &geometry_);
    InodeAllocator inodes(&txn, &blocks, device_.get(), &superblock_,
                          &geometry_);
    // xfs_difree.
    Status status = inodes.FreeInode(ino);
    if (!status.ok()) return status;

    // xfs_iunlink_remove.
    auto head = inodes.UnlinkedBucketHead(agno, bucket);
    if (!head.ok()) return head.error();
    auto raw = ReadRawInode(ino);
    if (!raw.ok()) return raw.error();
    const uint32_t next_unlinked = RawNextUnlinked(raw.value());
    if (head.value() == kNullAgino) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "nlink 0 inode is not on the AGI unlinked list")
          .Ino(ino)
          .Metadata("ifree");
    }
    if (head.value() == agino) {
      status = inodes.SetUnlinkedBucketHead(agno, bucket, next_unlinked);
      if (!status.ok()) return status;
    } else {
      uint32_t previous = head.value();
      bool found = false;
      for (size_t step = 0; step < kMaxUnlinkedWalk; ++step) {
        if (previous == agino) break;
        auto previous_raw = ReadRawInode(geometry_.AginoToIno(agno, previous));
        if (!previous_raw.ok()) return previous_raw.error();
        const uint32_t next = RawNextUnlinked(previous_raw.value());
        if (next == agino) {
          std::vector<uint8_t> prev_image = std::move(previous_raw.value());
          PutBE32(prev_image.data(), di::kNextUnlinked, next_unlinked);
          StoreTimestamp(prev_image.data() + di::kCtime, now);
          status = LogPreparedInode(txn, geometry_.AginoToIno(agno, previous),
                                    std::move(prev_image), 0, {});
          if (!status.ok()) return status;
          found = true;
          break;
        }
        if (next == kNullAgino) break;
        previous = next;
      }
      if (!found) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "inode is missing from its AGI unlinked bucket")
            .Ino(ino)
            .Metadata("ifree");
      }
    }

    // Reset the inode core exactly like xfs_ifree.
    std::vector<uint8_t> image = std::move(raw.value());
    PutBE16(image.data(), di::kMode, 0);
    PutBE32(image.data(), di::kNlink, 0);
    PutBE32(image.data(), di::kGen, GetBE32(image.data(), di::kGen) + 1);
    PutBE32(image.data(), di::kNextUnlinked, kNullAgino);
    PutBE16(image.data(), di::kFlags, 0);
    PutBE64(image.data(), di::kFlags2, 0);
    image[di::kForkoff] = 0;
    image[di::kAformat] = 0;
    image[di::kFormat] = di::kFormatExtents;
    PutBE64(image.data(), di::kSize, 0);
    PutBE64(image.data(), di::kNblocks, 0);
    PutBE32(image.data(), di::kNextents, 0);
    PutBE16(image.data(), di::kAnextents, 0);
    PutBE32(image.data(), di::kDmevmask, 0);
    PutBE16(image.data(), di::kDmstate, 0);
    StoreTimestamp(image.data() + di::kCtime, now);
    status = LogPreparedInode(txn, ino, std::move(image), 0, {});
    if (!status.ok()) return status;
    status = blocks.Commit();
    if (!status.ok()) return status;
    return inodes.Commit();
  });
}

// ---------------------------------------------------------------------------
// Data fork truncate for an inode being freed
// ---------------------------------------------------------------------------

Status XfsFilesystem::TruncateForFree(uint64_t ino) {
  constexpr size_t kExtentsPerTransaction = 48;
  for (;;) {
    auto inode = OpenInode(ino);
    if (!inode.ok()) return inode.error();
    if (inode.value().nlink != 0) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "refusing to truncate an inode that still has links")
          .Ino(ino)
          .Metadata("ifree");
    }
    if (inode.value().format == di::kFormatLocal ||
        inode.value().nextents == 0) {
      return OkStatus();
    }
    bool more = false;
    Status status = RunTransaction([&](XfsTransaction& txn) -> Status {
      auto fresh = OpenInode(ino);
      if (!fresh.ok()) return fresh.error();
      if (fresh.value().nlink != 0) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       "refusing to truncate an inode that still has links")
            .Ino(ino)
            .Metadata("ifree");
      }
      BlockAllocator blocks(&txn, device_.get(), &geometry_);
      DataForkEditor editor(&txn, &blocks, device_.get(), &geometry_);
      Status s = editor.Load(fresh.value());
      if (!s.ok()) return s;
      editor.SetFreeRemovedBlocks(true);
      std::vector<XfsExtent> extents = editor.extents();
      size_t removed = 0;
      for (const XfsExtent& e : extents) {
        if (removed >= kExtentsPerTransaction) {
          more = true;
          break;
        }
        s = editor.RemoveRange(e.startoff, e.EndBlock());
        if (!s.ok()) return s;
        ++removed;
      }
      auto raw = ReadRawInode(ino);
      if (!raw.ok()) return raw.error();
      std::vector<uint8_t> image = std::move(raw.value());
      uint32_t ilog_fields = 0;
      std::vector<uint8_t> fork_region;
      s = editor.Finalize(&image, &ilog_fields, &fork_region);
      if (!s.ok()) return s;
      PutBE64(image.data(), di::kSize, 0);
      s = LogPreparedInode(txn, ino, std::move(image), ilog_fields,
                           fork_region);
      if (!s.ok()) return s;
      s = blocks.Commit();
      if (!s.ok()) return s;
      return OkStatus();
    });
    if (!status.ok()) return status;
    if (!more) break;
  }
  InvalidateCaches(CacheInvalidation::Extents(ino));
  return OkStatus();
}

}  // namespace xfs
