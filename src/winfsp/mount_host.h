// XFS for Windows - mount host (WinFsp lifetime + unmount signalling).
#pragma once

#include <string>

namespace xfs::winfsp {

// Mounts `target` (image or raw device) read-only at `drive` (e.g. "X:").
// Blocks until unmounted. Returns a process exit code.
int RunMountHost(const std::string& target, const std::string& drive,
                 bool force_unsafe_ro, bool verbose);

// Signals a running mount of `drive` to stop. Returns a process exit code.
int StopMountHost(const std::string& drive);

}  // namespace xfs::winfsp
