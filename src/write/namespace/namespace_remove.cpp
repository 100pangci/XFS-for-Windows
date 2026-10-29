// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Namespace mutation: unlink, rmdir and the unlinked-inode lifecycle.
//
// Owner: the remove-side namespace transaction (dirent, nlink, AGI
// unlinked list, parent pointer, child "..") and the later rolling
// truncate/ifree. It never frees an inode inside the namespace record.

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
using namespace_detail::ParseChildImage;
using namespace_detail::RawNextUnlinked;
using namespace_detail::ReadChildAttrFork;
using namespace_detail::StoreModifiedTimes;
using namespace_detail::StoreTimestamp;
using namespace_detail::kMaxUnlinkedWalk;
using namespace_detail::kNullAgino;

// ---------------------------------------------------------------------------
// Unlink / rmdir
// ---------------------------------------------------------------------------

Status XfsFilesystem::RemoveNamespaceEntry(uint64_t parent_ino,
                                           std::string_view name,
                                           bool directory_expected,
                                           bool defer_inode_free,
                                           uint64_t* removed_ino) {
  std::unique_lock<std::shared_mutex> namespace_barrier(write_barrier_);
  if (name.empty() || name == "." || name == "..") {
    return XFS_ERR(XfsError::kInvalidArgument, "invalid unlink name")
        .Ino(parent_ino);
  }
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
    s = StoreModifiedTimes(parent_image.data(), now);
    if (!s.ok()) return s;
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

    // Remove the reverse record in the same transaction.  This happens before
    // the directory ".." rewrite because that rewrite may change the child's
    // literal-area layout; the directory editor is then loaded from the
    // already-updated image so both edits share one forkoff.
    uint32_t child_attr_ilog = 0;
    std::vector<uint8_t> child_attr_region;
    if (geometry_.features.parent) {
      auto child_fork = ReadChildAttrFork(*device_, geometry_, child.value());
      if (!child_fork.ok()) return child_fork.error();
      ParentChange change;
      change.remove = ParentPointer{parent_ino, parent.value().gen,
                                    std::string(name)};
      auto prepared = ApplyParentChange(
          txn, blocks, *device_, geometry_, child.value(), child_fork.value(),
          std::move(child_image), change);
      if (!prepared.ok()) return prepared.error();
      child_image = std::move(prepared.value().image);
      child_attr_ilog = prepared.value().attr_fields;
      child_attr_region = prepared.value().attr_region;
    }

    // xfs_dir_remove_child: point an unlinked child directory's ".." entry at
    // the root so it never references an inode that may be freed before the
    // child is closed. That edit lives in the *child's* data fork, so it needs
    // the child's own fork/editor - never the parent's.
    uint32_t child_ilog = 0;
    std::vector<uint8_t> child_fork_region;
    if (is_dir && parent_ino != geometry_.rootino) {
      XfsInode updated;
      s = ParseChildImage(geometry_, entry.ino, child_image, &updated);
      if (!s.ok()) return s;
      DataForkEditor child_fork(&txn, &blocks, device_.get(), &geometry_);
      s = child_fork.Load(updated);
      if (!s.ok()) return s;
      DirectoryEditor child_dir(&txn, &blocks, device_.get(), &geometry_,
                                &child_fork);
      s = child_dir.Load(updated);
      if (!s.ok()) return s;
      s = child_dir.RepointDotDot(geometry_.rootino);
      if (!s.ok()) return s;
      s = child_dir.LogBlocks();
      if (!s.ok()) return s;
      s = child_dir.FinalizeInode(&child_image, &child_ilog, &child_fork_region);
      if (!s.ok()) return s;
    }

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
    } else {
      PutBE32(child_image.data(), di::kNextUnlinked,
              RawNextUnlinked(child_image));
    }
    s = StoreTimestamp(child_image.data(), di::kCtime, now);
    if (!s.ok()) return s;
    s = LogPreparedInode(txn, entry.ino, std::move(child_image), child_ilog,
                         child_fork_region, child_attr_ilog,
                         child_attr_region);
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
  CacheDirectoryRemove(parent_ino, name);
  InvalidateCaches(CacheInvalidation::Inode(parent_ino));
  InvalidateCaches(CacheInvalidation::Extents(ino));
  // An unlinked directory's ".." was repointed; its own cached listing (read
  // during the emptiness check) is stale.
  InvalidateCaches(CacheInvalidation::Directory(ino));
  if (removed_ino != nullptr) *removed_ino = ino;
  if (needs_free && !defer_inode_free) return FreeUnlinkedInodeLocked(ino);
  return OkStatus();
}

Status XfsFilesystem::Unlink(uint64_t parent_ino, std::string_view name,
                             bool defer_inode_free) {
  return RemoveNamespaceEntry(parent_ino, name, /*directory_expected=*/false,
                             defer_inode_free, nullptr);
}

Status XfsFilesystem::RemoveEmptyDirectory(uint64_t parent_ino,
                                      std::string_view name,
                                      bool defer_inode_free) {
  return RemoveNamespaceEntry(parent_ino, name, /*directory_expected=*/true,
                              defer_inode_free, nullptr);
}

// ---------------------------------------------------------------------------
// FreeUnlinkedInode: xfs_inactive (truncate) + xfs_ifree (difree, iunlink)
// ---------------------------------------------------------------------------

Status XfsFilesystem::FreeUnlinkedInode(uint64_t ino) {
  std::unique_lock<std::shared_mutex> namespace_barrier(write_barrier_);
  return FreeUnlinkedInodeLocked(ino);
}

Status XfsFilesystem::FreeUnlinkedInodeLocked(uint64_t ino) {
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

  Status status = RunTransaction([&](XfsTransaction& txn) -> Status {
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
    if (inode.value().HasAttrFork() && inode.value().anextents != 0) {
      return XFS_ERR(XfsError::kInternal,
                     "inode attr fork was not truncated before the free")
          .Ino(ino)
          .Actual(inode.value().anextents)
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
          status = StoreTimestamp(prev_image.data(), di::kCtime, now);
          if (!status.ok()) return status;
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
    // xfs_inode_uninit resets di_flags2 to the filesystem's new_diflags2
    // rather than zero, so a freed inode keeps BIGTIME/NREXT64 on a
    // filesystem that has those features.
    uint64_t flags2 = 0;
    if (geometry_.features.bigtime) flags2 |= di::kDiFlag2Bigtime;
    if (geometry_.features.nrext64) flags2 |= di::kDiFlag2Nrext64;
    PutBE64(image.data(), di::kFlags2, flags2);
    if ((flags2 & di::kDiFlag2Nrext64) == 0) {
      // Offset 24 is the v3 pad without NREXT64 and must be zero.
      PutBE64(image.data(), di::kV3Pad, 0);
    }
    image[di::kForkoff] = 0;
    image[di::kAformat] = di::kFormatExtents;
    image[di::kFormat] = di::kFormatExtents;
    PutBE64(image.data(), di::kSize, 0);
    PutBE64(image.data(), di::kNblocks, 0);
    SetInodeDataExtentCount(image.data(), 0);
    SetInodeAttrExtentCount(image.data(), 0);
    PutBE32(image.data(), di::kDmevmask, 0);
    PutBE16(image.data(), di::kDmstate, 0);
    const size_t free_time_fields[] = {di::kAtime, di::kMtime, di::kCtime,
                                       di::kCrtime};
    for (size_t field : free_time_fields) {
      status = StoreTimestamp(image.data(), field, now);
      if (!status.ok()) return status;
    }
    status = LogPreparedInode(txn, ino, std::move(image), 0, {});
    if (!status.ok()) return status;
    status = blocks.Commit();
    if (!status.ok()) return status;
    return inodes.Commit();
  });
  if (status.ok()) {
    // A freed inode number can be reused with a new generation; do not carry
    // the old deferred timestamp into the next incarnation.
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_times_.erase(ino);
  }
  return status;
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
    // xfs_attr_inactive: release every attribute-fork block before the inode
    // is freed.  The DA tree itself does not need to be re-read here: the
    // extent map is the ownership authority, and removing whole extents keeps
    // every intermediate committed image consistent.
    if (inode.value().HasAttrFork() && inode.value().anextents != 0) {
      constexpr size_t kAttrExtentsPerTransaction = 48;
      bool attr_more = false;
      Status status = RunTransaction([&](XfsTransaction& txn) -> Status {
        auto fresh = OpenInode(ino);
        if (!fresh.ok()) return fresh.error();
        if (fresh.value().nlink != 0) {
          return XFS_ERR(XfsError::kInvalidArgument,
                         "refusing to truncate an inode that still has links")
              .Ino(ino)
              .Metadata("ifree");
        }
        if (!fresh.value().HasAttrFork() ||
            fresh.value().anextents == 0) {
          return OkStatus();
        }
        BlockAllocator blocks(&txn, device_.get(), &geometry_);
        XfsInode synth;
        synth.ino = ino;
        synth.format = fresh.value().aformat;
        synth.nextents = fresh.value().anextents;
        synth.data_fork = fresh.value().attr_fork;
        DataForkEditor editor(&txn, &blocks, device_.get(), &geometry_,
                              /*attr_fork=*/true);
        Status s = editor.Load(synth);
        if (!s.ok()) return s;
        editor.SetFreeRemovedBlocks(true);
        std::vector<XfsExtent> extents = editor.extents();
        size_t removed = 0;
        for (const XfsExtent& e : extents) {
          if (removed >= kAttrExtentsPerTransaction) {
            attr_more = true;
            break;
          }
          s = editor.RemoveRange(e.startoff, e.EndBlock());
          if (!s.ok()) return s;
          ++removed;
        }
        auto raw = ReadRawInode(ino);
        if (!raw.ok()) return raw.error();
        std::vector<uint8_t> image = std::move(raw.value());
        std::vector<uint8_t> temp = image;
        uint32_t data_fields = 0;
        std::vector<uint8_t> data_region;
        s = editor.Finalize(&temp, &data_fields, &data_region);
        if (!s.ok()) return s;
        const uint8_t aformat = editor.format();
        const uint32_t anextents = editor.nextents();
        image[di::kAformat] = aformat;
        SetInodeAttrExtentCount(image.data(), anextents);
        PutBE64(image.data(), di::kNblocks,
                GetBE64(image.data(), di::kNblocks) +
                    static_cast<uint64_t>(editor.nblocks_delta()));
        const size_t attr_off = di::kCoreSizeV3 +
                                static_cast<size_t>(fresh.value().forkoff) * 8;
        const size_t attr_size = static_cast<size_t>(
            geometry_.inode_size - attr_off);
        std::memcpy(image.data() + attr_off, temp.data() + di::kCoreSizeV3,
                    std::min(attr_size, temp.size() - di::kCoreSizeV3));
        uint32_t attr_fields = 0;
        std::vector<uint8_t> attr_region;
        if (aformat == di::kFormatBtree) {
          attr_fields = lf::kILogABRoot;
          attr_region = std::move(data_region);
        } else if (!editor.extents().empty()) {
          attr_fields = lf::kILogAExt;
          attr_region.assign(editor.extents().size() * 16, 0);
          std::memcpy(attr_region.data(), temp.data() + di::kCoreSizeV3,
                      attr_region.size());
        }
        s = LogPreparedInode(txn, ino, std::move(image), 0, {}, attr_fields,
                             attr_region);
        if (!s.ok()) return s;
        s = blocks.Commit();
        if (!s.ok()) return s;
        return OkStatus();
      });
      if (!status.ok()) return status;
      if (attr_more) continue;  // next rolling transaction
    }
    // A symlink whose target lives in remote blocks is freed like Linux
    // xfs_inactive_symlink_rmt: the inode is turned into a regular file (a
    // zero-length symlink must never be visible on disk) in the same
    // transaction that releases the blocks, so it needs at least one pass even
    // when the extent list is already empty.
    const bool remote_symlink = inode.value().IsSymlink() &&
                                inode.value().format != di::kFormatLocal;
    if (!remote_symlink && (inode.value().format == di::kFormatLocal ||
                            inode.value().nextents == 0)) {
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
      if (fresh.value().IsSymlink()) {
        const uint16_t mode = GetBE16(image.data(), di::kMode);
        PutBE16(image.data(), di::kMode,
                static_cast<uint16_t>((mode & ~kSIfmt) | kSIfreg));
      }
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
