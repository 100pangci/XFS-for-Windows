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

}  // namespace xfs::winfsp

#endif  // XFSWIN_HAS_WINFSP
