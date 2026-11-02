// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - transactional drive-letter takeover.
//
// Windows assigns a drive letter to an XFS volume it cannot read; Explorer
// then shows that letter as RAW and offers to format it.  UX-1 makes the XFS
// automount take that same letter over: the RAW mount point is detached, the
// WinFsp filesystem is mounted at the letter, and the original Windows mount
// point is restored when the session ends.  Everything here is platform
// independent; the Win32 operations are injected through TakeoverOps.
//
// State machine:
//
//   kNone ────────────── nothing to take over (no same-volume letter)
//   kObserved ────────── the same-volume RAW letter was identified
//   kPrepared ────────── identity re-verified, original mount point captured
//   kOriginalRemoved ─── DeleteVolumeMountPointW succeeded and was verified
//   kMounting ────────── the WinFsp mount is in progress
//   kOwned ───────────── WinFsp owns the letter
//        │
//        ├── rollback before the WinFsp mount: kRollingBack → kRestored
//        └── restore after the session ends:    kRestored / kRestorePending
//
// A takeover is only started for a volume whose Windows filesystem is
// unrecognized (ERROR_UNRECOGNIZED_VOLUME), whose volume extents match the XFS
// candidate exactly, and whose only drive-letter mount point is the letter
// being taken over.  Directory mount points are captured and never touched.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xfs/device_discovery.h"
#include "xfs/error.h"

namespace xfs {

enum class TakeoverState : uint8_t {
  kNone = 0,
  kObserved,
  kPrepared,
  kOriginalRemoved,
  kMounting,
  kOwned,
  kRollingBack,
  kRestored,
  kRestorePending,
  kFailed,
};

const char* ToString(TakeoverState state);

// The session-scoped record.  It is never persisted: the original mount point
// belongs to the current device insertion, and restoring it after a reboot
// would re-create a mount point for a volume this process no longer verified.
struct DriveLetterTakeover {
  TakeoverState state = TakeoverState::kNone;
  // Letter WinFsp mounted / will mount, e.g. "F:".
  std::string drive;
  // Original Windows drive-letter mount point that was detached, e.g. "F:\\".
  // It can differ from `drive` when a preferred letter was configured; the
  // original letter is then still released so no unusable RAW drive remains.
  std::string mount_point;
  std::string original_drive;      // "F:" derived from mount_point
  std::string volume_name;         // "\\?\Volume{...}\\"
  std::string volume_device_path;  // "\\?\Volume{...}"
  std::string fs_uuid;
  uint32_t disk_number = 0;
  uint32_t partition_number = 0;
  uint64_t offset_bytes = 0;
  uint64_t length_bytes = 0;
  // True while the source volume was last seen attached.  A restore for a
  // removed device is a no-op, never an attempt against a dead volume.
  bool volume_present = false;
  // Directory mount points found on the same volume.  Display/diagnostic only;
  // the takeover never removes them.
  std::vector<std::string> other_mount_points;
  std::string error;
};

// True when the record refers to a Windows mount point that may still have to
// be restored (the volume may already be gone; restore decides that).
bool TakeoverNeedsRestore(const DriveLetterTakeover& takeover);

// The platform operations.  Each one reports failure through Status; the
// transaction function turns a failure into the right state and, where it is
// safe, a rollback.  Implementations must re-read live Windows state in
// VerifyAndCapture instead of trusting the discovery snapshot.
class TakeoverOps {
 public:
  virtual ~TakeoverOps() = default;

  // Re-verifies, against live Windows state, that:
  //   * the volume identified by filesystem.volume_device_path is still
  //     enumerated and still reports an unrecognized filesystem;
  //   * its disk extents still match the XFS candidate exactly;
  //   * `release_drive` is a normal drive-letter mount point of that volume
  //     and the only one (multiple letters must never be taken over silently);
  // then fills volume_name, mount_point, other_mount_points and the identity
  // fields of the record.
  virtual Status VerifyAndCapture(const DiscoveredFilesystem& filesystem,
                                  std::string_view drive,
                                  std::string_view release_drive,
                                  DriveLetterTakeover* takeover) = 0;

  // Detaches the recorded mount point.  Must not touch directory mount points.
  virtual Status RemoveMountPoint(DriveLetterTakeover* takeover) = 0;

  // True when the recorded mount point no longer resolves to the volume.
  virtual Status VerifyRemoved(const DriveLetterTakeover& takeover) = 0;

  virtual bool VolumePresent(const DriveLetterTakeover& takeover) = 0;

  // Re-creates the original mount point on the volume, refusing a letter that
  // now belongs to a different volume.
  virtual Status RestoreMountPoint(DriveLetterTakeover* takeover) = 0;

  // True when the letters's volume is the recorded volume again.
  virtual Status VerifyRestored(const DriveLetterTakeover& takeover) = 0;
};

struct TakeoverResult {
  Status status;
  // Always populated, including on failure: a failed rollback leaves a record
  // in kRestorePending so the caller can retry the restoration.
  DriveLetterTakeover takeover;
};

// Runs Observed → Prepared → OriginalRemoved.  A no-op (kNone, Ok) when
// `release_drive` is empty.  If removal succeeds but verification fails, the
// original mount point is restored before returning the error.
TakeoverResult PrepareDriveLetterTakeover(TakeoverOps& ops,
                                          const DiscoveredFilesystem& filesystem,
                                          std::string_view drive,
                                          std::string_view release_drive);

// Records the transition before the WinFsp mount is attempted.  Kept separate
// so a crash between removal and mount leaves an accurate record.
void MarkTakeoverMounting(DriveLetterTakeover* takeover);

// Records that the WinFsp mount succeeded and the letter is ours.
void MarkTakeoverOwned(DriveLetterTakeover* takeover);

// Undoes a takeover that never produced a usable mount.  Restores the original
// mount point (never to a removed volume); on failure leaves the record in
// kRestorePending and returns the error.
Status RollbackDriveLetterTakeover(TakeoverOps& ops,
                                   DriveLetterTakeover* takeover);

// Restores after a session ends (service stop, removal, automount disable).
// A missing volume is success with volume_present=false: the original mount
// point must not be re-created for a detached device.
Status RestoreDriveLetterTakeover(TakeoverOps& ops,
                                  DriveLetterTakeover* takeover);

// One reconciliation retry of a failed restore.  Identical to
// RestoreDriveLetterTakeover, but keeps kRestorePending on failure.
Status RetryDriveLetterRestore(TakeoverOps& ops,
                               DriveLetterTakeover* takeover);

}  // namespace xfs
