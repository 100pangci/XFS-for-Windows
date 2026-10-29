// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Namespace mutation: atomic rename and replace.
//
// Owner: the whole namespace flip in one committed record. The target and
// source dirents, both parents' timestamps/link counts, the moved inode's
// ctime/parent pointer and a cross-parent directory's ".." are never split.

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
using namespace_detail::FtypeForMode;
using namespace_detail::ParseChildImage;
using namespace_detail::RawNextUnlinked;
using namespace_detail::ReadChildAttrFork;
using namespace_detail::StoreModifiedTimes;
using namespace_detail::StoreTimestamp;
using namespace_detail::kMaxLink;
using namespace_detail::kMaxUnlinkedWalk;

// ---------------------------------------------------------------------------
// Rename (xfs_rename / xfs_dir_rename_children)
// ---------------------------------------------------------------------------
//
// One transaction carries every change Linux makes in xfs_rename:
//
//   target side   insert (or xfs_dir_replace) the target name -> moved inode
//                 + target parent timestamps (and +1 parent link for a
//                 cross-parent directory move into an empty name)
//   target inode  link count -1 (-2 when a directory is replaced) and the AGI
//                 unlinked insert when it reaches zero (xfs_droplink)
//   moved inode   ctime; for a cross-parent directory move the ".." rewrite
//                 (xfs_dir_replace on the moved inode) and +1/-1 on the two
//                 parents
//   source side   remove the source name + source parent timestamps
//
// Because the whole namespace flip lives in one committed log record, a crash
// can only leave the old namespace or the new one - never both names, neither
// name, a dangling dirent or a wrong link count. A record that does not fit
// the transaction budget fails here, before any write.

Result<XfsFilesystem::RenameOutcome> XfsFilesystem::Rename(
    uint64_t src_parent, std::string_view src_name, uint64_t dst_parent,
    std::string_view dst_name, bool replace_existing,
    bool defer_replaced_free) {
  std::unique_lock<std::shared_mutex> namespace_barrier(write_barrier_);
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume, "filesystem is mounted read-only")
        .Metadata("rename");
  }
  if (src_name.empty() || src_name == "." || src_name == ".." ||
      dst_name.empty() || dst_name == "." || dst_name == "..") {
    return XFS_ERR(XfsError::kInvalidArgument, "invalid rename name")
        .Metadata("rename");
  }

  RenameOutcome outcome;
  bool replaced_zero_link = false;
  Status status = RunTransaction([&](XfsTransaction& txn) -> Status {
    // --- resolve and validate everything before modifying anything ---
    auto src_dp = OpenInode(src_parent);
    if (!src_dp.ok()) return src_dp.error();
    if (!src_dp.value().IsDir()) {
      return XFS_ERR(XfsError::kNotDirectory, "source parent is not a directory")
          .Ino(src_parent)
          .Metadata("rename");
    }
    if ((src_dp.value().flags & di::kDiFlagImmutable) != 0) {
      return XFS_ERR(XfsError::kPermissionDenied, "source parent is immutable")
          .Ino(src_parent)
          .Metadata("rename");
    }
    const bool same_parent = src_parent == dst_parent;
    XfsInode dst_dp;
    if (same_parent) {
      dst_dp = src_dp.value();
    } else {
      auto d = OpenInode(dst_parent);
      if (!d.ok()) return d.error();
      if (!d.value().IsDir()) {
        return XFS_ERR(XfsError::kNotDirectory,
                       "target parent is not a directory")
            .Ino(dst_parent)
            .Metadata("rename");
      }
      if ((d.value().flags & di::kDiFlagImmutable) != 0) {
        return XFS_ERR(XfsError::kPermissionDenied,
                       "target parent is immutable")
            .Ino(dst_parent)
            .Metadata("rename");
      }
      dst_dp = std::move(d.value());
    }

    auto src_entry = LookupName(src_parent, src_name);
    if (!src_entry.ok()) return src_entry.error();
    auto moved = OpenInode(src_entry.value().ino);
    if (!moved.ok()) return moved.error();
    if ((moved.value().flags & di::kDiFlagImmutable) != 0) {
      return XFS_ERR(XfsError::kPermissionDenied, "source is immutable")
          .Ino(moved.value().ino)
          .Metadata("rename");
    }
    const bool src_is_dir = moved.value().IsDir();
    const uint8_t src_ftype = FtypeForMode(moved.value().mode);

    // The VFS returns success without touching anything when both names
    // resolve to the same inode (including the same path).
    if (same_parent && src_name == dst_name) {
      outcome.moved_ino = moved.value().ino;
      return OkStatus();
    }

    bool target_exists = false;
    XfsInode target;
    auto target_entry = LookupName(dst_parent, dst_name);
    if (target_entry.ok()) {
      if (!replace_existing) {
        return XFS_ERR(XfsError::kAlreadyExists, "target name already exists")
            .Ino(dst_parent)
            .Metadata("rename");
      }
      auto t = OpenInode(target_entry.value().ino);
      if (!t.ok()) return t.error();
      target = std::move(t.value());
      if (target.ino == moved.value().ino) {
        // Renaming between two names of the same inode: POSIX no-op.
        outcome.moved_ino = moved.value().ino;
        return OkStatus();
      }
      const bool target_is_dir = target.IsDir();
      if (target_is_dir && !src_is_dir) {
        return XFS_ERR(XfsError::kNotRegularFile,
                       "cannot replace a directory with a file")
            .Ino(target.ino)
            .Metadata("rename");
      }
      if (!target_is_dir && src_is_dir) {
        return XFS_ERR(XfsError::kNotDirectory,
                       "cannot replace a file with a directory")
            .Ino(target.ino)
            .Metadata("rename");
      }
      if ((target.flags & di::kDiFlagImmutable) != 0) {
        return XFS_ERR(XfsError::kPermissionDenied, "target is immutable")
            .Ino(target.ino)
            .Metadata("rename");
      }
      if (target_is_dir) {
        // xfs_dir_rename_children: an existing target directory must be
        // empty (nlink 2 and only "." / ".." entries).
        if (target.nlink != 2) {
          return XFS_ERR(XfsError::kDirectoryNotEmpty,
                         "target directory is not empty")
              .Ino(target.ino)
              .Actual(target.nlink)
              .Metadata("rename");
        }
        auto listing = ListDirectory(target.ino);
        if (!listing.ok()) return listing.error();
        for (const XfsDirEntry& e : listing.value()) {
          if (e.name != "." && e.name != "..") {
            return XFS_ERR(XfsError::kDirectoryNotEmpty,
                           "target directory is not empty")
                .Ino(target.ino)
                .Metadata("rename");
          }
        }
      }
      target_exists = true;
    } else if (target_entry.error().code != XfsError::kNotFound) {
      return target_entry.error();
    }

    if (src_is_dir) {
      // VFS start_renaming: moving a directory into its own subtree (or onto
      // itself) is EINVAL. Walk ".." from the target parent up to the root.
      uint64_t cursor = dst_parent;
      for (size_t depth = 0; depth <= kMaxUnlinkedWalk; ++depth) {
        if (cursor == moved.value().ino) {
          return XFS_ERR(XfsError::kWouldCycle,
                         "cannot move a directory into its own subtree")
              .Ino(moved.value().ino)
              .Metadata("rename");
        }
        if (cursor == geometry_.rootino) break;
        auto up = LookupName(cursor, "..");
        if (!up.ok()) return up.error();
        if (up.value().ino == cursor) break;  // defensive: self-parent
        cursor = up.value().ino;
      }
      if (!same_parent && !target_exists && dst_dp.nlink >= kMaxLink) {
        // vfs_rename: the new parent gains a ".." link (EMLINK).
        return XFS_ERR(XfsError::kTooManyLinks,
                       "target parent link count is at the limit")
            .Ino(dst_parent)
            .Actual(dst_dp.nlink)
            .Metadata("rename");
      }
    }

    // --- target side ---
    const XfsTimestamp now = CurrentTimestamp();
    BlockAllocator blocks(&txn, device_.get(), &geometry_);
    InodeAllocator inodes(&txn, &blocks, device_.get(), &superblock_,
                          &geometry_);

    DataForkEditor src_fork(&txn, &blocks, device_.get(), &geometry_);
    Status s = src_fork.Load(src_dp.value());
    if (!s.ok()) return s;
    DirectoryEditor src_dir(&txn, &blocks, device_.get(), &geometry_, &src_fork);
    s = src_dir.Load(src_dp.value());
    if (!s.ok()) return s;

    std::unique_ptr<DataForkEditor> dst_fork;
    std::unique_ptr<DirectoryEditor> dst_dir;
    DirectoryEditor* target_dir = &src_dir;
    if (!same_parent) {
      dst_fork = std::make_unique<DataForkEditor>(&txn, &blocks, device_.get(),
                                                  &geometry_);
      s = dst_fork->Load(dst_dp);
      if (!s.ok()) return s;
      dst_dir = std::make_unique<DirectoryEditor>(
          &txn, &blocks, device_.get(), &geometry_, dst_fork.get());
      s = dst_dir->Load(dst_dp);
      if (!s.ok()) return s;
      target_dir = dst_dir.get();
    }

    if (target_exists) {
      // xfs_dir_replace: the destination name now points at the moved inode.
      s = target_dir->Replace(dst_name, moved.value().ino, src_ftype);
      if (!s.ok()) return s;
    } else {
      s = target_dir->Insert(
          DirEntryWrite{std::string(dst_name), moved.value().ino, src_ftype});
      if (!s.ok()) return s;
    }
    // Log the target directory's blocks (shortform writes ride in the inode
    // item; block/leaf/node edits are buffer items here).
    s = target_dir->LogBlocks();
    if (!s.ok()) return s;

    // --- source side (after the target, like xfs_dir_rename_children) ---
    auto removed = src_dir.Remove(src_name);
    if (!removed.ok()) return removed.error();
    s = src_dir.LogBlocks();
    if (!s.ok()) return s;

    // --- the moved inode: reverse namespace first, then ".." ---
    // xfs_parent_replacename() removes (src_dp, src_name) and inserts
    // (target_dp, target_name) as one attr-fork change.  For a cross-parent
    // directory move the ".." data-fork rewrite is applied to the image that
    // already carries the new forkoff, using a fresh directory editor.
    {
      auto raw = ReadRawInode(moved.value().ino);
      if (!raw.ok()) return raw.error();
      std::vector<uint8_t> image = std::move(raw.value());
      uint32_t ilog_fields = 0;
      std::vector<uint8_t> fork_region;
      uint32_t attr_ilog = 0;
      std::vector<uint8_t> attr_region;
      if (geometry_.features.parent) {
        auto fork = ReadChildAttrFork(*device_, geometry_, moved.value());
        if (!fork.ok()) return fork.error();
        ParentChange change;
        change.remove = ParentPointer{src_parent, src_dp.value().gen,
                                      std::string(src_name)};
        change.add = ParentPointer{dst_parent, dst_dp.gen,
                                   std::string(dst_name)};
        auto prepared = ApplyParentChange(
            txn, blocks, *device_, geometry_, moved.value(), fork.value(),
            std::move(image), change);
        if (!prepared.ok()) return prepared.error();
        image = std::move(prepared.value().image);
        attr_ilog = prepared.value().attr_fields;
        attr_region = prepared.value().attr_region;
        if (prepared.value().data_fields != 0 ||
            !prepared.value().data_region.empty()) {
          ilog_fields = prepared.value().data_fields;
          fork_region = prepared.value().data_region;
        }
      }
      if (src_is_dir && !same_parent) {
        XfsInode updated;
        s = ParseChildImage(geometry_, moved.value().ino, image, &updated);
        if (!s.ok()) return s;
        DataForkEditor moved_fork(&txn, &blocks, device_.get(), &geometry_);
        s = moved_fork.Load(updated);
        if (!s.ok()) return s;
        DirectoryEditor moved_dir(&txn, &blocks, device_.get(), &geometry_,
                                  &moved_fork);
        s = moved_dir.Load(updated);
        if (!s.ok()) return s;
        s = moved_dir.RepointDotDot(dst_parent);
        if (!s.ok()) return s;
        s = moved_dir.LogBlocks();
        if (!s.ok()) return s;
        s = moved_dir.FinalizeInode(&image, &ilog_fields, &fork_region);
        if (!s.ok()) return s;
      }
      s = StoreTimestamp(image.data(), di::kCtime, now);
      if (!s.ok()) return s;
      s = LogPreparedInode(txn, moved.value().ino, std::move(image),
                           ilog_fields, fork_region, attr_ilog, attr_region);
      if (!s.ok()) return s;
    }

    // --- the replaced inode (xfs_droplink twice for a directory) ---
    if (target_exists) {
      auto raw = ReadRawInode(target.ino);
      if (!raw.ok()) return raw.error();
      std::vector<uint8_t> image = std::move(raw.value());
      uint32_t attr_ilog = 0;
      std::vector<uint8_t> attr_region;
      if (geometry_.features.parent) {
        auto fork = ReadChildAttrFork(*device_, geometry_, target);
        if (!fork.ok()) return fork.error();
        ParentChange change;
        change.remove = ParentPointer{dst_parent, dst_dp.gen,
                                      std::string(dst_name)};
        auto prepared = ApplyParentChange(
            txn, blocks, *device_, geometry_, target, fork.value(),
            std::move(image), change);
        if (!prepared.ok()) return prepared.error();
        image = std::move(prepared.value().image);
        attr_ilog = prepared.value().attr_fields;
        attr_region = prepared.value().attr_region;
      }
      const uint32_t drops = src_is_dir ? 2u : 1u;
      const uint32_t nlink = GetBE32(image.data(), di::kNlink);
      if (nlink < drops) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "replaced inode has too few links")
            .Ino(target.ino)
            .Actual(nlink)
            .Metadata("rename");
      }
      const uint32_t new_nlink = nlink - drops;
      PutBE32(image.data(), di::kNlink, new_nlink);
      if (new_nlink == 0) {
        const uint32_t agno = geometry_.InoAgno(target.ino);
        const uint32_t agino = geometry_.InoAgino(target.ino);
        const uint32_t bucket = agino % ondisk::kAgiUnlinkedBuckets;
        auto head = inodes.UnlinkedBucketHead(agno, bucket);
        if (!head.ok()) return head.error();
        if (head.value() == agino) {
          return XFS_ERR(XfsError::kCorruptInode,
                         "inode is already the head of its unlinked bucket")
              .Ino(target.ino)
              .Metadata("rename");
        }
        PutBE32(image.data(), di::kNextUnlinked, head.value());
        s = inodes.SetUnlinkedBucketHead(agno, bucket, agino);
        if (!s.ok()) return s;
      } else {
        PutBE32(image.data(), di::kNextUnlinked,
                RawNextUnlinked(image));
      }
      s = StoreTimestamp(image.data(), di::kCtime, now);
      if (!s.ok()) return s;
      s = LogPreparedInode(txn, target.ino, std::move(image), 0, {}, attr_ilog,
                           attr_region);
      if (!s.ok()) return s;
      outcome.replaced_ino = target.ino;
      outcome.replaced_new_nlink = new_nlink;
      replaced_zero_link = new_nlink == 0;
    }

    // --- the parents: fork edits, timestamps, link counts ---
    {
      auto raw = ReadRawInode(src_parent);
      if (!raw.ok()) return raw.error();
      std::vector<uint8_t> image = std::move(raw.value());
      uint32_t ilog_fields = 0;
      std::vector<uint8_t> fork_region;
      s = src_dir.FinalizeInode(&image, &ilog_fields, &fork_region);
      if (!s.ok()) return s;
      if (src_is_dir && (!same_parent || target_exists)) {
        const uint32_t nlink = GetBE32(image.data(), di::kNlink);
        if (nlink == 0) {
          return XFS_ERR(XfsError::kCorruptInode,
                         "source parent link count is already zero")
              .Ino(src_parent)
              .Metadata("rename");
        }
        PutBE32(image.data(), di::kNlink, nlink - 1);
      }
      s = StoreModifiedTimes(image.data(), now);
      if (!s.ok()) return s;
      s = LogPreparedInode(txn, src_parent, std::move(image), ilog_fields,
                           fork_region);
      if (!s.ok()) return s;
    }
    if (!same_parent) {
      auto raw = ReadRawInode(dst_parent);
      if (!raw.ok()) return raw.error();
      std::vector<uint8_t> image = std::move(raw.value());
      uint32_t ilog_fields = 0;
      std::vector<uint8_t> fork_region;
      s = dst_dir->FinalizeInode(&image, &ilog_fields, &fork_region);
      if (!s.ok()) return s;
      if (!target_exists && src_is_dir) {
        // The moved directory's ".." adds one link to the new parent.
        const uint32_t nlink = GetBE32(image.data(), di::kNlink);
        PutBE32(image.data(), di::kNlink, nlink + 1);
      }
      s = StoreModifiedTimes(image.data(), now);
      if (!s.ok()) return s;
      s = LogPreparedInode(txn, dst_parent, std::move(image), ilog_fields,
                           fork_region);
      if (!s.ok()) return s;
    }

    s = blocks.Commit();
    if (!s.ok()) return s;
    s = inodes.Commit();
    if (!s.ok()) return s;
    outcome.moved_ino = moved.value().ino;
    outcome.replaced_zero_link = replaced_zero_link;
    return OkStatus();
  });
  if (!status.ok()) return status.error();

  InvalidateCaches(CacheInvalidation::Directory(src_parent));
  InvalidateCaches(CacheInvalidation::Inode(src_parent));
  if (dst_parent != src_parent) {
    InvalidateCaches(CacheInvalidation::Directory(dst_parent));
    InvalidateCaches(CacheInvalidation::Inode(dst_parent));
  }
  if (outcome.moved_ino != 0) {
    // A moved directory's ".." entry changed; both its parsed inode and its
    // directory listing cache are stale.  Invalidate the listing for files
    // too: it is harmless and keeps the cache policy simple.
    InvalidateCaches(CacheInvalidation::Inode(outcome.moved_ino));
    InvalidateCaches(CacheInvalidation::Directory(outcome.moved_ino));
  }
  if (outcome.replaced_ino != 0) {
    InvalidateCaches(CacheInvalidation::Inode(outcome.replaced_ino));
    InvalidateCaches(CacheInvalidation::Directory(outcome.replaced_ino));
  }
  XFS_DEBUG("rename " << src_parent << ":'" << src_name << "' -> " << dst_parent
                      << ":'" << dst_name << "' moved=" << outcome.moved_ino
                      << " replaced=" << outcome.replaced_ino);
  if (outcome.replaced_zero_link && !defer_replaced_free) {
    Status freed = FreeUnlinkedInodeLocked(outcome.replaced_ino);
    if (!freed.ok()) return freed.error();
  }
  return outcome;
}

}  // namespace xfs
