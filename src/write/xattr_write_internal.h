// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - internal shared attribute-fork mutation interface.
//
// RW-6D's writer originally lived entirely inside xattr_write.cpp.  RW-6E's
// namespace transactions need to mutate a child's attr fork (parent pointer)
// in the same transaction as the directory entry, so the core of the writer
// is exposed here.  This header is private to src/write; it is not part of
// the public xfs/ API.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/inode.h"
#include "xfs/parent.h"
#include "xfs/superblock.h"
#include "xfs/transaction.h"
#include "xfs/xattr.h"

namespace xfs {

class BlockAllocator;

// The inode image and log regions produced by one attr-fork change.  The
// caller must hand this to XfsFilesystem::LogPreparedInode exactly once.
struct PreparedInode {
  std::vector<uint8_t> image;
  uint32_t data_fields = 0;
  std::vector<uint8_t> data_region;
  uint32_t attr_fields = 0;
  std::vector<uint8_t> attr_region;
};

// Apply a complete desired attribute set to one inode's attr fork.
// `changed_index` identifies the one entry whose remote value may need a new
// allocation; `removed_entry` is the exact old entry for an incremental
// removal; `removed_remote` releases a replaced/removed remote value.
Result<PreparedInode> PrepareXattrMutation(
    XfsTransaction& txn, BlockAllocator& alloc, BlockDevice& device,
    const XfsGeometry& geo, const XfsInode& inode, const XattrFork& fork,
    std::vector<uint8_t> raw, std::vector<XattrEntry> entries,
    int changed_index,
    std::optional<std::pair<uint32_t, uint32_t>> removed_remote,
    std::optional<XattrEntry> removed_entry, bool preserve_shortform_empty);

// Parent-pointer mutations built on the same attr engine.  These enforce the
// namespace invariants (exact record match, no duplicate, no self-parent,
// valid name) before touching the fork and return one combined inode image.
Result<PreparedInode> PrepareParentPointerAdd(
    XfsTransaction& txn, BlockAllocator& alloc, BlockDevice& device,
    const XfsGeometry& geo, const XfsInode& child, const XattrFork& fork,
    std::vector<uint8_t> raw, const ParentPointer& pointer);

Result<PreparedInode> PrepareParentPointerRemove(
    XfsTransaction& txn, BlockAllocator& alloc, BlockDevice& device,
    const XfsGeometry& geo, const XfsInode& child, const XattrFork& fork,
    std::vector<uint8_t> raw, const ParentPointer& pointer,
    bool missing_ok = false);

// Remove old_pointer and insert new_pointer in one incremental edit.  This is
// the rename/replace operation (xfs_parent_replacename).
Result<PreparedInode> PrepareParentPointerReplace(
    XfsTransaction& txn, BlockAllocator& alloc, BlockDevice& device,
    const XfsGeometry& geo, const XfsInode& child, const XattrFork& fork,
    std::vector<uint8_t> raw, const ParentPointer& old_pointer,
    const ParentPointer& new_pointer, bool missing_ok = false);

// Reads the child inode and its attr fork from the device.  The caller must
// already have the parsed inode.  Used by namespace operations.
Result<ParentPointer> FindExactParentPointer(BlockDevice& device,
                                             const XfsGeometry& geo,
                                             const XfsInode& child,
                                             const ParentPointer& pointer);

}  // namespace xfs
