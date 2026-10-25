// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - extended attributes (RW-6D).
//
// This header is the typed, byte-order-independent view of the second inode
// fork.  It deliberately does not expose raw leaf/node bytes: callers deal in
// namespace + name + value entries, and the verifier owns the on-disk layout.
//
// Layout reference: Linux fs/xfs/libxfs/xfs_da_format.h and
// xfs_attr_leaf.c, xfs_attr_remote.c, xfs_da_btree.c.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/extent.h"
#include "xfs/inode.h"
#include "xfs/superblock.h"

namespace xfs {

// xfs_da_format.h: on-disk namespace bits in xfs_attr_leaf_entry.flags and
// shortform entry flags.  XFS_ATTR_LOCAL is a value-placement bit, not a
// namespace; XFS_ATTR_INCOMPLETE is a transient operation marker.
constexpr uint8_t kAttrLocalBit = 0;
constexpr uint8_t kAttrRootBit = 1;
constexpr uint8_t kAttrSecureBit = 2;
constexpr uint8_t kAttrParentBit = 3;
constexpr uint8_t kAttrIncompleteBit = 7;

constexpr uint8_t kAttrLocal = 1u << kAttrLocalBit;
constexpr uint8_t kAttrRoot = 1u << kAttrRootBit;
constexpr uint8_t kAttrSecure = 1u << kAttrSecureBit;
constexpr uint8_t kAttrParent = 1u << kAttrParentBit;
constexpr uint8_t kAttrIncomplete = 1u << kAttrIncompleteBit;

constexpr uint8_t kAttrNspOnDiskMask = kAttrRoot | kAttrSecure | kAttrParent;
constexpr uint8_t kAttrPrivateNspMask = kAttrParent;
constexpr uint8_t kAttrOnDiskMask =
    kAttrNspOnDiskMask | kAttrLocal | kAttrIncomplete;

// Linux MAXNAMELEN is 256 including the NUL, so a name has at most 255
// bytes on disk.  The local name/value fields are u8/u16 respectively;
// XFS_XATTR_SIZE_MAX is 65536 bytes.
constexpr uint32_t kXattrMaxNameBytes = 255;
constexpr uint32_t kXattrMaxValueBytes = 1u << 16;

// Attribute DA tree limits.
constexpr size_t kAttrLeafMapSize = 3;
constexpr uint16_t kAttrLeafMagicV5 = 0x3bee;
constexpr uint16_t kAttrDaNodeMagicV5 = 0x3ebe;
constexpr size_t kAttrDaHeaderSize = 56;  // xfs_da3_blkinfo
constexpr size_t kAttrLeafHeaderSize = 80;  // xfs_attr3_leaf_hdr
constexpr size_t kAttrNodeHeaderSize = 64;  // xfs_da3_node_hdr
constexpr size_t kAttrLeafEntrySize = 8;
constexpr size_t kAttrNodeEntrySize = 8;
constexpr size_t kAttrRemoteHeaderSize = 56;  // xfs_attr3_rmt_hdr
constexpr uint32_t kAttrNullBlock = 0;        // forw/back/before sentinel
constexpr uint16_t kAttrMaxTreeDepth = 5;     // XFS_DA_NODE_MAXDEPTH
constexpr uint64_t kAttrNullCommitLsn = ~0ULL;  // NULLCOMMITLSN

// Human-readable namespace.  `System` is the on-disk ROOT namespace entry
// used by POSIX ACLs: Linux exposes it as system.posix_acl_{access,default}
// and stores the literal names SGI_ACL_FILE / SGI_ACL_DEFAULT.
enum class XattrNamespaceKind {
  kUser,
  kTrusted,
  kSecurity,
  kSystemAcl,
  kParent,   // private, read/preserve only in RW-6D
  kUnknown,
};

struct XattrNamespace {
  XattrNamespaceKind kind = XattrNamespaceKind::kUser;
  uint8_t flags = 0;  // exactly one namespace bit, or 0 for user

  static XattrNamespace FromFlags(uint8_t flags);
  // Parses a userspace name prefix.  `system.posix_acl_access` and
  // `system.posix_acl_default` map to the XFS root namespace names
  // SGI_ACL_FILE / SGI_ACL_DEFAULT.  Parent pointers are only accepted when
  // `allow_parent` is true (the raw CLI uses this for read/diagnostics).
  static Result<XattrNamespace> ParseName(std::string_view name,
                                          std::string* on_disk_name,
                                          bool allow_parent = false);
  // On-disk namespace name for this namespace.
  const char* Prefix() const;
  std::string DisplayName(std::string_view on_disk_name) const;
  bool IsPrivate() const { return kind == XattrNamespaceKind::kParent; }
};

// One verified attribute.  `name` is the on-disk suffix (the part after the
// VFS namespace prefix), exactly the bytes stored in the shortform/leaf entry.
struct XattrEntry {
  uint32_t hash = 0;
  uint8_t flags = 0;  // namespace + LOCAL + INCOMPLETE
  std::string name;
  std::vector<uint8_t> value;
  bool remote = false;
  uint32_t remote_lbno = 0;      // logical attr-fork block
  uint32_t remote_blocks = 0;    // number of remote value blocks
  uint32_t remote_value_len = 0; // valuelen stored in the leaf entry
  bool incomplete = false;       // transient xfs_attr3 state

  bool IsLocal() const { return (flags & kAttrLocal) != 0; }
  bool IsIncomplete() const { return (flags & kAttrIncomplete) != 0; }
  XattrNamespace Namespace() const { return XattrNamespace::FromFlags(flags); }
  std::string DisplayName(bool include_private = false) const;
  // Namespace + exact on-disk name bytes.
  bool SameKey(const XattrEntry& other) const {
    return (flags & kAttrNspOnDiskMask) ==
               (other.flags & kAttrNspOnDiskMask) &&
           name == other.name;
  }
};

// The complete verified attribute fork.  `entries` is in hash order for leaf
// and node formats and in on-disk insertion order for shortform, matching the
// kernel's list order.  Incomplete entries are included in `entries` and
// flagged; normal list/get callers filter them out.
enum class XattrForkFormat { kNone, kShortform, kLeaf, kNode, kExtentsEmpty };

// One DA leaf in traversal/key order.  `first_entry`/`entry_count` index into
// XattrFork::entries, which is the flattened concatenation of all leaves.
struct XattrDaLeaf {
  uint32_t logical = 0;
  uint32_t forw = 0;
  uint32_t back = 0;
  uint32_t first_entry = 0;
  uint32_t entry_count = 0;
};

struct XattrDaNodeEntry {
  uint32_t hash = 0;  // last hash in the child subtree
  uint32_t child = 0; // logical attr-fork block
};

// One DA node level block.  `forw`/`back` are sibling links at the same level.
struct XattrDaNode {
  uint32_t logical = 0;
  uint32_t forw = 0;
  uint32_t back = 0;
  uint16_t level = 0;
  std::vector<XattrDaNodeEntry> entries;
};

struct XattrFork {
  XattrForkFormat format = XattrForkFormat::kNone;
  uint8_t forkoff = 0;
  uint8_t aformat = 0;
  uint32_t anextents = 0;
  uint16_t root_level = 0;       // 0 for a single leaf/shortform
  uint64_t tree_blocks = 0;      // DA leaf + node blocks reachable from root
  uint64_t remote_blocks = 0;    // total remote value blocks
  uint32_t leaf_count = 0;
  std::vector<XfsExtent> extents;  // complete attr-fork extent map
  std::vector<XattrEntry> entries;
  std::vector<XattrDaLeaf> leaves;
  std::vector<XattrDaNode> nodes;

  bool HasIncomplete() const {
    for (const XattrEntry& e : entries) {
      if (e.incomplete) return true;
    }
    return false;
  }
};

// xfs_da_hashname: rotate the hash by 7 bits, then XOR each character.  The
// same function is used by the kernel, xfs_db and xfs_repair.
uint32_t AttrHashName(const std::string& name);

// Size accounting, exactly xfs_attr_leaf_entsize_{local,remote}().
size_t AttrLocalEntrySize(size_t name_len, size_t value_len);
size_t AttrRemoteEntrySize(size_t name_len);
size_t AttrLocalEntryMax(size_t block_size);  // 3/4 block

// Namespace/name validation.  `for_storage` uses the kernel's on-disk name
// check (no NULs, < MAXNAMELEN, one namespace bit).  VFS prefixes are checked
// separately by ParseName.
Status ValidateXattrName(std::string_view on_disk_name, uint8_t flags,
                         bool allow_incomplete = false);

// Loads and fully verifies an inode's attr fork:
//   - the attr extent map (EXTENTS or BTREE, NREXT64 counters);
//   - every DA leaf/node block (magic, CRC, UUID, blkno, owner, LSN,
//     sibling links, hash order, node keys, levels, reachability);
//   - every remote value block (XFS_ARM3 header, owner, offset, bytes, CRC).
// It accepts Linux-created local, single-leaf, node/multi-level-node and
// remote-value forks.  A corrupt structure is rejected, never approximated.
Result<XattrFork> ReadXattrFork(BlockDevice& device, const XfsGeometry& geo,
                                const XfsInode& inode, uint64_t sb_lsn = 0);

// Convenience views over ReadXattrFork.  They skip private namespaces and
// incomplete entries; `include_private` exposes parent-pointer entries for
// diagnostics and preservation checks.
Result<std::vector<XattrEntry>> ListXattrs(BlockDevice& device,
                                          const XfsGeometry& geo,
                                          const XfsInode& inode,
                                          bool include_private = false);
Result<XattrEntry> GetXattr(BlockDevice& device, const XfsGeometry& geo,
                            const XfsInode& inode, std::string_view name);

// Maps a logical attr-fork block to its physical filesystem block.  Returns
// false when the logical block is a hole.  Used by the oracle and repair paths.
Result<uint64_t> AttrLogicalToPhysical(const XattrFork& fork,
                                       const XfsGeometry& geo,
                                       uint64_t logical_block);

}  // namespace xfs
