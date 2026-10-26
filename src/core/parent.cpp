// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - typed parent pointers and the namespace <-> attr audit.
//
// The on-disk rules are from Linux fs/xfs/libxfs/xfs_parent.c and
// xfs_da_format.h, and from xfsprogs-7.2.0 repair/pptr.c and scrub/parent.c:
//
//   * attr namespace XFS_ATTR_PARENT, attr name = dirent name;
//   * local value = struct xfs_parent_rec { __be64 p_ino; __be32 p_gen; };
//   * hash = xfs_dir2_hashname(name) ^ high32(p_ino) ^ low32(p_ino);
//   * "." and ".." never get parent pointers;
//   * the root directory has no parent pointer; its ".." points to itself;
//   * a linked directory has exactly one parent pointer (not nlink);
//   * a non-directory has one parent pointer per hard link;
//   * a directory with nlink 0 (unlinked, awaiting inactivation) has none.
#include "xfs/parent.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "xfs/directory.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"

namespace xfs {
namespace {

bool IsParentNamespace(uint8_t flags) {
  return (flags & kAttrNspOnDiskMask) == kAttrParent;
}

std::string PointerText(const ParentPointer& pptr) {
  return std::to_string(pptr.parent_ino) + "/" +
         std::to_string(pptr.parent_gen) + "/'" + pptr.name + "'";
}

}  // namespace

uint32_t ParentPointerHash(std::string_view name, uint64_t parent_ino) {
  const uint32_t dirhash = XfsDirNameHash(name);
  return dirhash ^ static_cast<uint32_t>(parent_ino >> 32) ^
         static_cast<uint32_t>(parent_ino);
}

Status ValidateParentPointerName(std::string_view name, uint64_t child_ino) {
  if (name.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "parent pointer name is empty")
        .Metadata("parent pointer");
  }
  if (name.size() >= kXattrMaxNameBytes + 1) {
    return XFS_ERR(XfsError::kNameTooLong,
                   "parent pointer name is longer than MAXNAMELEN - 1")
        .Metadata("parent pointer")
        .Actual(name.size());
  }
  if (name.find('\0') != std::string_view::npos) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "parent pointer name contains a NUL byte")
        .Metadata("parent pointer");
  }
  if (name.find('/') != std::string_view::npos) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "parent pointer name contains a slash")
        .Metadata("parent pointer");
  }
  // These two names are never represented by a parent pointer: "." is the
  // inode itself and ".." is the forward parent link.  A pointer with either
  // name cannot have a matching non-dot dirent.
  if (name == "." || name == "..") {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "parent pointer name is a dot entry")
        .Metadata("parent pointer")
        .Ino(child_ino);
  }
  return OkStatus();
}

std::vector<uint8_t> EncodeParentPointerValue(const ParentPointer& pointer) {
  std::vector<uint8_t> value(kParentRecSize, 0);
  PutBE64(value.data(), 0, pointer.parent_ino);
  PutBE32(value.data(), kParentGenOffset, pointer.parent_gen);
  return value;
}

Result<ParentPointer> DecodeParentPointer(const XattrEntry& entry,
                                          const XfsGeometry& geo,
                                          uint64_t child_ino,
                                          BlockDevice* device,
                                          bool check_parent) {
  const uint8_t nsp = entry.flags & kAttrNspOnDiskMask;
  if (nsp != kAttrParent) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "attribute is not in the parent-pointer namespace")
        .Ino(child_ino)
        .Actual(nsp)
        .Metadata("parent pointer");
  }
  if (entry.flags & (kAttrIncomplete | kAttrRoot | kAttrSecure)) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "parent pointer has an invalid namespace/transient flag")
        .Ino(child_ino)
        .Actual(entry.flags)
        .Metadata("parent pointer");
  }
  if (entry.remote) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "parent pointer value is remote; xfs_parent_rec must be "
                   "local")
        .Ino(child_ino)
        .Metadata("parent pointer");
  }
  if (entry.value.size() != kParentRecSize) {
    return XFS_ERR(XfsError::kBadLength,
                   "parent pointer value is not a 12-byte xfs_parent_rec")
        .Ino(child_ino)
        .Expected(kParentRecSize)
        .Actual(entry.value.size())
        .Metadata("parent pointer");
  }
  Status name_ok = ValidateParentPointerName(entry.name, child_ino);
  if (!name_ok.ok()) return name_ok.error();

  ParentPointer out;
  out.parent_ino = GetBE64(entry.value.data(), 0);
  out.parent_gen = GetBE32(entry.value.data(), kParentGenOffset);
  out.name = entry.name;

  if (out.parent_ino == child_ino) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "parent pointer points at its own inode")
        .Ino(child_ino)
        .Metadata("parent pointer");
  }
  if (!geo.InoValid(out.parent_ino)) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "parent pointer parent inode is out of range")
        .Ino(child_ino)
        .Actual(out.parent_ino)
        .Metadata("parent pointer");
  }
  const uint32_t expected_hash = ParentPointerHash(out.name, out.parent_ino);
  if (entry.hash != 0 && entry.hash != expected_hash) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "parent pointer hash does not match name and parent inode")
        .Ino(child_ino)
        .Expected(expected_hash)
        .Actual(entry.hash)
        .Metadata("parent pointer");
  }

  if (check_parent) {
    if (device == nullptr) {
      return XFS_ERR(XfsError::kInternal,
                     "parent validation needs the block device")
          .Ino(child_ino)
          .Metadata("parent pointer");
    }
    auto parent = ReadInode(*device, geo, out.parent_ino);
    if (!parent.ok()) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "parent pointer references an unreadable inode: " +
                         parent.error().message)
          .Ino(child_ino)
          .Actual(out.parent_ino)
          .Metadata("parent pointer");
    }
    if (!parent.value().IsDir()) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "parent pointer references a non-directory inode")
          .Ino(child_ino)
          .Actual(out.parent_ino)
          .Metadata("parent pointer");
    }
    if (parent.value().gen != out.parent_gen) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "parent pointer generation is stale")
          .Ino(child_ino)
          .Expected(parent.value().gen)
          .Actual(out.parent_gen)
          .Metadata("parent pointer");
    }
  }
  return out;
}

XattrEntry MakeParentPointerEntry(const ParentPointer& pointer) {
  XattrEntry entry;
  entry.flags = kAttrParent;
  entry.name = pointer.name;
  entry.hash = ParentPointerHash(pointer.name, pointer.parent_ino);
  entry.value = EncodeParentPointerValue(pointer);
  entry.remote = false;
  entry.incomplete = false;
  return entry;
}

Result<std::vector<ParentPointer>> ListParents(BlockDevice& device,
                                               const XfsGeometry& geo,
                                               const XfsInode& inode) {
  auto fork = ReadXattrFork(device, geo, inode);
  if (!fork.ok()) return fork.error();
  std::vector<ParentPointer> out;
  for (const XattrEntry& entry : fork.value().entries) {
    if (!IsParentNamespace(entry.flags)) continue;
    auto decoded = DecodeParentPointer(entry, geo, inode.ino);
    if (!decoded.ok()) return decoded.error();
    out.push_back(std::move(decoded.value()));
  }
  return out;
}

Result<std::optional<ParentPointer>> FindParent(
    BlockDevice& device, const XfsGeometry& geo, const XfsInode& inode,
    uint64_t parent_ino, uint32_t parent_gen, std::string_view name) {
  auto parents = ListParents(device, geo, inode);
  if (!parents.ok()) return parents.error();
  for (const ParentPointer& pointer : parents.value()) {
    if (pointer.parent_ino == parent_ino &&
        pointer.parent_gen == parent_gen && pointer.name == name) {
      return std::optional<ParentPointer>(pointer);
    }
  }
  return std::optional<ParentPointer>();
}

Result<ParentNamespaceReport> XfsFilesystem::VerifyParentPointers() const {
  ParentNamespaceReport report;
  if (!geometry_.features.parent) {
    // Nothing to audit: parent pointers are not part of this filesystem.
    report.clean();
    return report;
  }

  struct DirState {
    uint64_t parent = 0;
    uint32_t parent_gen = 0;
    std::string name;
    bool seen = false;
    uint64_t subdirs = 0;
  };
  std::map<uint64_t, DirState> dirs;
  std::unordered_map<uint64_t, std::vector<ParentPointer>> expected;
  std::unordered_set<uint64_t> visited;

  auto root = OpenInode(geometry_.rootino);
  if (!root.ok()) return root.error();
  dirs.emplace(geometry_.rootino,
               DirState{geometry_.rootino, root.value().gen, "", false, 0});

  std::vector<uint64_t> stack{geometry_.rootino};
  while (!stack.empty()) {
    const uint64_t dir_ino = stack.back();
    stack.pop_back();
    if (!visited.insert(dir_ino).second) continue;
    auto state_it = dirs.find(dir_ino);
    if (state_it == dirs.end()) {
      return XFS_ERR(XfsError::kInternal, "directory state lost during audit")
          .Ino(dir_ino)
          .Metadata("parent pointers");
    }
    DirState& state = state_it->second;
    state.seen = true;

    auto inode = OpenInode(dir_ino);
    if (!inode.ok()) {
      report.malformed.push_back("directory " + std::to_string(dir_ino) +
                                 " unreadable: " + inode.error().message);
      continue;
    }
    if (!inode.value().IsDir()) {
      report.wrong_parent_type.push_back(
          "inode " + std::to_string(dir_ino) +
          " is referenced as a directory but has mode " +
          std::to_string(inode.value().mode));
      continue;
    }
    auto listing = ListDirectory(dir_ino);
    if (!listing.ok()) {
      report.malformed.push_back("directory " + std::to_string(dir_ino) +
                                 " unreadable: " + listing.error().message);
      continue;
    }

    bool has_dot = false;
    bool has_dotdot = false;
    uint64_t subdirs = 0;
    std::set<std::string> names;
    for (const XfsDirEntry& entry : listing.value()) {
      if (entry.name == ".") {
        has_dot = true;
        if (entry.ino != dir_ino) {
          report.wrong_dotdot.push_back(
              "directory " + std::to_string(dir_ino) +
              " '.' points at " + std::to_string(entry.ino));
        }
        continue;
      }
      if (entry.name == "..") {
        has_dotdot = true;
        const uint64_t want =
            dir_ino == geometry_.rootino ? geometry_.rootino : state.parent;
        if (entry.ino != want) {
          report.wrong_dotdot.push_back(
              "directory " + std::to_string(dir_ino) + " '..' points at " +
              std::to_string(entry.ino) + ", expected " +
              std::to_string(want));
        }
        continue;
      }
      if (!names.insert(entry.name).second) {
        report.malformed.push_back(
            "directory " + std::to_string(dir_ino) +
            " has a duplicate dirent name '" + entry.name + "'");
        continue;
      }
      ++report.directory_edges;

      auto child = OpenInode(entry.ino);
      if (!child.ok()) {
        report.malformed.push_back(
            "directory " + std::to_string(dir_ino) + " entry '" + entry.name +
            "' references unreadable inode " + std::to_string(entry.ino));
        continue;
      }
      if (child.value().IsDir()) {
        if (entry.ino == geometry_.rootino) {
          report.wrong_parent_type.push_back(
              "root inode appears as a directory entry");
          continue;
        }
        auto existing = dirs.find(entry.ino);
        if (existing != dirs.end()) {
          report.malformed.push_back(
              "directory " + std::to_string(entry.ino) +
              " is linked twice (second parent " + std::to_string(dir_ino) +
              " name '" + entry.name + "')");
          continue;
        }
        dirs.emplace(entry.ino,
                     DirState{dir_ino, inode.value().gen, entry.name, false, 0});
        ++subdirs;
        stack.push_back(entry.ino);
      } else {
        expected[entry.ino].push_back(
            ParentPointer{dir_ino, inode.value().gen, entry.name});
      }
    }
    state.subdirs = subdirs;

    if (!has_dot) {
      report.wrong_dotdot.push_back("directory " + std::to_string(dir_ino) +
                                    " has no '.' entry");
    }
    if (!has_dotdot) {
      report.wrong_dotdot.push_back("directory " + std::to_string(dir_ino) +
                                    " has no '..' entry");
    }
    if (inode.value().nlink == 0) {
      report.link_count_mismatch.push_back(
          "directory " + std::to_string(dir_ino) +
          " is reachable but has nlink 0");
    } else if (inode.value().nlink != 2 + subdirs) {
      report.link_count_mismatch.push_back(
          "directory " + std::to_string(dir_ino) + " nlink " +
          std::to_string(inode.value().nlink) + " != 2 + " +
          std::to_string(subdirs) + " subdirectories");
    }
  }

  // Root has no parent pointer.
  expected.erase(geometry_.rootino);

  // Compare every reachable inode's actual parent pointers with the edge set
  // derived from the forward directory walk.
  std::set<uint64_t> checked;
  for (const auto& [ino, wanted] : expected) {
    if (!checked.insert(ino).second) continue;
    auto inode = OpenInode(ino);
    if (!inode.ok()) {
      report.malformed.push_back("inode " + std::to_string(ino) +
                                 " unreadable: " + inode.error().message);
      continue;
    }
    auto actual = ListParents(*device_, geometry_, inode.value());
    if (!actual.ok()) {
      report.malformed.push_back("inode " + std::to_string(ino) +
                                 " parent attrs: " + actual.error().message);
      continue;
    }
    ++report.checked_inodes;
    report.parent_pointers += actual.value().size();

    std::set<std::tuple<uint64_t, uint32_t, std::string>> seen;
    std::vector<bool> matched(wanted.size(), false);
    for (const ParentPointer& pointer : actual.value()) {
      const auto exact =
          std::make_tuple(pointer.parent_ino, pointer.parent_gen, pointer.name);
      if (!seen.insert(exact).second) {
        report.duplicate.push_back("inode " + std::to_string(ino) + " " +
                                   PointerText(pointer));
      }

      auto parent = OpenInode(pointer.parent_ino);
      if (!parent.ok()) {
        report.dangling_parent.push_back(
            "inode " + std::to_string(ino) + " -> " + PointerText(pointer) +
            " (unreadable parent)");
        continue;
      }
      if (!parent.value().IsDir()) {
        report.non_directory_parent.push_back(
            "inode " + std::to_string(ino) + " -> " + PointerText(pointer));
        continue;
      }
      if (parent.value().gen != pointer.parent_gen) {
        report.wrong_parent_generation.push_back(
            "inode " + std::to_string(ino) + " -> " + PointerText(pointer) +
            "; live parent generation is " +
            std::to_string(parent.value().gen));
        report.stale_generation.push_back(
            "inode " + std::to_string(ino) + " -> " + PointerText(pointer));
        continue;
      }

      bool found = false;
      for (size_t i = 0; i < wanted.size(); ++i) {
        if (wanted[i] == pointer) {
          matched[i] = true;
          found = true;
          break;
        }
      }
      if (found) continue;

      bool wrong_gen = false;
      bool wrong_name = false;
      for (const ParentPointer& want : wanted) {
        if (want.parent_ino == pointer.parent_ino &&
            want.name == pointer.name &&
            want.parent_gen != pointer.parent_gen) {
          wrong_gen = true;
        }
        if (want.parent_ino == pointer.parent_ino &&
            want.parent_gen == pointer.parent_gen &&
            want.name != pointer.name) {
          wrong_name = true;
        }
      }
      if (wrong_gen) {
        report.wrong_parent_generation.push_back(
            "inode " + std::to_string(ino) + " -> " + PointerText(pointer));
      } else if (wrong_name) {
        report.wrong_name.push_back("inode " + std::to_string(ino) + " -> " +
                                    PointerText(pointer));
      } else {
        report.unexpected.push_back("inode " + std::to_string(ino) + " -> " +
                                    PointerText(pointer));
      }
    }
    for (size_t i = 0; i < wanted.size(); ++i) {
      if (!matched[i]) {
        report.missing.push_back("inode " + std::to_string(ino) +
                                 " expected " + PointerText(wanted[i]));
      }
    }
    if (!inode.value().IsDir() &&
        inode.value().nlink != actual.value().size()) {
      report.link_count_mismatch.push_back(
          "inode " + std::to_string(ino) + " nlink " +
          std::to_string(inode.value().nlink) + " != " +
          std::to_string(actual.value().size()) + " parent pointers");
    }
    if (wanted.size() != actual.value().size() &&
        inode.value().nlink != 0) {
      // A count mismatch on a directory means a missing/extra pointer even
      // when every present record happens to match one edge.
      if (inode.value().IsDir() && actual.value().size() != 1) {
        report.link_count_mismatch.push_back(
            "linked directory " + std::to_string(ino) + " has " +
            std::to_string(actual.value().size()) + " parent pointers");
      }
    }
  }

  // Root: no parent pointers, and its '..' was already checked above.
  {
    auto root_inode = OpenInode(geometry_.rootino);
    if (root_inode.ok()) {
      auto actual = ListParents(*device_, geometry_, root_inode.value());
      if (!actual.ok()) {
        report.malformed.push_back("root inode parent attrs: " +
                                   actual.error().message);
      } else {
        report.parent_pointers += actual.value().size();
        for (const ParentPointer& pointer : actual.value()) {
          report.unexpected.push_back("root inode -> " + PointerText(pointer));
        }
      }
    }
  }

  // Every reachable directory must have its one (non-root) parent pointer.
  for (const auto& [ino, state] : dirs) {
    if (ino == geometry_.rootino || !state.seen) continue;
    if (expected.find(ino) != expected.end()) continue;
    auto inode = OpenInode(ino);
    if (!inode.ok()) continue;
    auto actual = ListParents(*device_, geometry_, inode.value());
    if (!actual.ok()) {
      report.malformed.push_back("directory " + std::to_string(ino) +
                                 " parent attrs: " + actual.error().message);
      continue;
    }
    report.parent_pointers += actual.value().size();
    if (inode.value().nlink != 0) {
      ParentPointer want{state.parent, state.parent_gen, state.name};
      bool found = false;
      for (const ParentPointer& p : actual.value()) {
        if (p == want) found = true;
      }
      if (!found) {
        report.missing.push_back("directory " + std::to_string(ino) +
                                 " expected " + PointerText(want));
      }
      if (actual.value().size() != 1) {
        report.link_count_mismatch.push_back(
            "linked directory " + std::to_string(ino) + " has " +
            std::to_string(actual.value().size()) + " parent pointers");
      }
    } else if (!actual.value().empty()) {
      report.unexpected.push_back(
          "unlinked directory " + std::to_string(ino) + " has parent pointers");
    }
  }

  // Reverse records on an inode that is no longer reachable from the root
  // tree (deferred free, torn namespace transaction) must not be ignored: the
  // kernel's repair scan builds its master index from the inobt, not only from
  // a forward walk.  `checked` above is the reachable set.
  auto allocated = ListAllocatedInodes();
  if (!allocated.ok()) {
    report.malformed.push_back("cannot enumerate allocated inodes: " +
                               allocated.error().message);
  } else {
    for (uint64_t ino : allocated.value()) {
      if (ino == geometry_.rootino) continue;
      if (expected.find(ino) != expected.end()) continue;
      if (dirs.find(ino) != dirs.end()) continue;
      auto inode = OpenInode(ino);
      if (!inode.ok()) continue;
      auto actual = ListParents(*device_, geometry_, inode.value());
      if (!actual.ok()) {
        report.malformed.push_back("unreachable inode " + std::to_string(ino) +
                                   " parent attrs: " +
                                   actual.error().message);
        continue;
      }
      if (!actual.value().empty()) {
        report.parent_pointers += actual.value().size();
        report.unreachable_with_parents.push_back(
            "inode " + std::to_string(ino) + " has " +
            std::to_string(actual.value().size()) +
            " parent pointer(s) but is unreachable from the root tree");
      }
    }
  }

  return report;
}

std::string ParentNamespaceReport::Summary() const {
  auto count = [](const std::vector<std::string>& v) {
    return std::to_string(v.size());
  };
  return "edges=" + std::to_string(directory_edges) +
         " pointers=" + std::to_string(parent_pointers) +
         " inodes=" + std::to_string(checked_inodes) +
         " missing=" + count(missing) +
         " unexpected=" + count(unexpected) +
         " wrong_gen=" + count(wrong_parent_generation) +
         " wrong_name=" + count(wrong_name) +
         " duplicate=" + count(duplicate) +
         " dangling=" + count(dangling_parent) +
         " non_dir_parent=" + count(non_directory_parent) +
         " dotdot=" + count(wrong_dotdot) +
         " nlink=" + count(link_count_mismatch) +
         " malformed=" + count(malformed) +
         " orphan_pptr=" + count(unreachable_with_parents);
}

}  // namespace xfs
