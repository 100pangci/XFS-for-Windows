// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/drive_letter_takeover.h"

#include <algorithm>
#include <utility>

#include "xfs/drive_letter.h"
#include "xfs/uuid.h"

namespace xfs {
namespace {

std::string NormalizeOrEmpty(std::string_view drive) {
  if (drive.empty()) return {};
  auto normalized = NormalizeDriveLetter(drive);
  return normalized.ok() ? normalized.value() : std::string();
}

// Takeover is limited to D:..Z: exactly like automatic allocation.  Taking
// over C: could detach the Windows system volume, and A:/B: are floppy-class
// letters.  This check is deliberately independent of the caller.
bool IsTakeoverLetter(std::string_view drive) {
  return drive.size() == 2 && drive[1] == ':' && drive[0] >= 'D' &&
         drive[0] <= 'Z';
}

TakeoverResult Failed(TakeoverResult result, XfsError code,
                      const std::string& message) {
  result.status = XFS_ERR(code, message).Metadata("drive-letter takeover");
  result.takeover.state = TakeoverState::kFailed;
  result.takeover.error = message;
  return result;
}

Status RestoreInternal(TakeoverOps& ops, DriveLetterTakeover* takeover) {
  if (!takeover) return OkStatus();
  if (takeover->state == TakeoverState::kNone ||
      takeover->state == TakeoverState::kRestored) {
    return OkStatus();
  }
  if (takeover->mount_point.empty() || takeover->volume_name.empty()) {
    // Nothing was captured, so no Windows state was changed.  This is the
    // normal path for a pre-removal failure.
    takeover->state = TakeoverState::kRestored;
    takeover->error.clear();
    return OkStatus();
  }
  if (!ops.VolumePresent(*takeover)) {
    // The device was removed.  Re-creating a mount point on a dead volume
    // would either fail or attach a future, different volume to the old
    // letter; both are worse than leaving the letter free.
    takeover->volume_present = false;
    takeover->state = TakeoverState::kRestored;
    takeover->error.clear();
    return OkStatus();
  }
  takeover->volume_present = true;

  Status restored = ops.RestoreMountPoint(takeover);
  if (restored.ok()) {
    Status verified = ops.VerifyRestored(*takeover);
    if (verified.ok()) {
      takeover->state = TakeoverState::kRestored;
      takeover->error.clear();
      return OkStatus();
    }
    restored = verified;
  }
  takeover->state = TakeoverState::kRestorePending;
  takeover->error = restored.error().Format();
  return restored;
}

}  // namespace

const char* ToString(TakeoverState state) {
  switch (state) {
    case TakeoverState::kNone: return "none";
    case TakeoverState::kObserved: return "observed";
    case TakeoverState::kPrepared: return "prepared";
    case TakeoverState::kOriginalRemoved: return "original-removed";
    case TakeoverState::kMounting: return "mounting";
    case TakeoverState::kOwned: return "owned";
    case TakeoverState::kRollingBack: return "rolling-back";
    case TakeoverState::kRestored: return "restored";
    case TakeoverState::kRestorePending: return "restore-pending";
    case TakeoverState::kFailed: return "failed";
  }
  return "unknown";
}

bool TakeoverNeedsRestore(const DriveLetterTakeover& takeover) {
  switch (takeover.state) {
    case TakeoverState::kOriginalRemoved:
    case TakeoverState::kMounting:
    case TakeoverState::kOwned:
    case TakeoverState::kRollingBack:
    case TakeoverState::kRestorePending:
      return !takeover.mount_point.empty();
    default:
      return false;
  }
}

TakeoverResult PrepareDriveLetterTakeover(
    TakeoverOps& ops, const DiscoveredFilesystem& filesystem,
    std::string_view drive_view, std::string_view release_drive_view,
    std::string_view release_volume_device_path) {
  TakeoverResult result;
  const std::string drive = NormalizeOrEmpty(drive_view);
  if (drive.empty()) {
    return Failed(std::move(result), XfsError::kInvalidArgument,
                  "invalid drive letter for the WinFsp mount");
  }
  result.takeover.drive = drive;

  if (release_drive_view.empty()) {
    // No same-volume Windows letter: this is a plain allocation, not a
    // takeover.  The record is a no-op handle the caller may keep.
    result.takeover.state = TakeoverState::kNone;
    return result;
  }

  const std::string release_drive = NormalizeOrEmpty(release_drive_view);
  if (release_drive.empty() || !IsTakeoverLetter(release_drive)) {
    return Failed(std::move(result), XfsError::kInvalidArgument,
                  "the Windows drive letter " + std::string(release_drive_view) +
                      " is not in the D:..Z: takeover range");
  }
  result.takeover.original_drive = release_drive;
  result.takeover.fs_uuid = FormatUuid(filesystem.probe.superblock.uuid);
  result.takeover.disk_number = filesystem.location.disk_number;
  result.takeover.partition_number = filesystem.location.partition_number;
  result.takeover.offset_bytes = filesystem.location.offset_bytes;
  result.takeover.length_bytes = filesystem.location.length_bytes;
  result.takeover.state = TakeoverState::kObserved;

  // Re-check the discovery facts locally before touching Windows.  The
  // platform verifies them again against live state; neither check is allowed
  // to trust a drive letter or a disk number on its own.
  //
  // The caller can supply a volume GUID it has just verified live as the owner
  // of `release_drive`.  A pass that runs right after a restore may see the
  // Mount Manager's per-volume path list before it lists that letter again,
  // so the snapshot checks below would reject a correlation that is already
  // true.  Skipping them is safe because every identity check that matters is
  // repeated against live state in VerifyAndCapture before the mount point is
  // removed.
  const bool release_live_verified = !release_volume_device_path.empty();
  if (filesystem.probe.state != XfsProbeState::kValid) {
    return Failed(std::move(result), XfsError::kUnsupportedFeature,
                  "the XFS superblock did not verify; no drive letter may be "
                  "taken over");
  }
  if (!release_live_verified) {
    if (!filesystem.windows_volume_found ||
        !filesystem.volume_identity_exact) {
      return Failed(std::move(result), XfsError::kNotMounted,
                    "the Windows volume does not match the XFS candidate's "
                    "disk extents exactly");
    }
    if (!filesystem.volume_unrecognized_raw) {
      return Failed(std::move(result), XfsError::kInvalidArgument,
                    "the Windows volume has a recognized filesystem; its "
                    "drive letter is never taken over");
    }
    if (filesystem.volume_device_path.empty()) {
      return Failed(std::move(result), XfsError::kInvalidArgument,
                    "the Windows volume identity is missing");
    }
  }
  if (IsNullUuid(filesystem.probe.superblock.uuid)) {
    return Failed(std::move(result), XfsError::kBadUuid,
                  "the verified XFS filesystem has no UUID");
  }
  if (!release_live_verified) {
    const std::string only_drive =
        SingleDriveLetterMountPoint(filesystem.mount_points);
    if (only_drive.empty()) {
      return Failed(std::move(result), XfsError::kAlreadyExists,
                    "the Windows volume does not have exactly one drive-letter "
                    "mount point; multiple letters are never taken over");
    }
    if (only_drive != release_drive) {
      return Failed(std::move(result), XfsError::kInvalidArgument,
                    "drive " + release_drive +
                        " is not the Windows volume's drive letter");
    }
  }

  Status verified = ops.VerifyAndCapture(filesystem, drive, release_drive,
                                         release_volume_device_path,
                                         &result.takeover);
  if (!verified.ok()) {
    result.status = verified;
    result.takeover.state = TakeoverState::kFailed;
    result.takeover.error = verified.error().Format();
    return result;
  }
  result.takeover.state = TakeoverState::kPrepared;

  Status removed = ops.RemoveMountPoint(&result.takeover);
  if (!removed.ok()) {
    // DeleteVolumeMountPointW can report a failure after the association is
    // gone (for example when Explorer still probes the old letter).  Verify
    // instead of assuming, so the volume is not left with no mount point.
    Status check = ops.VerifyRemoved(result.takeover);
    if (!check.ok()) {
      result.status = removed;
      result.takeover.state = TakeoverState::kFailed;
      result.takeover.error = removed.error().Format();
      return result;
    }
  }
  result.takeover.state = TakeoverState::kOriginalRemoved;

  Status confirmed = ops.VerifyRemoved(result.takeover);
  if (!confirmed.ok()) {
    const std::string detail = confirmed.error().Format();
    const Status rollback = RollbackDriveLetterTakeover(ops, &result.takeover);
    if (!rollback.ok()) {
      result.status =
          XFS_ERR(XfsError::kIoError,
                  "the Windows mount point " + result.takeover.mount_point +
                      " could not be confirmed removed and restoring it failed: " +
                      rollback.error().Format())
              .Metadata("drive-letter takeover");
    } else {
      result.status = confirmed;
    }
    return result;
  }

  return result;
}

void MarkTakeoverMounting(DriveLetterTakeover* takeover) {
  if (!takeover) return;
  if (takeover->state == TakeoverState::kOriginalRemoved) {
    takeover->state = TakeoverState::kMounting;
  }
}

void MarkTakeoverOwned(DriveLetterTakeover* takeover) {
  if (!takeover) return;
  if (takeover->state == TakeoverState::kMounting) {
    takeover->state = TakeoverState::kOwned;
  }
}

Status RollbackDriveLetterTakeover(TakeoverOps& ops,
                                   DriveLetterTakeover* takeover) {
  if (!takeover) return OkStatus();
  if (!TakeoverNeedsRestore(*takeover)) {
    if (takeover->state == TakeoverState::kObserved ||
        takeover->state == TakeoverState::kPrepared) {
      // No mount point was detached yet; there is nothing to undo.
      takeover->state = TakeoverState::kFailed;
    }
    return OkStatus();
  }
  takeover->state = TakeoverState::kRollingBack;
  return RestoreInternal(ops, takeover);
}

Status RestoreDriveLetterTakeover(TakeoverOps& ops,
                                  DriveLetterTakeover* takeover) {
  return RestoreInternal(ops, takeover);
}

Status RetryDriveLetterRestore(TakeoverOps& ops,
                               DriveLetterTakeover* takeover) {
  return RestoreInternal(ops, takeover);
}

}  // namespace xfs
