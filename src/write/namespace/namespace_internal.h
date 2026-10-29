// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - module-private namespace mutation helpers.
//
// This header is private to src/write/namespace.  It is not part of the
// public xfs/ API and must not be included by the parser, formatter, recovery
// engine or platform adapters.
//
// Invariant ownership of this module:
//
//   namespace_common.cpp owns the construction of new inode images, the
//   timestamp encoding used by namespace transactions, and the application of
//   a child's reverse parent-pointer change onto an already-built inode image.
//   It does not own transactions, directory entries, link counts or the
//   decision to mutate a particular inode.
//
//   namespace_create.cpp owns create/mkdir/symlink: inode allocation (including
//   the separate chunk-preallocation transaction), the forward dirent, the
//   child's parent pointer, the parent timestamps and (for mkdir) the parent
//   link count.  All of those commit in one transaction.
//
//   namespace_remove.cpp owns unlink/rmdir: dirent removal, nlink transition,
//   AGI unlinked-list insertion, parent-pointer removal and the ".." rewrite,
//   all in one transaction.  The later truncate/ifree lifecycle owns its own
//   rolling transactions and never becomes visible as a partial namespace.
//
//   namespace_rename.cpp owns the atomic namespace flip: target replace/insert,
//   source removal, both parents' timestamps/link counts, the moved inode's
//   ctime/parent pointer and a cross-parent directory's "..".
//
//   namespace_link.cpp owns xfs_link: dirent insert, parent timestamps, child
//   nlink increment and parent-pointer add in one transaction.
//
// No file in this module owns the journal record format, log tail, metadata
// write-back order or cache invalidation policy; those remain owned by
// src/write/transaction.cpp and src/core/invalidation.cpp.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xfs/error.h"
#include "xfs/parent.h"
#include "xfs/transaction.h"
#include "xfs/xattr.h"
#include "write/xattr_write_internal.h"

namespace xfs {

class BlockAllocator;
class BlockDevice;
struct XfsGeometry;
struct XfsInode;

namespace namespace_detail {

constexpr uint32_t kNullAgino = 0xffffffffu;
constexpr size_t kMaxUnlinkedWalk = 1u << 20;
// XFS_MAXLINK: the di_nlink ceiling the VFS enforces (EMLINK).
constexpr uint32_t kMaxLink = (1u << 31) - 1;

uint32_t NextGeneration();

// XFS_DIR3_FT_* for an inode mode (xfs_mode_to_ftype).
uint8_t FtypeForMode(uint16_t mode);

Status StoreTimestamp(uint8_t* inode, size_t field_offset,
                      const XfsTimestamp& ts);
Status StoreModifiedTimes(uint8_t* inode, const XfsTimestamp& now);

// Builds a complete new v3 inode image. For a directory `sf` is the initial
// shortform image (header with the parent, no entries); regular files start
// with an empty extent fork. CRC and di_lsn are stamped by LogPreparedInode.
std::vector<uint8_t> BuildNewInode(const XfsGeometry& geo, uint64_t ino,
                                   uint16_t mode, uint32_t uid, uint32_t gid,
                                   uint32_t gen, const XfsTimestamp& now,
                                   const std::vector<uint8_t>* sf);

std::vector<uint8_t> BuildNewDirShortform(uint64_t parent_ino);

uint32_t RawNextUnlinked(const std::vector<uint8_t>& raw);

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
    bool missing_ok = false);

// Reads the child's attr fork for an existing inode.  New inodes that are
// still only in memory pass a default-constructed XattrFork instead.
Result<XattrFork> ReadChildAttrFork(BlockDevice& device,
                                    const XfsGeometry& geo,
                                    const XfsInode& child);

Status ParseChildImage(const XfsGeometry& geo, uint64_t ino,
                       const std::vector<uint8_t>& raw, XfsInode* out);

// Combines an attr-fork preparation with the data-fork region the operation
// already prepared (a new directory/symlink), preferring the data region the
// attr writer emitted when a forkoff change forced it to relayout the fork.
void ChooseForkRegions(const PreparedInode& prepared, uint32_t* data_fields,
                       std::vector<uint8_t>* data_region,
                       uint32_t* attr_fields,
                       std::vector<uint8_t>* attr_region);

}  // namespace namespace_detail
}  // namespace xfs
