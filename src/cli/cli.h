// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - command line interface.
#pragma once

#include <string>
#include <vector>

namespace xfs::cli {

struct Options {
  bool verbose = false;
  bool debug = false;
  bool force_unsafe_ro = false;
  bool yes_i_really_mean_it = false;
  // Machine-readable output for `journal` (the recovery differential).
  bool json = false;
  // Read-write mounting is experimental and restricted to image files: the
  // RW-0 writer can only overwrite existing allocated blocks, metadata
  // mutations (timestamps, allocation) are not implemented yet.
  bool rw_experimental = false;
  // Formatter-only modern inode feature switches (RW-6A).
  bool finobt = false;
  bool inobtcount = false;
  bool sparse_inodes = false;
  bool bigtime = false;
  bool nrext64 = false;
};

// Runs the CLI. argv excludes the program name.
int Run(const std::vector<std::string>& args);

}  // namespace xfs::cli
