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
#include "write/xattr_write_internal.h"

namespace xfs {
namespace {

namespace di = ondisk::inode;
namespace d = ondisk::dir;
namespace sym = ondisk::symlink;
namespace lf = logfmt;

constexpr uint32_t kNullAgino = 0xffffffffu;
constexpr size_t kMaxUnlinkedWalk = 1u << 20;

uint32_t NextGeneration() {
  static std::mt19937 rng(std::random_device{}());
  return rng();
}

// XFS_MAXLINK: the di_nlink ceiling the VFS enforces (EMLINK).
constexpr uint32_t kMaxLink = (1u << 31) - 1;

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

// One child's reverse-namespace change.  `remove` and `add` describe the
// exact records; a rename uses both, a link/create uses only `add`, and an
// unlink uses only `remove`.
struct ParentChange {
  std::optional<ParentPointer> remove;
  std::optional<ParentPointer> add;
  bool empty() const { return !remove.has_value() && !add.has_value(); }
};

Result<PreparedInode> ApplyParentChange(
    XfsTransaction& txn, BlockAllocator& blocks, BlockDevice& device,
    const XfsGeometry& geo, const XfsInode& child, const XattrFork& fork,
    std::vector<uint8_t> raw, const ParentChange& change,
    bool missing_ok = false) {
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
        auto fork = ReadChildAttrFork(*device_, geometry_, source.value());
        if (!fork.ok()) return fork.error();
        ParentChange change;
        change.add = ParentPointer{dst_parent, parent.value().gen,
                                   std::string(dst_name)};
        auto prepared = ApplyParentChange(
            txn, blocks, *device_, geometry_, source.value(), fork.value(),
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
