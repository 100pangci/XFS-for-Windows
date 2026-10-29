// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// FUZZ-2: independent state model for stateful filesystem fuzzing.
//
// This model is deliberately not built on the XFS readers or writers.  It
// knows only the operations the generator emits and the semantic state they
// produce.  The real filesystem is compared against it; the model never
// derives an expectation from an on-disk structure.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xfstest::stateful {

enum class NodeType { kFile, kDirectory, kSymlink };

enum class OpType {
  kMkdir,
  kCreate,
  kWrite,
  kExtend,
  kOverwrite,
  kSparseWrite,
  kTruncateSmaller,
  kTruncateLarger,
  kRename,
  kHardlink,
  kUnlink,
  kRmdir,
  kSymlink,
  kSetXattr,
  kReplaceXattr,
  kRemoveXattr,
  kReflink,
  kCowOverwrite,
  kPreallocate,
  kConvertUnwritten,
};

const char* ToString(NodeType type);
const char* ToString(OpType type);
bool ParseOpType(const std::string& name, OpType* out);
bool IsDataOperation(OpType type);
// Data operations whose semantic contract after a crash is weaker than
// "complete old or complete new state".
bool IsRangeCrashOperation(OpType type);

struct Operation {
  OpType type = OpType::kCreate;
  uint64_t node = 0;         // subject (file/symlink/xattr target, new node)
  uint64_t dst_node = 0;     // reflink destination file
  uint64_t parent = 0;       // source/new parent (model node id)
  uint64_t dst_parent = 0;   // rename/link destination parent
  std::string name;          // source/new name
  std::string dst_name;      // rename/link destination name
  uint64_t offset = 0;
  uint64_t length = 0;
  uint64_t size = 0;         // truncate target size
  uint32_t data_seed = 0;    // deterministic payload seed
  std::string xattr_name;
  uint32_t xattr_seed = 0;
  uint32_t xattr_length = 0;
  bool replace_existing = false;
  bool keep_size = false;
  std::string symlink_target;

  // Human-readable one-line form (trace).
  std::string Describe() const;
};

// Deterministic payload byte.  Both the model and the executor use this
// function, so only the operation log is needed to reconstruct the data.
uint8_t PatternByte(uint32_t seed, uint64_t offset);
std::vector<uint8_t> PatternBytes(uint32_t seed, uint64_t length);

struct ModelNode {
  uint64_t id = 0;
  NodeType type = NodeType::kFile;
  uint64_t actual_ino = 0;  // filled in by the executor, never by the model
  std::vector<uint8_t> data;  // logical file content (holes are zeros)
  std::string target;         // symlink target
  std::map<std::string, uint64_t> entries;  // directory name -> node id
  std::map<std::string, std::vector<uint8_t>> xattrs;
  bool shared_hint = false;
  std::vector<std::pair<uint64_t, uint64_t>> cloned_ranges;

  uint64_t Size() const {
    return type == NodeType::kFile ? data.size() : target.size();
  }
};

struct EntryRef {
  uint64_t parent = 0;
  std::string name;
  uint64_t node = 0;
};

class ModelFs {
 public:
  ModelFs();

  ModelFs Clone() const { return *this; }

  uint64_t root() const { return root_; }
  const std::map<uint64_t, ModelNode>& nodes() const { return nodes_; }
  std::map<uint64_t, ModelNode>& mutable_nodes() { return nodes_; }
  const ModelNode* Find(uint64_t id) const;
  ModelNode* FindMutable(uint64_t id);

  // Applies an operation to the model.  Returns false with a diagnostic when
  // the operation is not legal in the current state.  The generator uses this
  // as the precondition check; the runner applies it to obtain the expected
  // state before calling the real filesystem.
  bool Apply(const Operation& op, std::string* error);

  std::string PathOf(uint64_t id) const;
  std::vector<std::string> PathsOf(uint64_t id) const;
  std::vector<uint64_t> Directories() const;
  std::vector<uint64_t> Files() const;
  std::vector<uint64_t> Symlinks() const;
  std::vector<EntryRef> Entries() const;
  std::vector<EntryRef> EmptyDirectories() const;
  uint32_t NameCount(uint64_t id) const;
  uint32_t ExpectedNlink(uint64_t id) const;
  bool IsAncestor(uint64_t ancestor, uint64_t node) const;
  bool IsEmptyDirectory(uint64_t id) const;

  // FNV-1a 64 over the logical content / xattr value.  The independent Python
  // oracle implements the same hash.
  uint64_t ContentHash(uint64_t id) const;
  static uint64_t HashBytes(const std::vector<uint8_t>& bytes);

  // Emits one state object for the independent oracle.  `indent` is applied to
  // every line so the caller can embed it in an array.
  std::string ToJson(const std::string& indent) const;

 private:
  uint64_t root_ = 0;
  uint64_t next_id_ = 1;
  std::map<uint64_t, ModelNode> nodes_;

  bool InsertEntry(uint64_t parent, const std::string& name, uint64_t child,
                   std::string* error);
  void RemoveEntry(uint64_t parent, const std::string& name);
  void CollectEntries(uint64_t dir, std::vector<EntryRef>* out) const;
};

std::string JsonEscape(const std::string& value);

// Stable tab-separated operation encoding for the reducer and replay mode.
// Names and targets are hex so the format never depends on whitespace.
std::string OperationToLine(const Operation& op);
bool OperationFromLine(const std::string& line, Operation* op,
                       std::string* error);

}  // namespace xfstest::stateful
