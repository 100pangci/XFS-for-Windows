// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - command line interface.
#pragma once

#include <cstdint>
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
  // The documented read-write writer switch.  Image files need this flag
  // alone; a real device additionally needs device_write_qualification and the
  // class opt-in, and the backend's device policy still applies.
  bool read_write = false;
  // Deprecated spelling: `--rw-experimental` sets the same read_write flag and
  // is kept for scripts written before the profile graduated.  This bit only
  // preserves the legacy automount-enrollment qualification requirement and
  // drives the deprecation warning.
  bool rw_experimental_alias = false;
  // UX-1: `automount enable --rw` is the ordinary one-time enrollment spelling.
  // It never authorizes a manual raw write; it records the identity pins after
  // an explicit confirmation and applies only to automatic mounting.
  bool automount_rw = false;
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
  int device_concurrency_rounds = 0;  // device-selftest concurrency matrix
  std::string device_cache_mode;  // buffered|write-through|no-buffering|both
  // DEV-1R: bind a destructive run to one physical device.  The backend
  // compares these values with the freshly probed identity before any volume
  // is locked or any byte is written.
  std::string expect_serial;
  std::string expect_stable_id;
  uint64_t expect_size_bytes = 0;
};

// Runs the CLI. argv excludes the program name.
int Run(const std::vector<std::string>& args);

}  // namespace xfs::cli
