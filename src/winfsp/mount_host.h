// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - mount host (WinFsp lifetime + unmount signalling).
#pragma once

#include <cstdint>
#include <string>

#include "xfs/device_policy.h"

namespace xfs::winfsp {

// A raw disk region: a whole disk (offset 0, length 0) or one partition of it.
// `path` stays the whole-disk device path so the DEV-1 volume-ownership and
// identity rules apply to the handle the partition view is built on.
struct MountTarget {
  std::string path;
  uint64_t offset_bytes = 0;
  uint64_t length_bytes = 0;  // 0 = to the end of `path`
};

// Mounts `target` (image or raw device) at `drive` (e.g. "X:"). Blocks until
// unmounted. Returns a process exit code.
//
// `read_write` requests a writable mount. `device_policy` is the DEV-1 policy
// for a raw target (ignored for image files); a raw read-write mount without a
// policy uses the conservative qualification policy (virtual disks only).
int RunMountHost(const MountTarget& target, const std::string& drive,
                 bool force_unsafe_ro, bool verbose, bool read_write,
                 bool perf_stats,
                 bool allow_rmapbt_qualification = false,
                 bool allow_reflink_qualification = false,
                 const DeviceOpenPolicy* device_policy = nullptr);

// Compatibility overload for callers that mount a whole target.
int RunMountHost(const std::string& target, const std::string& drive,
                 bool force_unsafe_ro, bool verbose, bool read_write,
                 bool perf_stats,
                 bool allow_rmapbt_qualification = false,
                 bool allow_reflink_qualification = false,
                 const DeviceOpenPolicy* device_policy = nullptr);

// Signals a running mount of `drive` to stop. Returns a process exit code.
int StopMountHost(const std::string& drive);

}  // namespace xfs::winfsp
