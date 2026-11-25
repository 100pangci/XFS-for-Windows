// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// FUZZ-2: semantic comparison between the independent model and the real
// filesystem.  This verifier only calls the public read API; it never derives
// an expected result from the filesystem's own parser.
#pragma once

#include <cstdint>
#include <string>

#include "stateful/model.h"
#include "xfs/filesystem.h"

namespace xfstest::stateful {

struct VerifyResult {
  bool ok = false;
  std::string error;
};

// Compares every path, type, nlink, size, logical content, symlink target,
// xattr and directory listing in `model`.  `skip_node` (when nonzero) is a
// file whose size/content are covered by the weaker crash contract instead.
VerifyResult VerifyModel(xfs::XfsFilesystem& fs, const ModelFs& model,
                         uint64_t skip_node = 0);

// Crash verification for a data operation.  `skeleton` selects the logical
// before/after state guaranteed by the crash point; independently written file
// payload bytes still follow the old/new (or partial-data) contract.  When
// requested, the target's logical size must match that selected state.
VerifyResult VerifyRangeCrash(xfs::XfsFilesystem& fs, const ModelFs& skeleton,
                              const ModelFs& before, const ModelFs& after,
                              const Operation& op,
                              bool require_expected_size);

// Reads a file through the public API.  Holes read as zero bytes.
bool ReadAll(xfs::XfsFilesystem& fs, uint64_t ino, uint64_t size,
             std::vector<uint8_t>* out, std::string* error);

bool IsNoSpace(const xfs::ErrorContext& error);
std::string DescribeError(const xfs::ErrorContext& error);

}  // namespace xfstest::stateful
