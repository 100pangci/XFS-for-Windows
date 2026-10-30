// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "stateful/verifier.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>

#include "xfs/error.h"

namespace xfstest::stateful {
namespace {

VerifyResult Ok() { return VerifyResult{true, {}}; }
VerifyResult Fail(const std::string& error) {
  return VerifyResult{false, error};
}

uint16_t FileTypeBits(uint16_t mode) { return mode & 0170000; }

bool TypeMatches(NodeType expected, uint16_t mode) {
  const uint16_t bits = FileTypeBits(mode);
  switch (expected) {
    case NodeType::kFile:
      return bits == 0100000;
    case NodeType::kDirectory:
      return bits == 0040000;
    case NodeType::kSymlink:
      return bits == 0120000;
  }
  return false;
}

std::string Join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return a + ": " + b;
}

bool LookupEntry(xfs::XfsFilesystem& fs, const std::string& path,
                 uint64_t* ino, std::string* error) {
  if (path == "/") {
    *ino = fs.RootInode();
    return true;
  }
  auto found = fs.LookupPath(path);
  if (!found.ok()) {
    *error = "lookup " + path + ": " + found.error().Format();
    return false;
  }
  *ino = found.value().ino;
  return true;
}

bool CompareXattrs(xfs::XfsFilesystem& fs, uint64_t ino, const ModelNode& node,
                   std::string* error) {
  auto listed = fs.ListXattrs(ino, false);
  if (!listed.ok()) {
    *error = "listxattr inode " + std::to_string(ino) + ": " +
             listed.error().Format();
    return false;
  }
  std::map<std::string, std::vector<uint8_t>> actual;
  for (const xfs::XattrEntry& entry : listed.value()) {
    actual[entry.DisplayName(false)] = entry.value;
  }
  if (actual.size() != node.xattrs.size()) {
    *error = "inode " + std::to_string(ino) + " has " +
             std::to_string(actual.size()) + " attributes, expected " +
             std::to_string(node.xattrs.size());
    return false;
  }
  for (const auto& [name, value] : node.xattrs) {
    auto it = actual.find(name);
    if (it == actual.end()) {
      *error = "inode " + std::to_string(ino) + " missing xattr " + name;
      return false;
    }
    if (it->second != value) {
      *error = "inode " + std::to_string(ino) + " xattr " + name +
               " value differs (" + std::to_string(it->second.size()) +
               " vs " + std::to_string(value.size()) + " bytes)";
      return false;
    }
  }
  return true;
}

bool CompareDirectory(xfs::XfsFilesystem& fs, uint64_t ino, const ModelNode& node,
                      std::string* error) {
  auto listed = fs.ListDirectory(ino);
  if (!listed.ok()) {
    *error = "list directory inode " + std::to_string(ino) + ": " +
             listed.error().Format();
    return false;
  }
  std::vector<std::string> actual;
  for (const xfs::XfsDirEntry& entry : listed.value()) {
    if (entry.name == "." || entry.name == "..") continue;
    actual.push_back(entry.name);
  }
  std::vector<std::string> expected;
  for (const auto& [name, child] : node.entries) {
    (void)child;
    expected.push_back(name);
  }
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  if (actual != expected) {
    std::ostringstream out;
    out << "directory inode " << ino << " listing differs (actual";
    for (const std::string& name : actual) out << " " << name;
    out << ", expected";
    for (const std::string& name : expected) out << " " << name;
    out << ")";
    *error = out.str();
    return false;
  }
  return true;
}

bool CompareContent(xfs::XfsFilesystem& fs, uint64_t ino, uint64_t expected_size,
                    const ModelNode& node, std::string* error) {
  std::vector<uint8_t> actual;
  if (!ReadAll(fs, ino, expected_size, &actual, error)) return false;
  if (actual.size() != node.data.size()) {
    *error = "inode " + std::to_string(ino) + " size " +
             std::to_string(actual.size()) + " != model " +
             std::to_string(node.data.size());
    return false;
  }
  if (ModelFs::HashBytes(actual) != ModelFs::HashBytes(node.data)) {
    *error = "inode " + std::to_string(ino) + " content hash differs";
    return false;
  }
  return true;
}

}  // namespace

bool ReadAll(xfs::XfsFilesystem& fs, uint64_t ino, uint64_t size,
             std::vector<uint8_t>* out, std::string* error) {
  out->clear();
  out->reserve(static_cast<size_t>(size));
  constexpr size_t kChunk = 256 * 1024;
  uint64_t offset = 0;
  while (offset < size) {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(kChunk, size - offset));
    std::vector<uint8_t> buffer(want);
    auto read = fs.ReadFile(ino, offset, buffer.data(), want);
    if (!read.ok()) {
      if (error) {
        *error = "read inode " + std::to_string(ino) + " at " +
                 std::to_string(offset) + ": " + read.error().Format();
      }
      return false;
    }
    if (read.value() == 0) {
      if (error) {
        uint64_t inode_size = 0;
        auto opened = fs.OpenInode(ino);
        if (opened.ok()) inode_size = opened.value().size;
        *error = "short read at offset " + std::to_string(offset) +
                 " of " + std::to_string(size) + " for inode " +
                 std::to_string(ino) + " (OpenInode size " +
                 std::to_string(inode_size) + ")";
      }
      return false;
    }
    out->insert(out->end(), buffer.begin(), buffer.begin() + read.value());
    offset += read.value();
  }
  return true;
}

bool IsNoSpace(const xfs::ErrorContext& error) {
  return error.code == xfs::XfsError::kNoSpace;
}

std::string DescribeError(const xfs::ErrorContext& error) {
  return error.Format();
}

VerifyResult VerifyModel(xfs::XfsFilesystem& fs, const ModelFs& model,
                         uint64_t skip_node) {
  for (const auto& [id, node] : model.nodes()) {
    const std::vector<std::string> paths = model.PathsOf(id);
    if (paths.empty()) {
      return Fail("model inode " + std::to_string(id) + " is unreachable");
    }
    uint64_t first_ino = 0;
    for (const std::string& path : paths) {
      std::string error;
      uint64_t ino = 0;
      if (!LookupEntry(fs, path, &ino, &error)) return Fail(error);
      if (first_ino == 0) {
        first_ino = ino;
      } else if (ino != first_ino) {
        return Fail("hard-link paths of model inode " + std::to_string(id) +
                    " resolve to different inodes (" +
                    std::to_string(first_ino) + " vs " + std::to_string(ino) +
                    ")");
      }
      auto info = fs.GetFileInfo(ino);
      if (!info.ok()) {
        return Fail("stat " + path + ": " + info.error().Format());
      }
      if (!TypeMatches(node.type, info.value().mode)) {
        return Fail("type mismatch for " + path + " (mode " +
                    std::to_string(info.value().mode) + ")");
      }
      const uint32_t expected_nlink = model.ExpectedNlink(id);
      if (info.value().nlink != expected_nlink) {
        return Fail("nlink mismatch for " + path + " (actual " +
                    std::to_string(info.value().nlink) + ", expected " +
                    std::to_string(expected_nlink) + ")");
      }
      if (node.type == NodeType::kFile && id != skip_node) {
        if (info.value().size != node.data.size()) {
          return Fail("size mismatch for " + path + " (actual " +
                      std::to_string(info.value().size) + ", expected " +
                      std::to_string(node.data.size()) + ")");
        }
        if (!CompareContent(fs, ino, info.value().size, node, &error)) {
          return Fail(Join(path, error));
        }
      }
      if (node.type == NodeType::kSymlink) {
        auto target = fs.ReadSymlink(ino);
        if (!target.ok()) {
          return Fail("readlink " + path + ": " + target.error().Format());
        }
        if (target.value() != node.target) {
          return Fail("symlink target mismatch for " + path);
        }
        if (info.value().size != node.target.size()) {
          return Fail("symlink size mismatch for " + path);
        }
      }
      if (node.type == NodeType::kDirectory) {
        if (!CompareDirectory(fs, ino, node, &error)) {
          return Fail(Join(path, error));
        }
      }
      if (!CompareXattrs(fs, ino, node, &error)) {
        return Fail(Join(path, error));
      }
    }
  }
  return Ok();
}

VerifyResult VerifyRangeCrash(xfs::XfsFilesystem& fs, const ModelFs& before,
                              const ModelFs& after, const Operation& op) {
  const uint64_t target_id =
      op.type == OpType::kReflink ? op.dst_node : op.node;
  VerifyResult skeleton = VerifyModel(fs, after, target_id);
  if (!skeleton.ok) return skeleton;

  const ModelNode* before_node = before.Find(target_id);
  const ModelNode* after_node = after.Find(target_id);
  if (!before_node || !after_node ||
      before_node->type != NodeType::kFile ||
      after_node->type != NodeType::kFile) {
    // A data operation always targets an existing regular file; anything else
    // is a model bug, not a legal crash state.
    return Fail("crash target " + std::to_string(target_id) +
                " is not a file in both model states");
  }

  const std::string path = after.PathOf(target_id);
  uint64_t ino = 0;
  std::string error;
  if (!LookupEntry(fs, path, &ino, &error)) return Fail(error);
  auto info = fs.GetFileInfo(ino);
  if (!info.ok()) return Fail("stat " + path + ": " + info.error().Format());
  std::vector<uint8_t> actual;
  if (!ReadAll(fs, ino, info.value().size, &actual, &error)) {
    return Fail(Join(path, error));
  }

  const uint64_t before_size = before_node->data.size();
  const uint64_t after_size = after_node->data.size();
  const uint64_t actual_size = actual.size();
  if (actual_size != before_size && actual_size != after_size) {
    return Fail("crash size for " + path + " is " +
                std::to_string(actual_size) + ", neither old " +
                std::to_string(before_size) + " nor new " +
                std::to_string(after_size));
  }

  auto before_byte = [&](uint64_t pos, uint8_t* value) {
    if (pos >= before_node->data.size()) return false;
    *value = before_node->data[static_cast<size_t>(pos)];
    return true;
  };
  auto after_byte = [&](uint64_t pos, uint8_t* value) {
    if (pos >= after_node->data.size()) return false;
    *value = after_node->data[static_cast<size_t>(pos)];
    return true;
  };

  for (uint64_t pos = 0; pos < actual_size; ++pos) {
    const uint8_t got = actual[static_cast<size_t>(pos)];
    uint8_t old_value = 0;
    uint8_t new_value = 0;
    const bool has_old = before_byte(pos, &old_value);
    const bool has_new = after_byte(pos, &new_value);
    bool allowed = false;
    switch (op.type) {
      case OpType::kTruncateSmaller:
        if (pos < after_size) {
          allowed = has_old && got == old_value;
        } else {
          allowed = got == 0 || (has_old && got == old_value);
        }
        break;
      case OpType::kTruncateLarger:
      case OpType::kPreallocate:
        if (pos < before_size) {
          allowed = has_old && got == old_value;
        } else {
          allowed = got == 0;
        }
        break;
      case OpType::kReflink: {
        const uint64_t start = op.size;
        const uint64_t end = start + op.length;
        if (pos >= start && pos < end) {
          allowed = (has_old && got == old_value) ||
                    (has_new && got == new_value);
        } else if (has_old) {
          allowed = got == old_value;
        } else {
          allowed = got == 0;
        }
        break;
      }
      case OpType::kConvertUnwritten:
        allowed = has_new && got == new_value;
        break;
      case OpType::kWrite:
      case OpType::kExtend:
      case OpType::kOverwrite:
      case OpType::kSparseWrite:
      case OpType::kCowOverwrite:
        if (pos < before_size) {
          allowed = (has_old && got == old_value) ||
                    (has_new && got == new_value);
        } else {
          allowed = got == 0 || (has_new && got == new_value);
        }
        break;
      default:
        return Fail("unhandled crash data operation " + std::string(ToString(op.type)));
    }
    if (!allowed) {
      return Fail("crash byte " + std::to_string(pos) + " in " + path +
                  " is outside the old/new contract (value " +
                  std::to_string(static_cast<unsigned>(got)) + ")");
    }
  }
  return Ok();
}

}  // namespace xfstest::stateful
