// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Namespace mutation: hard links.
//
// Owner: xfs_link. The destination dirent, parent timestamps, child nlink
// increment and reverse parent pointer commit together.

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
using namespace_detail::ReadChildAttrFork;
using namespace_detail::StoreModifiedTimes;
using namespace_detail::StoreTimestamp;
using namespace_detail::kMaxLink;

// ---------------------------------------------------------------------------
// Hard link (xfs_link / xfs_dir_add_child)
// ---------------------------------------------------------------------------

Status XfsFilesystem::Link(uint64_t ino, uint64_t dst_parent,
                           std::string_view dst_name) {
  std::unique_lock<std::shared_mutex> namespace_barrier(write_barrier_);
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume, "filesystem is mounted read-only")
        .Metadata("link");
  }
  if (dst_name.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument, "invalid link name")
        .Metadata("link");
  }
  Status status = RunTransaction([&](XfsTransaction& txn) -> Status {
    auto parent = OpenInode(dst_parent);
    if (!parent.ok()) return parent.error();
    if (!parent.value().IsDir()) {
      return XFS_ERR(XfsError::kNotDirectory, "link parent is not a directory")
          .Ino(dst_parent)
          .Metadata("link");
    }
    if ((parent.value().flags & di::kDiFlagImmutable) != 0) {
      return XFS_ERR(XfsError::kPermissionDenied, "parent is immutable")
          .Ino(dst_parent)
          .Metadata("link");
    }
    auto source = OpenInode(ino);
    if (!source.ok()) return source.error();
    if (source.value().IsDir()) {
      // VFS/xfs_link: hard links to directories are forbidden.
      return XFS_ERR(XfsError::kPermissionDenied,
                     "cannot hard link a directory")
          .Ino(ino)
          .Metadata("link");
    }
    if (source.value().nlink >= kMaxLink) {
      return XFS_ERR(XfsError::kTooManyLinks,
                     "link count is at the XFS maximum")
          .Ino(ino)
          .Actual(source.value().nlink)
          .Metadata("link");
    }
    auto existing = LookupName(dst_parent, dst_name);
    if (existing.ok()) {
      return XFS_ERR(XfsError::kAlreadyExists, "name already exists")
          .Ino(dst_parent)
          .Metadata("link");
    }
    if (existing.error().code != XfsError::kNotFound) {
      return existing.error();
    }

    const XfsTimestamp now = CurrentTimestamp();
    BlockAllocator blocks(&txn, device_.get(), &geometry_);
    DataForkEditor fork(&txn, &blocks, device_.get(), &geometry_);
    Status s = fork.Load(parent.value());
    if (!s.ok()) return s;
    DirectoryEditor dir(&txn, &blocks, device_.get(), &geometry_, &fork);
    s = dir.Load(parent.value());
    if (!s.ok()) return s;
    s = dir.Insert(DirEntryWrite{std::string(dst_name), ino,
                                 FtypeForMode(source.value().mode)});
    if (!s.ok()) return s;
    s = dir.LogBlocks();
    if (!s.ok()) return s;

    // Parent: mtime/ctime, and the dirent insertion (xfs_dir_add_child).
    {
      auto raw = ReadRawInode(dst_parent);
      if (!raw.ok()) return raw.error();
      std::vector<uint8_t> image = std::move(raw.value());
      uint32_t ilog_fields = 0;
      std::vector<uint8_t> fork_region;
      s = dir.FinalizeInode(&image, &ilog_fields, &fork_region);
      if (!s.ok()) return s;
      s = StoreModifiedTimes(image.data(), now);
      if (!s.ok()) return s;
      s = LogPreparedInode(txn, dst_parent, std::move(image), ilog_fields,
                           fork_region);
      if (!s.ok()) return s;
    }

    // Source inode: xfs_bumplink (nlink + ctime) and the matching parent
    // pointer, in one logged inode item.
    {
      auto raw = ReadRawInode(ino);
      if (!raw.ok()) return raw.error();
      std::vector<uint8_t> image = std::move(raw.value());
      const uint32_t nlink = GetBE32(image.data(), di::kNlink);
      if (nlink >= kMaxLink) {
        return XFS_ERR(XfsError::kTooManyLinks,
                       "link count is at the XFS maximum")
            .Ino(ino)
            .Metadata("link");
      }
      PutBE32(image.data(), di::kNlink, nlink + 1);
      uint32_t attr_ilog = 0;
      std::vector<uint8_t> attr_region;
      if (geometry_.features.parent) {
        auto attr_fork =
            ReadChildAttrFork(*device_, geometry_, source.value());
        if (!attr_fork.ok()) return attr_fork.error();
        ParentChange change;
        change.add = ParentPointer{dst_parent, parent.value().gen,
                                   std::string(dst_name)};
        auto prepared = ApplyParentChange(
            txn, blocks, *device_, geometry_, source.value(), attr_fork.value(),
            std::move(image), change);
        if (!prepared.ok()) return prepared.error();
        image = std::move(prepared.value().image);
        attr_ilog = prepared.value().attr_fields;
        attr_region = prepared.value().attr_region;
      }
      s = StoreTimestamp(image.data(), di::kCtime, now);
      if (!s.ok()) return s;
      s = LogPreparedInode(txn, ino, std::move(image), 0, {}, attr_ilog,
                           attr_region);
      if (!s.ok()) return s;
    }

    s = blocks.Commit();
    if (!s.ok()) return s;
    return OkStatus();
  });
  if (!status.ok()) return status;

  auto linked = OpenInode(ino);
  if (linked.ok()) {
    CacheDirectoryAdd(dst_parent, dst_name, ino,
                      FtypeForMode(linked.value().mode));
  } else {
    InvalidateCaches(CacheInvalidation::Directory(dst_parent));
  }
  InvalidateCaches(CacheInvalidation::Inode(dst_parent));
  InvalidateCaches(CacheInvalidation::Inode(ino));
  XFS_DEBUG("link " << ino << " -> " << dst_parent << ":'" << dst_name << "'");
  return OkStatus();
}

}  // namespace xfs
