// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// FUZZ-2: deterministic, precondition-aware operation generator.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "stateful/model.h"

namespace xfstest::stateful {

enum class Workload {
  kMixed,
  kDirectory,
  kXattr,
  kAlloc,
  kEnospc,
};

bool ParseWorkload(const std::string& name, Workload* out);
const char* ToString(Workload workload);

struct GeneratorOptions {
  uint64_t seed = 0;
  Workload workload = Workload::kMixed;
  bool modern_profile = true;
  bool reflink = true;
  uint64_t max_file_bytes = 8ull << 20;
};

// SplitMix64: tiny, deterministic, and independent of the C++ standard
// library's distribution implementation.
class Rng {
 public:
  explicit Rng(uint64_t seed) : state_(seed) {}
  uint64_t Next();
  uint64_t Range(uint64_t low, uint64_t high);  // inclusive
  bool Chance(uint32_t percent);
  uint32_t Next32() { return static_cast<uint32_t>(Next()); }

 private:
  uint64_t state_;
};

class Generator {
 public:
  Generator(const ModelFs& model, const GeneratorOptions& options);

  // Generates the next operation.  The caller applies it to the model.  The
  // generator checks the precondition on a copy first, so a successful return
  // means the operation is legal in the current model state.
  Operation Next(uint64_t op_index, std::string* error);

  uint64_t next_node_id() const { return next_node_id_; }
  void set_next_node_id(uint64_t value) { next_node_id_ = value; }

 private:
  const ModelFs& model_;
  GeneratorOptions options_;
  Rng rng_;
  uint64_t next_node_id_ = 1;
  uint64_t name_counter_ = 1;
  uint64_t pressure_dir_ = 0;
  uint64_t xattr_target_ = 0;
  uint64_t xattr_node_target_ = 0;
  uint64_t xattr_leaf_target_ = 0;
  uint32_t xattr_leaf_entries_ = 0;

  OpType ChooseType(uint64_t op_index);
  uint64_t NewNodeId() { return next_node_id_++; }
  std::string FreshName(uint64_t parent, char kind);
  std::vector<uint64_t> CandidateFiles() const;
  std::vector<uint64_t> CandidateDirs() const;
  std::vector<EntryRef> CandidateEntries(bool non_directory) const;
  uint64_t Pick(const std::vector<uint64_t>& values, uint64_t fallback);
  const EntryRef* PickEntry(const std::vector<EntryRef>& entries);

  Operation MakeCreateOp(uint64_t op_index, OpType type);
  Operation MakeRenameOp(uint64_t op_index);
  Operation MakeDataOp(uint64_t op_index, OpType type);
  Operation MakeXattrOp(uint64_t op_index, OpType type);
  Operation MakeReflinkOp(uint64_t op_index);
  Operation MakePreallocateOp(uint64_t op_index);
  Operation MakeConvertOp(uint64_t op_index);
  Operation FallbackCreate(uint64_t op_index);
};

}  // namespace xfstest::stateful
