// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "stateful/model.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace xfstest::stateful {
namespace {

constexpr uint64_t kMaxFileBytes = 16ull << 20;

void SetError(std::string* error, const std::string& message) {
  if (error) *error = message;
}

uint64_t Fnv1a(const uint8_t* data, size_t size) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex64(uint64_t value) {
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

std::string HexBytes(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char c : value) {
    out += kHex[c >> 4];
    out += kHex[c & 0xf];
  }
  return out;
}

bool UnhexBytes(const std::string& text, std::string* out) {
  if (text.size() % 2 != 0) return false;
  out->clear();
  out->reserve(text.size() / 2);
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < text.size(); i += 2) {
    const int hi = nibble(text[i]);
    const int lo = nibble(text[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out->push_back(static_cast<char>((hi << 4) | lo));
  }
  return true;
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::string current;
  for (char c : line) {
    if (c == '\t') {
      fields.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  fields.push_back(current);
  return fields;
}

bool ParseFieldU64(const std::string& text, uint64_t* out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 0);
  if (end == text.c_str() || *end != '\0') return false;
  *out = static_cast<uint64_t>(value);
  return true;
}

}  // namespace

const char* ToString(NodeType type) {
  switch (type) {
    case NodeType::kFile:
      return "file";
    case NodeType::kDirectory:
      return "dir";
    case NodeType::kSymlink:
      return "symlink";
  }
  return "?";
}

const char* ToString(OpType type) {
  switch (type) {
    case OpType::kMkdir:
      return "mkdir";
    case OpType::kCreate:
      return "create";
    case OpType::kWrite:
      return "write";
    case OpType::kExtend:
      return "extend";
    case OpType::kOverwrite:
      return "overwrite";
    case OpType::kSparseWrite:
      return "sparse_write";
    case OpType::kTruncateSmaller:
      return "truncate_smaller";
    case OpType::kTruncateLarger:
      return "truncate_larger";
    case OpType::kRename:
      return "rename";
    case OpType::kHardlink:
      return "hardlink";
    case OpType::kUnlink:
      return "unlink";
    case OpType::kRmdir:
      return "rmdir";
    case OpType::kSymlink:
      return "symlink";
    case OpType::kSetXattr:
      return "setxattr";
    case OpType::kReplaceXattr:
      return "replacexattr";
    case OpType::kRemoveXattr:
      return "removexattr";
    case OpType::kReflink:
      return "reflink";
    case OpType::kCowOverwrite:
      return "cow_overwrite";
    case OpType::kPreallocate:
      return "preallocate";
    case OpType::kConvertUnwritten:
      return "convert_unwritten";
  }
  return "?";
}

bool ParseOpType(const std::string& name, OpType* out) {
  for (int i = 0; i <= static_cast<int>(OpType::kConvertUnwritten); ++i) {
    const OpType type = static_cast<OpType>(i);
    if (name == ToString(type)) {
      *out = type;
      return true;
    }
  }
  return false;
}

bool IsDataOperation(OpType type) {
  switch (type) {
    case OpType::kWrite:
    case OpType::kExtend:
    case OpType::kOverwrite:
    case OpType::kSparseWrite:
    case OpType::kTruncateSmaller:
    case OpType::kTruncateLarger:
    case OpType::kReflink:
    case OpType::kCowOverwrite:
    case OpType::kPreallocate:
    case OpType::kConvertUnwritten:
      return true;
    default:
      return false;
  }
}

bool IsRangeCrashOperation(OpType type) {
  return IsDataOperation(type) && type != OpType::kConvertUnwritten;
}

uint8_t PatternByte(uint32_t seed, uint64_t offset) {
  uint64_t x = (static_cast<uint64_t>(seed) << 32) ^ offset;
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31;
  return static_cast<uint8_t>(x >> 24);
}

std::vector<uint8_t> PatternBytes(uint32_t seed, uint64_t length) {
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  for (uint64_t i = 0; i < length; ++i) {
    bytes[static_cast<size_t>(i)] = PatternByte(seed, i);
  }
  return bytes;
}

std::string Operation::Describe() const {
  std::ostringstream out;
  out << ToString(type);
  switch (type) {
    case OpType::kMkdir:
    case OpType::kCreate:
    case OpType::kSymlink:
      out << " parent=" << parent << " name=" << name << " node=" << node;
      if (type == OpType::kSymlink) {
        out << " target_len=" << symlink_target.size();
      }
      break;
    case OpType::kWrite:
    case OpType::kExtend:
    case OpType::kOverwrite:
    case OpType::kSparseWrite:
    case OpType::kCowOverwrite:
      out << " node=" << node << " offset=" << offset
          << " length=" << length << " data_seed=0x" << std::hex << data_seed
          << std::dec;
      break;
    case OpType::kTruncateSmaller:
    case OpType::kTruncateLarger:
      out << " node=" << node << " size=" << size;
      break;
    case OpType::kRename:
      out << " parent=" << parent << " name=" << name
          << " dst_parent=" << dst_parent << " dst_name=" << dst_name
          << " replace=" << (replace_existing ? 1 : 0);
      break;
    case OpType::kHardlink:
      out << " node=" << node << " dst_parent=" << dst_parent
          << " dst_name=" << dst_name;
      break;
    case OpType::kUnlink:
    case OpType::kRmdir:
      out << " parent=" << parent << " name=" << name;
      break;
    case OpType::kSetXattr:
    case OpType::kReplaceXattr:
      out << " node=" << node << " name=" << xattr_name
          << " length=" << xattr_length << " value_seed=0x" << std::hex
          << xattr_seed << std::dec;
      break;
    case OpType::kRemoveXattr:
      out << " node=" << node << " name=" << xattr_name;
      break;
    case OpType::kReflink:
      out << " node=" << node << " src_offset=" << offset
          << " dst_node=" << dst_node << " dst_offset=" << size
          << " length=" << length;
      break;
    case OpType::kPreallocate:
      out << " node=" << node << " offset=" << offset
          << " length=" << length << " keep_size=" << (keep_size ? 1 : 0);
      break;
    case OpType::kConvertUnwritten:
      out << " node=" << node << " offset=" << offset
          << " length=" << length;
      break;
  }
  return out.str();
}

ModelFs::ModelFs() {
  root_ = 0;
  next_id_ = 1;
  ModelNode root;
  root.id = 0;
  root.type = NodeType::kDirectory;
  nodes_[0] = root;
}

const ModelNode* ModelFs::Find(uint64_t id) const {
  auto it = nodes_.find(id);
  return it == nodes_.end() ? nullptr : &it->second;
}

ModelNode* ModelFs::FindMutable(uint64_t id) {
  auto it = nodes_.find(id);
  return it == nodes_.end() ? nullptr : &it->second;
}

bool ModelFs::InsertEntry(uint64_t parent, const std::string& name,
                          uint64_t child, std::string* error) {
  ModelNode* dir = FindMutable(parent);
  if (!dir || dir->type != NodeType::kDirectory) {
    SetError(error, "parent is not a directory");
    return false;
  }
  if (name.empty() || name == "." || name == ".." || name.size() > 255) {
    SetError(error, "invalid name");
    return false;
  }
  if (dir->entries.count(name) != 0) {
    SetError(error, "name already exists");
    return false;
  }
  dir->entries[name] = child;
  return true;
}

void ModelFs::RemoveEntry(uint64_t parent, const std::string& name) {
  ModelNode* dir = FindMutable(parent);
  if (dir) dir->entries.erase(name);
}

bool ModelFs::Apply(const Operation& op, std::string* error) {
  if (IsDataOperation(op.type)) {
    ModelNode* node = FindMutable(op.node);
    if (!node || node->type != NodeType::kFile) {
      SetError(error, "data operation target is not a regular file");
      return false;
    }
    switch (op.type) {
      case OpType::kWrite:
      case OpType::kExtend:
      case OpType::kOverwrite:
      case OpType::kSparseWrite:
      case OpType::kCowOverwrite: {
        if (op.length == 0) {
          SetError(error, "zero-length write");
          return false;
        }
        if (op.offset > kMaxFileBytes || op.length > kMaxFileBytes - op.offset) {
          SetError(error, "write exceeds the model file size cap");
          return false;
        }
        const uint64_t end = op.offset + op.length;
        if (node->data.size() < end) node->data.resize(static_cast<size_t>(end), 0);
        const std::vector<uint8_t> bytes =
            PatternBytes(op.data_seed, op.length);
        std::copy(bytes.begin(), bytes.end(), node->data.begin() + op.offset);
        if (op.type == OpType::kCowOverwrite) node->shared_hint = false;
        break;
      }
      case OpType::kTruncateSmaller:
        if (op.size >= node->data.size()) {
          SetError(error, "truncate_smaller does not shrink");
          return false;
        }
        node->data.resize(static_cast<size_t>(op.size));
        break;
      case OpType::kTruncateLarger:
        if (op.size <= node->data.size()) {
          SetError(error, "truncate_larger does not grow");
          return false;
        }
        if (op.size > kMaxFileBytes) {
          SetError(error, "truncate exceeds the model file size cap");
          return false;
        }
        node->data.resize(static_cast<size_t>(op.size), 0);
        break;
      case OpType::kPreallocate: {
        const uint64_t end = op.offset + op.length;
        const uint64_t new_size = op.keep_size
                                      ? node->data.size()
                                      : std::max<uint64_t>(node->data.size(), end);
        if (new_size > kMaxFileBytes) {
          SetError(error, "preallocate exceeds the model file size cap");
          return false;
        }
        node->data.resize(static_cast<size_t>(new_size), 0);
        break;
      }
      case OpType::kConvertUnwritten:
        break;
      case OpType::kReflink: {
        ModelNode* dst = FindMutable(op.dst_node);
        if (!dst || dst->type != NodeType::kFile) {
          SetError(error, "reflink destination is not a file");
          return false;
        }
        if (dst->id == node->id) {
          SetError(error, "reflink source and destination are the same file");
          return false;
        }
        uint64_t length = op.length;
        if (length == 0) {
          if (op.offset > node->data.size()) {
            SetError(error, "reflink source offset is past EOF");
            return false;
          }
          length = node->data.size() - op.offset;
        }
        if (op.offset > node->data.size() ||
            length > node->data.size() - op.offset) {
          SetError(error, "reflink source range is past EOF");
          return false;
        }
        const uint64_t dst_end = op.size + length;
        if (dst_end > kMaxFileBytes) {
          SetError(error, "reflink exceeds the model file size cap");
          return false;
        }
        if (dst->data.size() < dst_end) {
          dst->data.resize(static_cast<size_t>(dst_end), 0);
        }
        std::copy(node->data.begin() + op.offset,
                  node->data.begin() + op.offset + length,
                  dst->data.begin() + op.size);
        dst->shared_hint = true;
        dst->cloned_ranges.emplace_back(op.size, length);
        break;
      }
      default:
        SetError(error, "unhandled data operation");
        return false;
    }
    return true;
  }

  switch (op.type) {
    case OpType::kMkdir:
    case OpType::kCreate:
    case OpType::kSymlink: {
      if (nodes_.count(op.node) != 0) {
        SetError(error, "new node id already exists");
        return false;
      }
      ModelNode node;
      node.id = op.node;
      node.type = op.type == OpType::kMkdir
                      ? NodeType::kDirectory
                      : (op.type == OpType::kSymlink ? NodeType::kSymlink
                                                     : NodeType::kFile);
      node.target = op.symlink_target;
      if (!InsertEntry(op.parent, op.name, op.node, error)) return false;
      nodes_[op.node] = std::move(node);
      next_id_ = std::max(next_id_, op.node + 1);
      return true;
    }
    case OpType::kRename: {
      ModelNode* src_dir = FindMutable(op.parent);
      if (!src_dir || src_dir->type != NodeType::kDirectory) {
        SetError(error, "rename source parent is not a directory");
        return false;
      }
      auto src_it = src_dir->entries.find(op.name);
      if (src_it == src_dir->entries.end()) {
        SetError(error, "rename source does not exist");
        return false;
      }
      ModelNode* dst_dir = FindMutable(op.dst_parent);
      if (!dst_dir || dst_dir->type != NodeType::kDirectory) {
        SetError(error, "rename destination parent is not a directory");
        return false;
      }
      if (op.parent == op.dst_parent && op.name == op.dst_name) {
        SetError(error, "rename source and destination are the same");
        return false;
      }
      const uint64_t moved = src_it->second;
      ModelNode* moved_node = FindMutable(moved);
      if (!moved_node) {
        SetError(error, "rename source inode is missing");
        return false;
      }
      if (moved_node->type == NodeType::kDirectory &&
          (IsAncestor(moved, op.dst_parent) || moved == op.dst_parent)) {
        SetError(error, "rename would move a directory into its own subtree");
        return false;
      }
      auto dst_it = dst_dir->entries.find(op.dst_name);
      if (dst_it != dst_dir->entries.end()) {
        if (!op.replace_existing) {
          SetError(error, "rename destination exists");
          return false;
        }
        const uint64_t replaced = dst_it->second;
        if (replaced == moved) {
          SetError(error, "rename destination is another name of the source");
          return false;
        }
        ModelNode* replaced_node = FindMutable(replaced);
        if (!replaced_node) {
          SetError(error, "rename destination inode is missing");
          return false;
        }
        if (moved_node->type == NodeType::kDirectory) {
          if (replaced_node->type != NodeType::kDirectory ||
              !IsEmptyDirectory(replaced)) {
            SetError(error, "directory rename target is not an empty directory");
            return false;
          }
        } else if (replaced_node->type == NodeType::kDirectory) {
          SetError(error, "file rename target is a directory");
          return false;
        }
        dst_dir->entries.erase(dst_it);
        if (NameCount(replaced) == 0) nodes_.erase(replaced);
      }
      src_dir->entries.erase(src_it);
      dst_dir->entries[op.dst_name] = moved;
      return true;
    }
    case OpType::kHardlink: {
      ModelNode* node = FindMutable(op.node);
      if (!node || node->type == NodeType::kDirectory) {
        SetError(error, "hard link target is not a file or symlink");
        return false;
      }
      return InsertEntry(op.dst_parent, op.dst_name, op.node, error);
    }
    case OpType::kUnlink: {
      ModelNode* dir = FindMutable(op.parent);
      if (!dir || dir->type != NodeType::kDirectory) {
        SetError(error, "unlink parent is not a directory");
        return false;
      }
      auto it = dir->entries.find(op.name);
      if (it == dir->entries.end()) {
        SetError(error, "unlink target does not exist");
        return false;
      }
      ModelNode* node = FindMutable(it->second);
      if (!node || node->type == NodeType::kDirectory) {
        SetError(error, "unlink target is a directory");
        return false;
      }
      const uint64_t id = it->second;
      dir->entries.erase(it);
      if (NameCount(id) == 0) nodes_.erase(id);
      return true;
    }
    case OpType::kRmdir: {
      ModelNode* dir = FindMutable(op.parent);
      if (!dir || dir->type != NodeType::kDirectory) {
        SetError(error, "rmdir parent is not a directory");
        return false;
      }
      auto it = dir->entries.find(op.name);
      if (it == dir->entries.end()) {
        SetError(error, "rmdir target does not exist");
        return false;
      }
      ModelNode* child = FindMutable(it->second);
      if (!child || child->type != NodeType::kDirectory ||
          !child->entries.empty() || child->id == root_) {
        SetError(error, "rmdir target is not an empty non-root directory");
        return false;
      }
      dir->entries.erase(it);
      nodes_.erase(child->id);
      return true;
    }
    case OpType::kSetXattr:
    case OpType::kReplaceXattr: {
      ModelNode* node = FindMutable(op.node);
      if (!node) {
        SetError(error, "xattr target is missing");
        return false;
      }
      auto it = node->xattrs.find(op.xattr_name);
      if (op.type == OpType::kReplaceXattr && it == node->xattrs.end()) {
        SetError(error, "replacexattr target is missing");
        return false;
      }
      if (op.type == OpType::kSetXattr && it != node->xattrs.end()) {
        SetError(error, "setxattr target already exists");
        return false;
      }
      node->xattrs[op.xattr_name] = PatternBytes(op.xattr_seed, op.xattr_length);
      return true;
    }
    case OpType::kRemoveXattr: {
      ModelNode* node = FindMutable(op.node);
      if (!node) {
        SetError(error, "xattr target is missing");
        return false;
      }
      auto it = node->xattrs.find(op.xattr_name);
      if (it == node->xattrs.end()) {
        SetError(error, "removexattr target is missing");
        return false;
      }
      node->xattrs.erase(it);
      return true;
    }
    default:
      SetError(error, "unhandled operation");
      return false;
  }
}

std::string ModelFs::PathOf(uint64_t id) const {
  std::vector<std::string> paths = PathsOf(id);
  return paths.empty() ? std::string() : paths.front();
}

std::vector<std::string> ModelFs::PathsOf(uint64_t id) const {
  std::vector<std::string> result;
  std::vector<std::pair<uint64_t, std::string>> stack;
  stack.emplace_back(root_, "/");
  while (!stack.empty()) {
    auto [node_id, path] = stack.back();
    stack.pop_back();
    if (node_id == id) result.push_back(path);
    const ModelNode* node = Find(node_id);
    if (!node || node->type != NodeType::kDirectory) continue;
    for (auto it = node->entries.rbegin(); it != node->entries.rend(); ++it) {
      const std::string child_path =
          path == "/" ? "/" + it->first : path + "/" + it->first;
      stack.emplace_back(it->second, child_path);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<uint64_t> ModelFs::Directories() const {
  std::vector<uint64_t> out;
  for (const auto& [id, node] : nodes_) {
    if (node.type == NodeType::kDirectory) out.push_back(id);
  }
  return out;
}

std::vector<uint64_t> ModelFs::Files() const {
  std::vector<uint64_t> out;
  for (const auto& [id, node] : nodes_) {
    if (node.type == NodeType::kFile) out.push_back(id);
  }
  return out;
}

std::vector<uint64_t> ModelFs::Symlinks() const {
  std::vector<uint64_t> out;
  for (const auto& [id, node] : nodes_) {
    if (node.type == NodeType::kSymlink) out.push_back(id);
  }
  return out;
}

void ModelFs::CollectEntries(uint64_t dir, std::vector<EntryRef>* out) const {
  const ModelNode* node = Find(dir);
  if (!node || node->type != NodeType::kDirectory) return;
  for (const auto& [name, child] : node->entries) {
    out->push_back({dir, name, child});
    CollectEntries(child, out);
  }
}

std::vector<EntryRef> ModelFs::Entries() const {
  std::vector<EntryRef> out;
  CollectEntries(root_, &out);
  return out;
}

std::vector<EntryRef> ModelFs::EmptyDirectories() const {
  std::vector<EntryRef> out;
  for (const EntryRef& entry : Entries()) {
    if (entry.node == root_) continue;
    if (IsEmptyDirectory(entry.node)) out.push_back(entry);
  }
  return out;
}

uint32_t ModelFs::NameCount(uint64_t id) const {
  uint32_t count = 0;
  for (const auto& [parent, node] : nodes_) {
    if (node.type != NodeType::kDirectory) continue;
    for (const auto& [name, child] : node.entries) {
      (void)parent;
      (void)name;
      if (child == id) ++count;
    }
  }
  return count;
}

uint32_t ModelFs::ExpectedNlink(uint64_t id) const {
  const ModelNode* node = Find(id);
  if (!node) return 0;
  if (node->type != NodeType::kDirectory) return NameCount(id);
  uint32_t subdirs = 0;
  for (const auto& [name, child] : node->entries) {
    (void)name;
    const ModelNode* c = Find(child);
    if (c && c->type == NodeType::kDirectory) ++subdirs;
  }
  return 2 + subdirs;
}

bool ModelFs::IsAncestor(uint64_t ancestor, uint64_t node) const {
  std::vector<uint64_t> stack{node};
  std::vector<uint64_t> seen;
  while (!stack.empty()) {
    uint64_t current = stack.back();
    stack.pop_back();
    if (current == ancestor) return true;
    if (std::find(seen.begin(), seen.end(), current) != seen.end()) continue;
    seen.push_back(current);
    for (const auto& [parent, n] : nodes_) {
      if (n.type != NodeType::kDirectory) continue;
      for (const auto& [name, child] : n.entries) {
        (void)name;
        if (child == current) stack.push_back(parent);
      }
    }
  }
  return false;
}

bool ModelFs::IsEmptyDirectory(uint64_t id) const {
  const ModelNode* node = Find(id);
  return node && node->type == NodeType::kDirectory && node->entries.empty();
}

uint64_t ModelFs::ContentHash(uint64_t id) const {
  const ModelNode* node = Find(id);
  if (!node) return 0;
  if (node->type == NodeType::kFile) return HashBytes(node->data);
  if (node->type == NodeType::kSymlink) {
    return HashBytes(std::vector<uint8_t>(node->target.begin(),
                                          node->target.end()));
  }
  return 0;
}

uint64_t ModelFs::HashBytes(const std::vector<uint8_t>& bytes) {
  return Fnv1a(bytes.data(), bytes.size());
}

std::string JsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (unsigned char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  return out;
}

std::string ModelFs::ToJson(const std::string& indent) const {
  std::ostringstream out;
  out << indent << "{\n";
  out << indent << "  \"entries\": [\n";
  bool first_entry = true;
  for (const auto& [id, node] : nodes_) {
    std::vector<std::string> paths = PathsOf(id);
    if (paths.empty()) continue;
    if (!first_entry) out << ",\n";
    first_entry = false;
    out << indent << "    {\n";
    out << indent << "      \"id\": " << id << ",\n";
    out << indent << "      \"type\": \"" << ToString(node.type) << "\",\n";
    out << indent << "      \"paths\": [";
    for (size_t i = 0; i < paths.size(); ++i) {
      if (i) out << ", ";
      out << "\"" << JsonEscape(paths[i]) << "\"";
    }
    out << "],\n";
    out << indent << "      \"nlink\": " << ExpectedNlink(id) << ",\n";
    out << indent << "      \"size\": " << node.Size() << ",\n";
    out << indent << "      \"content_fnv\": \"" << Hex64(ContentHash(id))
        << "\",\n";
    if (node.type == NodeType::kSymlink) {
      out << indent << "      \"target\": \"" << JsonEscape(node.target)
          << "\",\n";
    }
    out << indent << "      \"xattrs\": [";
    bool first_xattr = true;
    for (const auto& [name, value] : node.xattrs) {
      if (!first_xattr) out << ", ";
      first_xattr = false;
      out << "{\"name\": \"" << JsonEscape(name) << "\", \"length\": "
          << value.size() << ", \"value_fnv\": \""
          << Hex64(HashBytes(value)) << "\"}";
    }
    out << "]\n";
    out << indent << "    }";
  }
  out << "\n" << indent << "  ],\n";
  out << indent << "  \"listings\": {";
  bool first_listing = true;
  for (const auto& [id, node] : nodes_) {
    if (node.type != NodeType::kDirectory) continue;
    std::vector<std::string> paths = PathsOf(id);
    if (paths.empty()) continue;
    if (!first_listing) out << ", ";
    first_listing = false;
    out << "\"" << JsonEscape(paths.front()) << "\": [";
    bool first_name = true;
    for (const auto& [name, child] : node.entries) {
      (void)child;
      if (!first_name) out << ", ";
      first_name = false;
      out << "\"" << JsonEscape(name) << "\"";
    }
    out << "]";
  }
  out << "}\n";
  out << indent << "}";
  return out.str();
}

std::string OperationToLine(const Operation& op) {
  std::ostringstream out;
  out << ToString(op.type) << '\t' << op.node << '\t' << op.dst_node << '\t'
      << op.parent << '\t' << op.dst_parent << '\t' << HexBytes(op.name)
      << '\t' << HexBytes(op.dst_name) << '\t' << op.offset << '\t'
      << op.length << '\t' << op.size << '\t' << op.data_seed << '\t'
      << HexBytes(op.xattr_name) << '\t' << op.xattr_seed << '\t'
      << op.xattr_length << '\t' << (op.replace_existing ? 1 : 0) << '\t'
      << (op.keep_size ? 1 : 0) << '\t' << HexBytes(op.symlink_target);
  return out.str();
}

bool OperationFromLine(const std::string& line, Operation* op,
                       std::string* error) {
  const std::vector<std::string> fields = SplitTabs(line);
  if (fields.size() != 17) {
    if (error) *error = "operation line does not have 17 fields";
    return false;
  }
  Operation parsed;
  if (!ParseOpType(fields[0], &parsed.type)) {
    if (error) *error = "unknown operation type " + fields[0];
    return false;
  }
  uint64_t values[8] = {};
  const int numeric[] = {1, 2, 3, 4, 7, 8, 9, 10, 12, 13, 14, 15};
  for (int index : numeric) {
    if (!ParseFieldU64(fields[static_cast<size_t>(index)], &values[0])) {
      if (error) {
        *error = "invalid numeric field " + std::to_string(index);
      }
      return false;
    }
    switch (index) {
      case 1: parsed.node = values[0]; break;
      case 2: parsed.dst_node = values[0]; break;
      case 3: parsed.parent = values[0]; break;
      case 4: parsed.dst_parent = values[0]; break;
      case 7: parsed.offset = values[0]; break;
      case 8: parsed.length = values[0]; break;
      case 9: parsed.size = values[0]; break;
      case 10: parsed.data_seed = static_cast<uint32_t>(values[0]); break;
      case 12: parsed.xattr_seed = static_cast<uint32_t>(values[0]); break;
      case 13: parsed.xattr_length = static_cast<uint32_t>(values[0]); break;
      case 14: parsed.replace_existing = values[0] != 0; break;
      case 15: parsed.keep_size = values[0] != 0; break;
    }
  }
  if (!UnhexBytes(fields[5], &parsed.name) ||
      !UnhexBytes(fields[6], &parsed.dst_name) ||
      !UnhexBytes(fields[11], &parsed.xattr_name) ||
      !UnhexBytes(fields[16], &parsed.symlink_target)) {
    if (error) *error = "invalid hex string field";
    return false;
  }
  *op = std::move(parsed);
  return true;
}

}  // namespace xfstest::stateful
