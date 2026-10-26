// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - parent pointers (RW-6E).
//
// Parent pointers are filesystem-owned attributes in the private
// XFS_ATTR_PARENT namespace (xfs_da_format.h).  They are the reverse half of
// the namespace:
//
//   dirent  (parent inode, name) -> child inode
//   parent pointer on the child    -> (parent inode, parent generation, name)
//
// The attribute name is the dirent name; the local attribute value is the
// 12-byte packed xfs_parent_rec:
//
//   +0  __be64 parent inode
//   +8  __be32 parent generation
//
// The attribute hash is xfs_dir2_hashname(name) ^
// upper_32_bits(parent_ino) ^ lower_32_bits(parent_ino).  Generation is not
// part of the hash, but it is part of the lookup value so a reused inode
// number is detectable.
//
// This header is the typed view used by the namespace writer, the checker and
// diagnostics.  No caller should assemble parent-pointer bytes by hand.
// Layout reference: Linux fs/xfs/libxfs/xfs_parent.c, xfs_parent.h,
// xfs_da_format.h; xfsprogs 7.2.0 repair/pptr.c and scrub/parent.c.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/inode.h"
#include "xfs/superblock.h"
#include "xfs/xattr.h"

namespace xfs {

constexpr size_t kParentRecSize = 12;
constexpr size_t kParentGenOffset = 8;

// One verified parent pointer.
struct ParentPointer {
  uint64_t parent_ino = 0;
  uint32_t parent_gen = 0;
  std::string name;  // exact dirent bytes

  bool operator==(const ParentPointer& other) const {
    return parent_ino == other.parent_ino &&
           parent_gen == other.parent_gen && name == other.name;
  }
  bool operator!=(const ParentPointer& other) const { return !(*this == other); }
};

// xfs_parent_hashval(): the dirent name hash mixed with the parent inode.
uint32_t ParentPointerHash(std::string_view name, uint64_t parent_ino);

// xfs_dir2_namecheck(): a parent pointer's name is a directory entry name,
// never a path, never empty and never "." or "..".
Status ValidateParentPointerName(std::string_view name,
                                 uint64_t child_ino = 0);

// Encodes the local attribute value.  `name` is not part of the value.
std::vector<uint8_t> EncodeParentPointerValue(const ParentPointer& pointer);

// Decodes and fully validates one attribute entry (namespace, local value,
// value length, parent inode range, generation field presence and hash).
// `check_parent` optionally opens the parent inode and verifies that it is a
// directory with the recorded generation.
Result<ParentPointer> DecodeParentPointer(const XattrEntry& entry,
                                          const XfsGeometry& geo,
                                          uint64_t child_ino,
                                          BlockDevice* device = nullptr,
                                          bool check_parent = false);

// Builds the attribute entry a namespace operation would insert.  The hash
// is computed; the value is local and 12 bytes.
XattrEntry MakeParentPointerEntry(const ParentPointer& pointer);

// Lists every complete parent pointer of an inode in attribute-fork order.
// A malformed parent entry is an error, never silently skipped.
Result<std::vector<ParentPointer>> ListParents(BlockDevice& device,
                                               const XfsGeometry& geo,
                                               const XfsInode& inode);

// Finds one exact parent pointer.  std::nullopt means "not present"; a
// malformed attr fork is still an error.
Result<std::optional<ParentPointer>> FindParent(
    BlockDevice& device, const XfsGeometry& geo, const XfsInode& inode,
    uint64_t parent_ino, uint32_t parent_gen, std::string_view name);

// The result of the bidirectional namespace <-> parent-pointer audit.
struct ParentNamespaceReport {
  uint64_t directory_edges = 0;   // real non-dot dirents
  uint64_t parent_pointers = 0;   // decoded parent attrs
  uint64_t checked_inodes = 0;
  std::vector<std::string> missing;                // dirent without PP
  std::vector<std::string> unexpected;             // PP without dirent
  std::vector<std::string> wrong_parent_generation;
  std::vector<std::string> wrong_name;
  std::vector<std::string> duplicate;
  std::vector<std::string> dangling_parent;        // missing/non-dir parent
  std::vector<std::string> wrong_parent_type;
  std::vector<std::string> non_directory_parent;
  std::vector<std::string> stale_generation;
  std::vector<std::string> wrong_dotdot;
  std::vector<std::string> link_count_mismatch;
  std::vector<std::string> malformed;
  std::vector<std::string> unreachable_with_parents;

  bool clean() const {
    return missing.empty() && unexpected.empty() &&
           wrong_parent_generation.empty() && wrong_name.empty() &&
           duplicate.empty() && dangling_parent.empty() &&
           wrong_parent_type.empty() && non_directory_parent.empty() &&
           stale_generation.empty() && wrong_dotdot.empty() &&
           link_count_mismatch.empty() && malformed.empty() &&
           unreachable_with_parents.empty();
  }
  std::string Summary() const;
};

}  // namespace xfs
