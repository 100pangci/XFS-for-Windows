// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - mount lifecycle and writable-profile preflight.
//
// Owner: superblock/geometry validation, log cleanliness/recovery ordering and
// the decision to expose a writable namespace. It does not own replay, the
// transaction writer or cache invalidation.
#include "xfs/filesystem.h"

#include <memory>
#include <string>
#include <utility>

#include "xfs/log.h"
#include "xfs/perf_stats.h"
#include "xfs/recovery.h"
#include "xfs/superblock.h"
#include "xfs/write.h"

namespace xfs {
namespace {

std::string Hex(uint64_t v) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out = "0x";
  bool started = false;
  for (int shift = 60; shift >= 0; shift -= 4) {
    const uint32_t digit = static_cast<uint32_t>((v >> shift) & 0xf);
    if (digit != 0 || started || shift == 0) {
      out.push_back(kDigits[digit]);
      started = true;
    }
  }
  return out;
}

}  // namespace

Result<std::unique_ptr<XfsFilesystem>> XfsFilesystem::Mount(
    std::unique_ptr<BlockDevice> device, const MountOptions& options) {
  if (options.perf_stats) PerfStats::Get().SetEnabled(true);
  if (!device) {
    return XFS_ERR(XfsError::kInvalidArgument, "no block device supplied");
  }
  auto sb_result = ReadSuperblock(*device, 0, options.verify_crc);
  if (!sb_result.ok()) return sb_result.error();
  XfsSuperblock sb = std::move(sb_result.value());

  Status mountable = CheckMountable(sb);
  if (!mountable.ok()) return mountable.error();

  XfsGeometry geo = MakeGeometry(sb);
  if (geo.dblocks * geo.block_size > device->Size()) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "filesystem larger than the device")
        .Expected(device->Size())
        .Actual(geo.dblocks * geo.block_size);
  }

  auto fs = std::unique_ptr<XfsFilesystem>(new XfsFilesystem());
  fs->superblock_ = std::move(sb);
  fs->geometry_ = std::move(geo);
  fs->options_ = options;
  fs->device_ = std::move(device);
  fs->writable_ = false;

  // Whitespace in the label: pad with spaces, not NULs (mkfs does this).
  XFS_SB_INFO("XFS v5 filesystem, uuid=" << FormatUuid(fs->superblock_.uuid));
  XFS_SB_INFO("blocksize=" << fs->geometry_.block_size
                           << " sectorsize=" << fs->geometry_.sector_size
                           << " inode size=" << fs->geometry_.inode_size);
  XFS_SB_INFO("agcount=" << fs->geometry_.agcount
                         << " agblocks=" << fs->geometry_.agblocks
                         << " dblocks=" << fs->geometry_.dblocks);
  XFS_SB_INFO("logstart=" << fs->geometry_.logstart
                          << " logblocks=" << fs->geometry_.logblocks);
  XFS_SB_INFO("root inode=" << fs->geometry_.rootino);

  if (fs->superblock_.features.ro_compat_unknown) {
    fs->warnings_.push_back(
        "superblock has unknown read-only compatible features (" +
        Hex(fs->superblock_.features.ro_compat_unknown) +
        "); read-only access proceeds");
  }
  if (fs->superblock_.features.compat_unknown) {
    fs->warnings_.push_back(
        "superblock has unknown compatible features (" +
        Hex(fs->superblock_.features.compat_unknown) + ")");
  }
  if (HasLogIncompatFeatures(fs->superblock_)) {
    fs->warnings_.push_back(
        "log-incompatible features are set; the log may contain records "
        "this build does not understand");
  }

  // Read the root inode before declaring success.
  auto root = fs->OpenInode(fs->geometry_.rootino);
  if (!root.ok()) return root.error();
  if (!root.value().IsDir()) {
    return XFS_ERR(XfsError::kCorruptInode, "root inode is not a directory")
        .Ino(fs->geometry_.rootino);
  }

  // Log state: scanned and validated with the recovery engine (RW-5). A
  // read-write mount replays a dirty log; a read-only mount refuses unless the
  // caller explicitly asked for the unsafe pre-recovery view.
  LogState log_state = LogState::kUnknown;
  bool log_checked = false;
  bool recovered = false;
  if (options.check_log) {
    auto scan = ScanLog(*fs->device_, fs->superblock_, fs->geometry_);
    if (!scan.ok()) {
      if (!options.force_unsafe_ro) return scan.error();
      fs->warnings_.push_back("log check failed: " + scan.error().message);
    } else {
      const LogScan& log = scan.value();
      log_checked = true;
      log_state = log.state;
      if (log.state == LogState::kClean || log.state == LogState::kEmpty) {
        XFS_SB_INFO("log is " << ToString(log.state) << " (" << log.detail
                              << ")");
        // An explicit recovery still has to finish the log: it adopts the
        // scanned position, runs the completion phases (the kernel processes
        // the AGI unlinked lists on every mount, clean log or not) and closes
        // the log with an unmount record when it does not have one. Without
        // this the close path would classify the log a second time, and a torn
        // record behind the newest good one reads as clean to the scanner but
        // not to the record-latest classifier.
        if (options.read_write && options.clean_log_after_recovery) {
          auto report = fs->RecoverScannedLog(log, RecoveryFinish::kCleanLog);
          if (!report.ok()) return report.error();
          recovered = true;
          fs->recovery_report_ = report.value();
          XFS_SB_INFO("clean log finished: unlinked="
                      << report.value().unlinked_inodes_processed);
        }
      } else {
        const bool can_recover = options.read_write &&
                                 options.recover_dirty_log;
        if (can_recover && !fs->device_->Info().writable) {
          return XFS_ERR(XfsError::kReadOnlyVolume,
                         "the log needs replay but the device was opened "
                         "read-only")
              .Metadata("log");
        }
        if (can_recover) {
          // Never recover a filesystem this build would then refuse to
          // modify: check the profile first.
          Status profile = CheckWritableProfile(
              fs->superblock_, fs->geometry_, options.recovery_only,
              options.allow_rmapbt_qualification, options.recovery_only,
              options.allow_reflink_qualification,
              options.allow_parent_qualification);
          if (!profile.ok()) {
            auto error = profile.error();
            error.message +=
                " (read-only access is unaffected; mount without the "
                "read-write option)";
            return error;
          }
          auto report = fs->RecoverScannedLog(
              log, options.clean_log_after_recovery
                       ? RecoveryFinish::kCleanLog
                       : RecoveryFinish::kMount);
          if (!report.ok()) return report.error();
          recovered = true;
          fs->recovery_report_ = report.value();
          XFS_SB_INFO("log recovered: transactions="
                      << report.value().replay.transactions_replayed
                      << " items=" << report.value().replay.items_replayed
                      << " buffers=" << report.value().replay.buffers_replayed
                      << " inodes=" << report.value().replay.inodes_replayed
                      << " intents=" << report.value().intents_completed
                      << " unlinked="
                      << report.value().unlinked_inodes_processed);
        } else if (!options.force_unsafe_ro) {
          std::string detail =
              "recoverable transactions=" + std::to_string(log.transactions.size());
          if (!log.unsupported_items.empty()) {
            detail += ", unsupported items=" +
                      std::to_string(log.unsupported_items.size()) + " (" +
                      log.unsupported_items.front() + ")";
          }
          return XFS_ERR(XfsError::kDirtyLog,
                         "log is not clean (" + log.detail + "; " + detail +
                             "); refusing to mount. Use --force-unsafe-ro to "
                             "mount read-only with the pre-recovery metadata, "
                             "or mount read-write (or run `xfswin recover`) to "
                             "replay the journal.")
              .Metadata("log");
        } else {
          fs->warnings_.push_back(
              "log is " + std::string(ToString(log.state)) + " (" +
              log.detail +
              "); mounting read-only with --force-unsafe-ro: metadata may "
              "reflect a pre-recovery state");
        }
      }
    }
  }

  // Read-write mounts are an allow-list: writable device, clean or recovered
  // log, and exactly the feature profile this build can modify. A refusal is
  // an error (never a silent read-only mount), so the caller decides.
  if (options.read_write) {
    if (!fs->device_->Info().writable) {
      return XFS_ERR(XfsError::kReadOnlyVolume,
                     "read-write mount requested but the device was opened "
                     "read-only")
          .Metadata("mount");
    }
    if (!log_checked) {
      return XFS_ERR(XfsError::kDirtyLog,
                     "read-write mounts require the log to be checked")
          .Metadata("log");
    }
    if (!recovered && log_state != LogState::kClean &&
        log_state != LogState::kEmpty) {
      return XFS_ERR(XfsError::kDirtyLog,
                     "read-write mounts require a clean or recoverable log "
                     "(recovery was disabled with recover_dirty_log=false)")
          .Metadata("log");
    }
    Status profile = CheckWritableProfile(
        fs->superblock_, fs->geometry_, options.recovery_only,
        options.allow_rmapbt_qualification, options.recovery_only,
        options.allow_reflink_qualification,
        options.allow_parent_qualification);
    if (!profile.ok()) {
      auto error = profile.error();
      error.message +=
          " (read-only access is unaffected; mount without the read-write "
          "option)";
      return error;
    }
    // A writable mount must never proceed on a namespace whose forward
    // dirents and reverse parent pointers already disagree.  Recovery has
    // finished at this point, so this is a clean preflight over committed
    // metadata, not a pre-replay snapshot.
    // Recovery-only mounts complete surviving intents first; they are not a
    // long-lived writable namespace and must be allowed to converge even when
    // the pre-recovery view was inconsistent.  The follow-up check is the
    // caller's gate before exposing such an image read-write.
    if (fs->superblock_.features.parent && !options.recovery_only) {
      auto audit = fs->VerifyParentPointers();
      if (!audit.ok() || !audit.value().clean()) {
        std::string detail =
            audit.ok() ? audit.value().Summary() : audit.error().Format();
        return XFS_ERR(XfsError::kNeedsRepair,
                       "parent-pointer namespace is inconsistent (" + detail +
                           "); run xfswin check, then repair or restore the "
                           "image")
            .Metadata("parent pointers");
      }
    }
    fs->writable_ = true;
    XFS_SB_INFO("mounted read-write: " << WritableProfileSummary(fs->superblock_));
    if (!recovered) {
      // A clean log can still carry zero-link inodes on the AGI unlinked
      // lists (a crash after the unlink transaction was flushed). Linux
      // processes them on every mount; so do we, before the volume is
      // visible (xlog_recover_process_iunlinks).
      uint64_t cleared = 0;
      auto processed = fs->ProcessAgiUnlinkedLists(&cleared);
      if (!processed.ok()) return processed.error();
      if (processed.value() != 0 || cleared != 0) {
        XFS_SB_INFO("unlinked inode recovery: freed="
                    << processed.value() << " cleared_buckets=" << cleared);
      }
    }
    if (fs->geometry_.features.reflink) {
      // xfs_reflink_recover_cow runs on every writable mount, including a
      // clean one.  A crash between CoW allocation and remap leaves COW
      // staging records that no inode references any more.
      auto cow = fs->RecoverCowStaging();
      if (!cow.ok()) {
        auto error = cow.error();
        error.message +=
            " (CoW staging recovery is required before a reflink mount)";
        return error;
      }
      if (fs->recovery_report_.has_value()) {
        fs->recovery_report_->cow_staging_freed = cow.value();
      }
      if (cow.value() != 0) {
        XFS_SB_INFO("CoW staging recovery: freed " << cow.value()
                                                   << " orphan extents");
      }
    }
    if (options.recovery_only) {
      // Recovery already completed (or the log was clean).  Do not leave a
      // recovery-only mount usable as a general writable filesystem.
      fs->writable_ = false;
    }
  }

  for (const auto& w : fs->warnings_) XFS_WARN(w);
  return fs;
}

}  // namespace xfs
