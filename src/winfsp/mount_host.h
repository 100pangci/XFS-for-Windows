// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - mount host (WinFsp lifetime + unmount signalling).
#pragma once

#include <string>

namespace xfs::winfsp {

// Mounts `target` (image or raw device) at `drive` (e.g. "X:"). Blocks until
// unmounted. Returns a process exit code.
//
// `read_write` requests a writable mount. Callers must have validated the
// target: the CLI only allows read-write for image files, never for raw
// devices (see xfswin mount --rw-experimental). A read-write mount opens the
// image writable and mounts the filesystem with the writable profile check.
int RunMountHost(const std::string& target, const std::string& drive,
                 bool force_unsafe_ro, bool verbose, bool read_write,
                 bool allow_rmapbt_qualification = false,
                 bool allow_reflink_qualification = false);

// Signals a running mount of `drive` to stop. Returns a process exit code.
int StopMountHost(const std::string& drive);

}  // namespace xfs::winfsp
