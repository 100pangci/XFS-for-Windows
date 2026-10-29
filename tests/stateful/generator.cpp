// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "stateful/generator.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace xfstest::stateful {
namespace {

constexpr uint64_t kBlock = 4096;

std::string MakeName(char prefix, uint64_t counter) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%c%llu", prefix,
                static_cast<unsigned long long>(counter));
  return buffer;
}

}  // namespace

bool ParseWorkload(const std::string& name, Workload* out) {
  if (name == "mixed") {
    *out = Workload::kMixed;
  } else if (name == "dir" || name == "directory") {
    *out = Workload::kDirectory;
  } else if (name == "xattr") {
    *out = Workload::kXattr;
  } else if (name == "alloc") {
    *out = Workload::kAlloc;
  } else if (name == "enospc") {
    *out = Workload::kEnospc;
  } else {
    return false;
  }
  return true;
}

const char* ToString(Workload workload) {
  switch (workload) {
    case Workload::kMixed:
      return "mixed";
    case Workload::kDirectory:
      return "dir";
    case Workload::kXattr:
      return "xattr";
    case Workload::kAlloc:
      return "alloc";
    case Workload::kEnospc:
      return "enospc";
  }
  return "?";
}

uint64_t Rng::Next() {
  state_ += 0x9e3779b97f4a7c15ull;
  uint64_t z = state_;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

uint64_t Rng::Range(uint64_t low, uint64_t high) {
  if (high <= low) return low;
  return low + Next() % (high - low + 1);
}

bool Rng::Chance(uint32_t percent) {
  return (Next() % 100) < percent;
}

Generator::Generator(const ModelFs& model, const GeneratorOptions& options)
    : model_(model), options_(options), rng_(options.seed) {}

std::vector<uint64_t> Generator::CandidateFiles() const {
  return model_.Files();
}

std::vector<uint64_t> Generator::CandidateDirs() const {
  return model_.Directories();
}

std::vector<EntryRef> Generator::CandidateEntries(bool non_directory) const {
  std::vector<EntryRef> out;
  for (const EntryRef& entry : model_.Entries()) {
    const ModelNode* node = model_.Find(entry.node);
    if (!node) continue;
    if (non_directory && node->type == NodeType::kDirectory) continue;
    out.push_back(entry);
  }
  return out;
}

uint64_t Generator::Pick(const std::vector<uint64_t>& values,
                         uint64_t fallback) {
  if (values.empty()) return fallback;
  return values[static_cast<size_t>(rng_.Range(0, values.size() - 1))];
}

const EntryRef* Generator::PickEntry(const std::vector<EntryRef>& entries) {
  if (entries.empty()) return nullptr;
  return &entries[static_cast<size_t>(rng_.Range(0, entries.size() - 1))];
}

std::string Generator::FreshName(uint64_t parent, char kind) {
  const ModelNode* dir = model_.Find(parent);
  for (int attempt = 0; attempt < 100000; ++attempt) {
    std::string name = MakeName(kind, name_counter_++);
    if (!dir || dir->entries.count(name) == 0) return name;
  }
  return MakeName(kind, name_counter_++);
}

OpType Generator::ChooseType(uint64_t op_index) {
  const uint32_t roll = static_cast<uint32_t>(rng_.Range(0, 99));
  switch (options_.workload) {
    case Workload::kDirectory:
      // Grow one directory to well over a thousand entries first so the
      // shortform -> block -> leaf -> node transitions are actually reached;
      // then churn the same tree with rename/unlink/rmdir.
      if (op_index <= 950) {
        return pressure_dir_ == 0 ? OpType::kMkdir : OpType::kCreate;
      }
      if (roll < 20) return OpType::kCreate;
      if (roll < 35) return OpType::kRename;
      if (roll < 55) return OpType::kUnlink;
      if (roll < 65) return OpType::kRmdir;
      if (roll < 80) return OpType::kHardlink;
      if (roll < 90) return OpType::kMkdir;
      return OpType::kSetXattr;
    case Workload::kXattr:
      if (roll < 35) return OpType::kSetXattr;
      if (roll < 55) return OpType::kReplaceXattr;
      if (roll < 75) return OpType::kRemoveXattr;
      if (roll < 82) return OpType::kCreate;
      if (roll < 90) return OpType::kWrite;
      if (roll < 95) return OpType::kPreallocate;
      return OpType::kMkdir;
    case Workload::kAlloc:
      if (op_index <= 120) return OpType::kSparseWrite;
      if (roll < 22) return OpType::kWrite;
      if (roll < 36) return OpType::kExtend;
      if (roll < 45) return OpType::kOverwrite;
      if (roll < 55) return OpType::kSparseWrite;
      if (roll < 65) return OpType::kTruncateSmaller;
      if (roll < 70) return OpType::kTruncateLarger;
      if (roll < 80) return OpType::kPreallocate;
      if (roll < 85) return OpType::kConvertUnwritten;
      if (roll < 95 || !options_.reflink) return OpType::kWrite;
      return OpType::kReflink;
    case Workload::kEnospc:
      if (roll < 70) return OpType::kPreallocate;
      if (roll < 85) return OpType::kSetXattr;
      if (roll < 93) return OpType::kCreate;
      if (roll < 96) return OpType::kMkdir;
      if (roll < 98) return OpType::kTruncateSmaller;
      if (roll < 99) return OpType::kUnlink;
      return OpType::kRmdir;
    case Workload::kMixed:
      break;
  }
  // A broad mixed workload.  Reflink is only selected on profiles that
  // advertise it; otherwise the slot degrades to an ordinary write.
  if (roll < 8) return OpType::kMkdir;
  if (roll < 22) return OpType::kCreate;
  if (roll < 32) return OpType::kWrite;
  if (roll < 38) return OpType::kExtend;
  if (roll < 44) return OpType::kOverwrite;
  if (roll < 49) return OpType::kSparseWrite;
  if (roll < 55) return OpType::kTruncateSmaller;
  if (roll < 58) return OpType::kTruncateLarger;
  if (roll < 67) return OpType::kRename;
  if (roll < 71) return OpType::kHardlink;
  if (roll < 79) return OpType::kUnlink;
  if (roll < 82) return OpType::kRmdir;
  if (roll < 86) return OpType::kSymlink;
  if (roll < 89) return OpType::kSetXattr;
  if (roll < 91) return OpType::kReplaceXattr;
  if (roll < 93) return OpType::kRemoveXattr;
  if (roll < 96) return OpType::kPreallocate;
  if (roll < 98) return OpType::kConvertUnwritten;
  if (options_.reflink && (op_index % 3) == 0) return OpType::kReflink;
  return OpType::kWrite;
}

Operation Generator::MakeCreateOp(uint64_t op_index, OpType type) {
  Operation op;
  op.type = type;
  uint64_t parent = model_.root();
  if (options_.workload == Workload::kDirectory) {
    if (pressure_dir_ == 0 || !model_.Find(pressure_dir_)) {
      parent = model_.root();
      op.name = FreshName(parent, 'p');
      op.node = NewNodeId();
      pressure_dir_ = op.node;
      op.parent = parent;
      return op;
    }
    parent = pressure_dir_;
  } else {
    const std::vector<uint64_t> dirs = CandidateDirs();
    parent = Pick(dirs, model_.root());
  }
  op.parent = parent;
  const char kind = type == OpType::kMkdir ? 'd'
                    : type == OpType::kSymlink ? 'l'
                                                : 'f';
  op.name = FreshName(parent, kind);
  op.node = NewNodeId();
  if (type == OpType::kSymlink) {
    const uint64_t target_len = rng_.Chance(30) ? rng_.Range(337, 900)
                                                : rng_.Range(4, 300);
    op.symlink_target.resize(static_cast<size_t>(target_len));
    for (uint64_t i = 0; i < target_len; ++i) {
      op.symlink_target[static_cast<size_t>(i)] =
          static_cast<char>('a' + (PatternByte(static_cast<uint32_t>(op_index),
                                               i) % 26));
    }
  }
  return op;
}

Operation Generator::MakeRenameOp(uint64_t op_index) {
  (void)op_index;
  const std::vector<EntryRef> entries = CandidateEntries(false);
  const EntryRef* source = PickEntry(entries);
  if (!source) {
    Operation fallback = FallbackCreate(0);
    return fallback;
  }
  Operation op;
  op.type = OpType::kRename;
  op.parent = source->parent;
  op.name = source->name;
  const ModelNode* moved = model_.Find(source->node);
  const std::vector<uint64_t> dirs = CandidateDirs();
  uint64_t dst_parent = Pick(dirs, model_.root());
  if (moved && moved->type == NodeType::kDirectory) {
    for (int attempt = 0; attempt < 32; ++attempt) {
      if (!model_.IsAncestor(source->node, dst_parent) &&
          dst_parent != source->node) {
        break;
      }
      dst_parent = Pick(dirs, model_.root());
    }
    if (model_.IsAncestor(source->node, dst_parent) ||
        dst_parent == source->node) {
      dst_parent = model_.root();
    }
  }
  op.dst_parent = dst_parent;

  const ModelNode* dst_dir = model_.Find(dst_parent);
  if (rng_.Chance(35) && dst_dir && !dst_dir->entries.empty()) {
    auto it = dst_dir->entries.begin();
    std::advance(it, static_cast<long>(rng_.Range(0, dst_dir->entries.size() - 1)));
    op.dst_name = it->first;
    op.replace_existing = true;
  } else {
    op.dst_name = FreshName(dst_parent, 'r');
    op.replace_existing = false;
  }
  if (op.parent == op.dst_parent && op.name == op.dst_name) {
    op.dst_name = FreshName(dst_parent, 'r');
    op.replace_existing = false;
  }
  return op;
}

Operation Generator::MakeDataOp(uint64_t op_index, OpType type) {
  const std::vector<uint64_t> files = CandidateFiles();
  uint64_t node = Pick(files, 0);
  if (node == 0) return FallbackCreate(0);
  const ModelNode* file = model_.Find(node);
  const uint64_t size = file ? file->data.size() : 0;
  Operation op;
  op.type = type;
  op.node = node;
  op.data_seed = rng_.Next32();
  const uint64_t max_bytes =
      std::min<uint64_t>(options_.max_file_bytes, 8ull << 20);

  switch (type) {
    case OpType::kExtend:
      if (size >= max_bytes) return MakeDataOp(op_index, OpType::kTruncateSmaller);
      op.offset = size;
      op.length = std::min<uint64_t>(rng_.Range(1, 16) * kBlock,
                                     max_bytes - std::min(size, max_bytes));
      if (op.length == 0) op.length = 1;
      break;
    case OpType::kOverwrite:
      if (size == 0) {
        op.type = OpType::kWrite;
        op.offset = 0;
        op.length = std::min<uint64_t>(rng_.Range(1, 8) * kBlock, max_bytes);
      } else {
        op.offset = rng_.Range(0, size - 1);
        uint64_t available = std::min<uint64_t>(size - op.offset,
                                                rng_.Range(1, 16) * kBlock);
        op.length = std::max<uint64_t>(1, available);
      }
      break;
    case OpType::kSparseWrite: {
      if (size >= max_bytes) return MakeDataOp(op_index, OpType::kTruncateSmaller);
      if (options_.workload == Workload::kAlloc && op_index <= 120) {
        // Build a deliberately fragmented file: one written block followed by
        // one hole, 120 times.  This crosses the inline extent limit and the
        // bmbt leaf/node growth thresholds.
        op.offset = (op_index - 2) * 2 * kBlock;
        if (op_index < 2) op.offset = 0;
        op.length = kBlock;
        break;
      }
      const uint64_t gap_blocks = rng_.Range(1, 64);
      op.offset = size + gap_blocks * kBlock;
      if (op.offset >= max_bytes) op.offset = size;
      op.length = std::min<uint64_t>(rng_.Range(1, 4) * kBlock,
                                     max_bytes - std::min(op.offset, max_bytes));
      if (op.length == 0) op.length = 1;
      break;
    }
    case OpType::kCowOverwrite: {
      if (!file || file->cloned_ranges.empty()) {
        op.type = OpType::kOverwrite;
        return MakeDataOp(op_index, OpType::kOverwrite);
      }
      const auto& range = file->cloned_ranges[
          static_cast<size_t>(rng_.Range(0, file->cloned_ranges.size() - 1))];
      if (range.second == 0 || range.first >= max_bytes) {
        op.type = OpType::kWrite;
        op.offset = 0;
        op.length = std::min<uint64_t>(kBlock, max_bytes);
        break;
      }
      op.offset = range.first +
                  rng_.Range(0, std::min<uint64_t>(range.second - 1, 7) * kBlock + 1);
      if (op.offset >= max_bytes) op.offset = range.first;
      op.length = std::min<uint64_t>(rng_.Range(1, 4) * kBlock,
                                     max_bytes - op.offset);
      if (op.length == 0) op.length = 1;
      break;
    }
    case OpType::kTruncateSmaller:
      if (size == 0) {
        // Pick a non-empty file without recursing into a fresh random pick
        // that could land on the size cap.
        for (uint64_t candidate : files) {
          const ModelNode* other = model_.Find(candidate);
          if (other && !other->data.empty()) {
            node = candidate;
            op.node = node;
            op.size = rng_.Chance(30) ? 0 : rng_.Range(0, other->data.size() - 1);
            if (op.size >= other->data.size()) op.size = other->data.size() / 2;
            return op;
          }
        }
        op.type = OpType::kWrite;
        op.offset = 0;
        op.length = std::min<uint64_t>(kBlock, max_bytes);
        break;
      }
      op.size = rng_.Chance(30) ? 0 : rng_.Range(0, size - 1);
      if (op.size >= size) op.size = size / 2;
      break;
    case OpType::kTruncateLarger:
      if (size >= max_bytes) return MakeDataOp(op_index, OpType::kTruncateSmaller);
      op.size = size + std::min<uint64_t>(rng_.Range(1, 64) * kBlock,
                                          max_bytes - std::min(size, max_bytes));
      if (op.size <= size) op.size = size + 1;
      break;
    default:
      op.type = OpType::kWrite;
      if (size >= max_bytes) {
        // A capped file cannot be extended; shrink it instead of emitting a
        // zero-length write.
        op.type = OpType::kTruncateSmaller;
        op.size = size / 2;
        break;
      }
      op.offset = size;
      op.length = std::min<uint64_t>(rng_.Range(1, 8) * kBlock,
                                     max_bytes - std::min(size, max_bytes));
      break;
  }
  return op;
}

Operation Generator::MakeXattrOp(uint64_t op_index, OpType type) {
  const bool node_mode = (op_index % 60) < 30;
  uint64_t node = node_mode ? xattr_node_target_ : xattr_leaf_target_;
  if (node_mode && (node == 0 || !model_.Find(node))) {
    const std::vector<uint64_t> files = CandidateFiles();
    node = Pick(files, 0);
    if (node == 0) return FallbackCreate(0);
    xattr_node_target_ = node;
  } else if (!node_mode &&
             (node == 0 || !model_.Find(node) || xattr_leaf_entries_ >= 8)) {
    const std::vector<uint64_t> files = CandidateFiles();
    node = 0;
    for (uint64_t candidate : files) {
      if (candidate == xattr_node_target_) continue;
      const ModelNode* file = model_.Find(candidate);
      if (file && file->xattrs.size() < 5) {
        node = candidate;
        break;
      }
    }
    if (node == 0) node = Pick(files, 0);
    if (node == 0) return FallbackCreate(0);
    xattr_leaf_target_ = node;
    xattr_leaf_entries_ = 0;
  } else if (!node_mode) {
    ++xattr_leaf_entries_;
  }
  xattr_target_ = node;
  const ModelNode* target = model_.Find(node);
  Operation op;
  op.type = type;
  op.node = node;

  std::vector<std::string> existing;
  if (target) {
    for (const auto& [name, value] : target->xattrs) {
      (void)value;
      existing.push_back(name);
    }
  }
  if ((type == OpType::kReplaceXattr || type == OpType::kRemoveXattr) &&
      existing.empty()) {
    op.type = OpType::kSetXattr;
    type = OpType::kSetXattr;
  }
  if (type == OpType::kReplaceXattr || type == OpType::kRemoveXattr) {
    op.xattr_name = existing[static_cast<size_t>(
        rng_.Range(0, existing.size() - 1))];
  } else {
    op.xattr_name = "user.s" + std::to_string(name_counter_++);
  }
  static constexpr uint32_t kSizes[] = {8, 64, 200, 512, 3000, 7000, 16000};
  op.xattr_length = kSizes[static_cast<size_t>(rng_.Range(0, 6))];
  if (type == OpType::kRemoveXattr) op.xattr_length = 0;
  op.xattr_seed = rng_.Next32();
  return op;
}

Operation Generator::MakeReflinkOp(uint64_t op_index) {
  (void)op_index;
  std::vector<uint64_t> files = CandidateFiles();
  if (files.size() < 2) return FallbackCreate(0);
  uint64_t src = Pick(files, 0);
  uint64_t dst = src;
  for (int attempt = 0; attempt < 16 && dst == src; ++attempt) {
    dst = Pick(files, src);
  }
  if (dst == src) return MakeDataOp(0, OpType::kWrite);
  const ModelNode* src_node = model_.Find(src);
  const uint64_t src_size = src_node ? src_node->data.size() : 0;
  if (src_size < kBlock) return MakeDataOp(0, OpType::kWrite);
  const uint64_t full_blocks = src_size / kBlock;
  const uint64_t src_block = rng_.Range(0, full_blocks - 1);
  const uint64_t available_blocks = full_blocks - src_block;
  const uint64_t length = std::min<uint64_t>(available_blocks,
                                             rng_.Range(1, 8)) * kBlock;
  const uint64_t max_bytes =
      std::min<uint64_t>(options_.max_file_bytes, 8ull << 20);
  uint64_t dst_offset = rng_.Chance(60) ? 0 : rng_.Range(0, 8) * kBlock;
  if (dst_offset + length > max_bytes) dst_offset = 0;
  Operation op;
  op.type = OpType::kReflink;
  op.node = src;
  op.dst_node = dst;
  op.offset = src_block * kBlock;
  op.size = dst_offset;
  op.length = length;
  return op;
}

Operation Generator::MakePreallocateOp(uint64_t op_index) {
  (void)op_index;
  const std::vector<uint64_t> files = CandidateFiles();
  uint64_t node = Pick(files, 0);
  if (node == 0) return FallbackCreate(0);
  const uint64_t max_bytes =
      std::min<uint64_t>(options_.max_file_bytes, 8ull << 20);
  if (options_.workload == Workload::kEnospc) {
    // Fill one file to its cap, then move to the next; this is what makes a
    // 300 MiB image reach ENOSPC instead of scattering small holes.
    for (uint64_t candidate : files) {
      const ModelNode* file = model_.Find(candidate);
      if (file && file->data.size() < max_bytes) {
        node = candidate;
        break;
      }
    }
  }
  const ModelNode* file = model_.Find(node);
  const uint64_t size = file ? file->data.size() : 0;
  if (size >= max_bytes) return MakeDataOp(op_index, OpType::kTruncateSmaller);
  const uint64_t blocks = options_.workload == Workload::kEnospc
                              ? rng_.Range(256, 2048)
                              : rng_.Range(1, 64);
  Operation op;
  op.type = OpType::kPreallocate;
  op.node = node;
  op.offset = size;
  if (options_.workload != Workload::kEnospc && rng_.Chance(20) &&
      size > kBlock) {
    op.offset = rng_.Range(0, size / kBlock - 1) * kBlock;
  }
  op.length = blocks * kBlock;
  if (op.offset + op.length > max_bytes) {
    op.length = max_bytes - std::min(op.offset, max_bytes);
  }
  if (op.length == 0) op.length = 1;
  op.keep_size = options_.workload == Workload::kEnospc ? false
                                                        : rng_.Chance(15);
  return op;
}

Operation Generator::MakeConvertOp(uint64_t op_index) {
  (void)op_index;
  const std::vector<uint64_t> files = CandidateFiles();
  uint64_t node = Pick(files, 0);
  if (node == 0) return FallbackCreate(0);
  const ModelNode* file = model_.Find(node);
  if (!file || file->data.empty()) return MakeDataOp(0, OpType::kWrite);
  Operation op;
  op.type = OpType::kConvertUnwritten;
  op.node = node;
  op.offset = 0;
  op.length = file->data.size();
  return op;
}

Operation Generator::FallbackCreate(uint64_t op_index) {
  return MakeCreateOp(op_index, OpType::kCreate);
}

Operation Generator::Next(uint64_t op_index, std::string* error) {
  std::string last_error;
  for (int attempt = 0; attempt < 24; ++attempt) {
    const OpType type = ChooseType(op_index);
    Operation op;
    switch (type) {
      case OpType::kMkdir:
      case OpType::kCreate:
      case OpType::kSymlink:
        op = MakeCreateOp(op_index, type);
        break;
      case OpType::kRename:
        op = MakeRenameOp(op_index);
        break;
      case OpType::kHardlink: {
        const std::vector<EntryRef> entries = CandidateEntries(true);
        const EntryRef* source = PickEntry(entries);
        if (!source) {
          op = FallbackCreate(op_index);
          break;
        }
        op.type = OpType::kHardlink;
        op.node = source->node;
        const std::vector<uint64_t> dirs = CandidateDirs();
        op.dst_parent = Pick(dirs, model_.root());
        op.dst_name = FreshName(op.dst_parent, 'h');
        break;
      }
      case OpType::kUnlink: {
        const std::vector<EntryRef> entries = CandidateEntries(true);
        const EntryRef* source = PickEntry(entries);
        if (!source) {
          op = FallbackCreate(op_index);
          break;
        }
        op.type = OpType::kUnlink;
        op.parent = source->parent;
        op.name = source->name;
        break;
      }
      case OpType::kRmdir: {
        const std::vector<EntryRef> empty = model_.EmptyDirectories();
        const EntryRef* source = PickEntry(empty);
        if (!source) {
          op = MakeCreateOp(op_index, OpType::kMkdir);
          break;
        }
        op.type = OpType::kRmdir;
        op.parent = source->parent;
        op.name = source->name;
        break;
      }
      case OpType::kSetXattr:
      case OpType::kReplaceXattr:
      case OpType::kRemoveXattr:
        op = MakeXattrOp(op_index, type);
        break;
      case OpType::kReflink:
        op = MakeReflinkOp(op_index);
        break;
      case OpType::kPreallocate:
        op = MakePreallocateOp(op_index);
        break;
      case OpType::kConvertUnwritten:
        op = MakeConvertOp(op_index);
        break;
      default:
        op = MakeDataOp(op_index, type);
        break;
    }

    // Namespace and attribute preconditions are checked on a copy.  Data
    // operations are clamped by construction; copying large content buffers
    // for every write would dominate the fuzzer's runtime.
    if (!IsDataOperation(op.type)) {
      ModelFs trial = model_;
      std::string why;
      if (!trial.Apply(op, &why)) {
        last_error = why;
        continue;
      }
    }
    if (error) error->clear();
    return op;
  }
  if (error) {
    *error = "generator could not produce a legal operation";
    if (!last_error.empty()) *error += ": " + last_error;
  }
  return Operation{};
}

}  // namespace xfstest::stateful
