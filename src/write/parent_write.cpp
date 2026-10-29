// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - parent-pointer mutations inside namespace transactions.
//
// The namespace core owns the forward directory entry.  These helpers own the
// reverse record on the child and always go through the RW-6D attribute
// engine (shortform -> leaf -> node -> BMBT), never a second attr writer:
//
//   create / mkdir / symlink  -> PrepareParentPointerAdd
//   link                      -> PrepareParentPointerAdd
//   unlink / rmdir            -> PrepareParentPointerRemove
//   same/cross-directory rename -> PrepareParentPointerReplace
//
// Every helper returns one complete inode image.  The caller adds link-count,
// timestamp and (for a directory move) ".." changes to that same image and
// logs it exactly once, so a crash can only leave the old or the new
// namespace + parent pointers.
#include <algorithm>
#include <cstddef>
#include <cstring>

#include "xfs/endian.h"
#include "write/xattr_write_internal.h"

namespace xfs {
namespace {

Status ValidatePointer(const XfsGeometry& geo, const XfsInode& child,
                       const ParentPointer& pointer) {
  if (!geo.features.parent) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "parent pointer on a filesystem without the parent feature")
        .Ino(child.ino)
        .Metadata("parent pointer");
  }
  Status name_ok = ValidateParentPointerName(pointer.name, child.ino);
  if (!name_ok.ok()) return name_ok;
  if (pointer.parent_ino == child.ino) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "parent pointer would point at the child itself")
        .Ino(child.ino)
        .Metadata("parent pointer");
  }
  if (!geo.InoValid(pointer.parent_ino)) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "parent pointer parent inode is out of range")
        .Ino(child.ino)
        .Actual(pointer.parent_ino)
        .Metadata("parent pointer");
  }
  return OkStatus();
}

bool IsParentEntry(const XattrEntry& entry) {
  return (entry.flags & kAttrNspOnDiskMask) == kAttrParent;
}

Result<ParentPointer> DecodeEntry(const XattrEntry& entry,
                                  const XfsGeometry& geo, uint64_t child_ino) {
  return DecodeParentPointer(entry, geo, child_ino);
}

// Finds an exact parent record in a decoded fork.  `hash_collision` receives
// a same-name/same-parent entry with a different generation, which is a stale
// pointer rather than a valid second record.
Result<size_t> FindExact(const std::vector<XattrEntry>& entries,
                         const XfsGeometry& geo, uint64_t child_ino,
                         const ParentPointer& pointer, bool* stale) {
  if (stale != nullptr) *stale = false;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (!IsParentEntry(entries[i])) continue;
    auto decoded = DecodeEntry(entries[i], geo, child_ino);
    if (!decoded.ok()) return decoded.error();
    const ParentPointer& p = decoded.value();
    if (p == pointer) return i;
    if (p.parent_ino == pointer.parent_ino && p.name == pointer.name) {
      if (stale != nullptr) *stale = true;
    }
  }
  return size_t(-1);
}

Result<PreparedInode> NoChange(std::vector<uint8_t> raw) {
  PreparedInode out;
  out.image = std::move(raw);
  return out;
}

}  // namespace

Result<ParentPointer> FindExactParentPointer(BlockDevice& device,
                                             const XfsGeometry& geo,
                                             const XfsInode& child,
                                             const ParentPointer& pointer) {
  auto fork = ReadXattrFork(device, geo, child);
  if (!fork.ok()) return fork.error();
  bool stale = false;
  auto index = FindExact(fork.value().entries, geo, child.ino, pointer, &stale);
  if (!index.ok()) return index.error();
  if (index.value() == size_t(-1)) {
    if (stale) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "parent pointer exists with a stale generation")
          .Ino(child.ino)
          .Metadata("parent pointer");
    }
    return XFS_ERR(XfsError::kNotFound, "parent pointer does not exist")
        .Ino(child.ino)
        .Metadata("parent pointer");
  }
  return DecodeEntry(fork.value().entries[index.value()], geo, child.ino);
}

Result<PreparedInode> PrepareParentPointerAdd(
    XfsTransaction& txn, BlockAllocator& alloc, BlockDevice& device,
    const XfsGeometry& geo, const XfsInode& child, const XattrFork& fork,
    std::vector<uint8_t> raw, const ParentPointer& pointer) {
  Status valid = ValidatePointer(geo, child, pointer);
  if (!valid.ok()) return valid.error();

  std::vector<XattrEntry> entries = fork.entries;
  bool stale = false;
  auto existing = FindExact(entries, geo, child.ino, pointer, &stale);
  if (!existing.ok()) return existing.error();
  if (existing.value() != size_t(-1)) {
    // Idempotent recovery: the deferred parent add may already be visible.
    return NoChange(std::move(raw));
  }
  if (stale) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "parent pointer with the same parent/name but a different "
                   "generation already exists")
        .Ino(child.ino)
        .Metadata("parent pointer");
  }

  XattrEntry entry = MakeParentPointerEntry(pointer);
  entries.push_back(entry);
  const int changed_index = static_cast<int>(entries.size()) - 1;
  return PrepareXattrMutation(
      txn, alloc, device, geo, child, fork, std::move(raw), std::move(entries),
      changed_index, std::nullopt, std::nullopt, false);
}

Result<PreparedInode> PrepareParentPointerRemove(
    XfsTransaction& txn, BlockAllocator& alloc, BlockDevice& device,
    const XfsGeometry& geo, const XfsInode& child, const XattrFork& fork,
    std::vector<uint8_t> raw, const ParentPointer& pointer,
    bool missing_ok) {
  Status valid = ValidatePointer(geo, child, pointer);
  if (!valid.ok()) return valid.error();

  std::vector<XattrEntry> entries = fork.entries;
  bool stale = false;
  auto existing = FindExact(entries, geo, child.ino, pointer, &stale);
  if (!existing.ok()) return existing.error();
  if (existing.value() == size_t(-1)) {
    if (stale) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "parent pointer exists with a stale generation")
          .Ino(child.ino)
          .Metadata("parent pointer");
    }
    if (missing_ok) return NoChange(std::move(raw));
    return XFS_ERR(XfsError::kCorruptInode,
                   "namespace entry has no corresponding parent pointer")
        .Ino(child.ino)
        .Metadata("parent pointer");
  }

  XattrEntry removed = entries[existing.value()];
  entries.erase(entries.begin() + static_cast<ptrdiff_t>(existing.value()));
  return PrepareXattrMutation(txn, alloc, device, geo, child, fork,
                              std::move(raw), std::move(entries), -1,
                              std::nullopt, std::move(removed), false);
}

Result<PreparedInode> PrepareParentPointerReplace(
    XfsTransaction& txn, BlockAllocator& alloc, BlockDevice& device,
    const XfsGeometry& geo, const XfsInode& child, const XattrFork& fork,
    std::vector<uint8_t> raw, const ParentPointer& old_pointer,
    const ParentPointer& new_pointer, bool missing_ok) {
  Status old_valid = ValidatePointer(geo, child, old_pointer);
  if (!old_valid.ok()) return old_valid.error();
  Status new_valid = ValidatePointer(geo, child, new_pointer);
  if (!new_valid.ok()) return new_valid.error();
  if (old_pointer == new_pointer) return NoChange(std::move(raw));

  std::vector<XattrEntry> entries = fork.entries;
  bool stale_old = false;
  auto old_index =
      FindExact(entries, geo, child.ino, old_pointer, &stale_old);
  if (!old_index.ok()) return old_index.error();
  bool stale_new = false;
  auto new_index =
      FindExact(entries, geo, child.ino, new_pointer, &stale_new);
  if (!new_index.ok()) return new_index.error();

  std::optional<XattrEntry> removed;
  if (old_index.value() != size_t(-1)) {
    removed = entries[old_index.value()];
    entries.erase(entries.begin() +
                  static_cast<ptrdiff_t>(old_index.value()));
    // The removal may have shifted the index of the already-present new
    // record; recompute after the erase.
    new_index = FindExact(entries, geo, child.ino, new_pointer, &stale_new);
    if (!new_index.ok()) return new_index.error();
  } else if (!missing_ok) {
    if (stale_old) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "old parent pointer has a stale generation")
          .Ino(child.ino)
          .Metadata("parent pointer");
    }
    return XFS_ERR(XfsError::kCorruptInode,
                   "rename source has no corresponding parent pointer")
        .Ino(child.ino)
        .Metadata("parent pointer");
  }
  if (stale_new && new_index.value() == size_t(-1)) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "new parent pointer collides with a stale generation")
        .Ino(child.ino)
        .Metadata("parent pointer");
  }

  int changed_index = -1;
  if (new_index.value() == size_t(-1)) {
    XattrEntry added = MakeParentPointerEntry(new_pointer);
    entries.push_back(added);
    changed_index = static_cast<int>(entries.size()) - 1;
  }

  if (!removed.has_value() && changed_index < 0) {
    return NoChange(std::move(raw));
  }
  return PrepareXattrMutation(txn, alloc, device, geo, child, fork,
                              std::move(raw), std::move(entries), changed_index,
                              std::nullopt, std::move(removed), false);
}

}  // namespace xfs
