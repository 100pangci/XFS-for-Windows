// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - WinFsp native API adapter (read-only).
#pragma once

#ifdef XFSWIN_HAS_WINFSP

#include <memory>
#include <string>

#include "xfs/error.h"
#include "xfs/filesystem.h"
#include "xfs/superblock.h"

namespace xfs::winfsp {

struct MountConfig {
  std::wstring mount_point;  // e.g. L"X:"
  bool force_unsafe_ro = false;
  bool verbose = false;
  // Read-write mount (images only; enforced by the CLI). The adapter enables
  // the Write/Flush callbacks, reports writable file attributes and asks
  // WinFsp to flush and purge its cache on cleanup.
  bool read_write = false;
};

// Creates a WinFsp file system object backed by `filesystem`, mounts it at
// config.mount_point and runs the dispatcher until the stop event is set
// (via StopMount) or the process receives Ctrl+C. Blocks.
Status RunMountedFilesystem(std::unique_ptr<XfsFilesystem> filesystem,
                            const MountConfig& config);

// Requests a running `xfswin mount` (in another process) to unmount the
// given drive letter.
Status RequestUnmount(const std::wstring& drive);

bool IsValidDriveLetter(const std::wstring& drive);

// Drives the adapter callbacks directly (without a WinFsp driver or mount) to
// validate buffer handling: open/list directories and files, read data and
// verify the process heap after every step. Used by `xfswin selftest`. When
// the filesystem is writable the self-test also overwrites bytes inside an
// existing file through the real Write callback and verifies them.
int RunAdapterSelfTest(std::unique_ptr<XfsFilesystem> filesystem);

}  // namespace xfs::winfsp

#endif  // XFSWIN_HAS_WINFSP
