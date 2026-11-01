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
  // PERF-1: collect and print the optional I/O/lock/cache counters.
  bool perf_stats = false;
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
  // RW-6B formatter/test profile: rmapbt=1, reflink=0.
  bool rmapbt = false;
  // RW-6C formatter/test profile: reflink=1 with independent rmapbt choice.
  bool reflink = false;
  // RW-6E formatter: parent pointers (implies exchange-range) or the full
  // modern xfsprogs 7.2 default profile.
  bool parent_pointers = false;
  bool modern_default_profile = false;
  // Legacy RW-6E development switch, accepted as a no-op now that parent
  // pointers are part of the ordinary writable profile.
  bool parent_qualification = false;
  // xattr list: expose private XFS_ATTR_PARENT entries as parent.*.
  bool include_private = false;
  // check --parents: force the bidirectional parent audit (it is on by default
  // for parent=1 filesystems).
  bool parent_check = false;
  // Legacy hidden qualification overrides.  Both feature sets are part of the
  // ordinary writable profile now; the flags are parsed and ignored so older
  // scripts keep working.
  bool rmapbt_qualification = false;
  bool reflink_qualification = false;
  // RW-6D xattr command options.
  bool xattr_create = false;      // XATTR_CREATE
  bool xattr_hex = false;         // hex output
  bool xattr_value_is_hex = false;  // --value-hex input
  std::string xattr_value_file;   // binary value input
  std::string xattr_output;       // binary value output
  // DEV-1 device qualification.  A raw device is writable only when the caller
  // explicitly asks for qualification, and each risky device class needs its
  // own opt-in.  The defaults refuse fixed and removable disks.
  bool device_write_qualification = false;
  bool allow_fixed_disk_write = false;
  bool allow_removable_disk_write = false;
  std::string device_cache_mode;  // buffered|write-through|no-buffering|both
};

// Runs the CLI. argv excludes the program name.
int Run(const std::vector<std::string>& args);

}  // namespace xfs::cli
