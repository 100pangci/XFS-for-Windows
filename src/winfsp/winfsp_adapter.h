// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - WinFsp native API adapter (read-only, or read-write for
// the supported profile on an image or a qualified raw device).
#pragma once

#ifdef XFSWIN_HAS_WINFSP

#include <cstdint>
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
  // Read-write mount. The CLI and the raw-device policy decide whether the
  // target is writable; the adapter enables the Write/Flush callbacks, reports
  // writable file attributes and asks WinFsp to flush and purge its cache on
  // cleanup.
  bool read_write = false;
  // Enable the optional performance counters for this mount.
  bool perf_stats = false;
};

// Creates a WinFsp file system object backed by `filesystem`, mounts it at
// config.mount_point and runs the dispatcher until the stop event is set
// (via StopMount) or the process receives Ctrl+C. Blocks.
Status RunMountedFilesystem(std::unique_ptr<XfsFilesystem> filesystem,
                            const MountConfig& config);

// A live WinFsp mount owned by the caller. Start() returns once the drive is
// visible; a dedicated thread waits for the stop event and performs the clean
// unmount (journal close, flush, drive-letter release). Several sessions can
// coexist in one process, each with its own device, adapter state and stop
// event. Stop() signals that event and waits until cleanup has finished.
class MountSession {
 public:
  static Result<std::unique_ptr<MountSession>> Start(
      std::unique_ptr<XfsFilesystem> filesystem, const MountConfig& config);
  ~MountSession();

  MountSession(const MountSession&) = delete;
  MountSession& operator=(const MountSession&) = delete;

  const std::string& id() const;
  const std::wstring& mount_point() const;
  // True until the cleanup thread has released the mount point.
  bool alive() const;
  // Async-signal-safe request: only sets the stop event. No-op after cleanup.
  void RequestStop();
  // Signals and waits for cleanup. Idempotent; returns the unmount status.
  Status Stop();
  // Blocks until the session stops for any reason (Stop or device failure).
  Status Wait();
  // True when the mount was requested read-write.
  bool read_write() const;

 private:
  struct Impl;
  explicit MountSession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

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
