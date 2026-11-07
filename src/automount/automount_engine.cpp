// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/automount_engine.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

namespace xfs {
namespace {

std::string UuidKey(const Uuid& uuid) { return FormatUuid(uuid); }

std::string LocationText(const PartitionLocation& location) {
  std::string text = location.disk_path.empty()
                         ? "disk " + std::to_string(location.disk_number)
                         : location.disk_path;
  if (location.partition_number != 0) {
    text += " partition " + std::to_string(location.partition_number);
  } else {
    text += " (whole disk)";
  }
  if (!location.serial_number.empty()) {
    text += " serial " + location.serial_number;
  }
  if (!location.stable_id.empty()) {
    text += " id " + location.stable_id;
  }
  return text;
}

bool SamePhysicalIdentity(const PartitionLocation& mounted,
                          const PartitionLocation& current) {
  return mounted.stable_id == current.stable_id &&
         mounted.serial_number == current.serial_number &&
         mounted.disk_size_bytes == current.disk_size_bytes &&
         mounted.offset_bytes == current.offset_bytes &&
         mounted.length_bytes == current.length_bytes &&
         mounted.disk_number == current.disk_number &&
         mounted.partition_number == current.partition_number;
}

std::chrono::milliseconds RetryDelay(const AutomountSettings& settings,
                                    uint32_t attempt) {
  uint64_t delay = settings.initial_retry_ms;
  for (uint32_t i = 1; i < attempt; ++i) {
    if (delay >= settings.max_retry_ms) break;
    delay *= 2;
  }
  delay = std::min<uint64_t>(delay, settings.max_retry_ms);
  return std::chrono::milliseconds(delay);
}

void AddWarning(std::vector<std::string>* warnings, const std::string& text) {
  if (text.empty()) return;
  if (std::find(warnings->begin(), warnings->end(), text) == warnings->end()) {
    warnings->push_back(text);
  }
}

std::string CombineNotes(const std::string& first, const std::string& second) {
  if (first.empty()) return second;
  if (second.empty()) return first;
  return first + " " + second;
}

bool TakeoverWasOwned(const DriveLetterTakeover& takeover) {
  return takeover.state == TakeoverState::kOwned;
}

}  // namespace

const char* ToString(AutomountLogLevel level) {
  switch (level) {
    case AutomountLogLevel::kDebug: return "debug";
    case AutomountLogLevel::kInfo: return "info";
    case AutomountLogLevel::kWarn: return "warn";
    case AutomountLogLevel::kError: return "error";
  }
  return "info";
}

const char* ToString(AutomountEntryState state) {
  switch (state) {
    case AutomountEntryState::kDisabled: return "disabled";
    case AutomountEntryState::kWaitingForDevice: return "waiting-for-device";
    case AutomountEntryState::kRetrying: return "retrying";
    case AutomountEntryState::kMounted: return "mounted";
    case AutomountEntryState::kMountedElsewhere: return "mounted-elsewhere";
    case AutomountEntryState::kRefused: return "refused";
    case AutomountEntryState::kFailed: return "failed";
  }
  return "unknown";
}

AutomountEngine::AutomountEngine(AutomountPlatform& platform,
                                 AutomountSettings settings)
    : platform_(platform), settings_(std::move(settings)) {
  if (settings_.initial_retry_ms == 0) settings_.initial_retry_ms = 1;
  if (settings_.max_retry_ms < settings_.initial_retry_ms) {
    settings_.max_retry_ms = settings_.initial_retry_ms;
  }
  if (settings_.max_ready_retries == 0) settings_.max_ready_retries = 1;
  if (settings_.max_debounce_wait_ms == 0) settings_.max_debounce_wait_ms = 1;
}

AutomountEngine::~AutomountEngine() = default;

void AutomountEngine::SetConfig(AutomountConfig config) {
  config_ = std::move(config);
  logged_refusals_.clear();
  retries_.clear();
  exhausted_.clear();
}

void AutomountEngine::SetConfigLoader(AutomountConfigLoader loader) {
  config_loader_ = std::move(loader);
  has_config_loader_ = static_cast<bool>(config_loader_);
}

void AutomountEngine::ScheduleReconcile(AutomountTimePoint at) {
  if (!pending_reconcile_ || at < *pending_reconcile_) {
    pending_reconcile_ = at;
  }
}

void AutomountEngine::Start(AutomountTimePoint now) {
  if (running_) return;
  running_ = true;
  pending_reconcile_ = now;
  first_pending_event_.reset();
  next_periodic_.reset();
  Log(AutomountLogLevel::kInfo, "engine-start", "", "", "", "started",
      "initial discovery without waiting for a device notification");
}

AutomountStopReport AutomountEngine::Stop(AutomountTimePoint now) {
  AutomountStopReport report;
  if (!running_) return report;
  Log(AutomountLogLevel::kInfo, "engine-stop", "", "", "", "stopping",
      "unmounting every automount-owned session");
  for (auto& session : mounts_) {
    const Status stopped =
        StopSession(session, now, "automount service is stopping");
    if (!stopped.ok()) {
      report.failures.push_back(
          "session " + session.drive + " (" + UuidKey(session.fs_uuid) +
          ") did not unmount cleanly: " + stopped.error().Format());
    }
  }
  mounts_.clear();
  RetryStoppingSessions(now);
  for (const auto& stopping : stopping_) {
    report.failures.push_back(
        "session " + stopping.drive + " (" + stopping.uuid +
        ") is still stopping: " +
        (stopping.last_error.empty() ? std::string("unknown error")
                                     : stopping.last_error));
  }
  stopping_.clear();
  for (auto& pending : pending_restores_) pending.next_at = now;
  RetryPendingRestores(now);
  for (const auto& pending : pending_restores_) {
    if (pending.takeover.mount_point.empty()) continue;
    report.failures.push_back(
        "the original Windows mount point " + pending.takeover.mount_point +
        " could not be restored before the service stopped: " +
        pending.takeover.error);
    Log(AutomountLogLevel::kError, "drive-letter-restore", pending.uuid,
        pending.takeover.original_drive, "", "failed",
        "the original Windows mount point " + pending.takeover.mount_point +
            " could not be restored before the service stopped: " +
            pending.takeover.error);
  }
  pending_restores_.clear();
  retries_.clear();
  exhausted_.clear();
  pending_reconcile_.reset();
  first_pending_event_.reset();
  next_periodic_.reset();
  running_ = false;
  return report;
}

void AutomountEngine::NotifyDeviceChanged(AutomountTimePoint now,
                                          std::string hint) {
  Log(AutomountLogLevel::kDebug, "device-change", "", "", "", "notified", hint);
  if (!running_) return;
  // A device state change may make a previously missing device readable.
  retries_.clear();
  exhausted_.clear();
  const auto window = std::chrono::milliseconds(config_.debounce_ms);
  const auto candidate = now + window;
  if (!pending_reconcile_) {
    first_pending_event_ = now;
    pending_reconcile_ = candidate;
  } else {
    const auto cap = *first_pending_event_ +
                     std::chrono::milliseconds(settings_.max_debounce_wait_ms);
    pending_reconcile_ =
        std::max(*pending_reconcile_, std::min(candidate, cap));
  }
}

void AutomountEngine::NotifySettingsChanged(AutomountTimePoint now) {
  Log(AutomountLogLevel::kDebug, "config-change", "", "", "", "notified", "");
  if (!running_) return;
  settings_changed_ = true;
  pending_reconcile_ = now;
}

void AutomountEngine::Tick(AutomountTimePoint now) {
  if (!running_) return;
  bool due = settings_changed_;
  if (pending_reconcile_ && now >= *pending_reconcile_) due = true;
  if (next_periodic_ && now >= *next_periodic_) due = true;
  for (const auto& [key, retry] : retries_) {
    (void)key;
    if (now >= retry.next_at) {
      due = true;
      break;
    }
  }
  if (!due) return;

  const bool reload_config_now = settings_changed_;
  settings_changed_ = false;
  pending_reconcile_.reset();
  first_pending_event_.reset();
  Reconcile(now, reload_config_now);
}

std::optional<AutomountTimePoint> AutomountEngine::NextWake(
    AutomountTimePoint now) const {
  if (!running_) return std::nullopt;
  std::optional<AutomountTimePoint> earliest;
  auto consider = [&earliest](AutomountTimePoint at) {
    if (!earliest || at < *earliest) earliest = at;
  };
  if (pending_reconcile_) consider(*pending_reconcile_);
  if (next_periodic_) consider(*next_periodic_);
  for (const auto& [key, retry] : retries_) {
    (void)key;
    consider(retry.next_at);
  }
  for (const auto& pending : pending_restores_) {
    consider(pending.next_at);
  }
  if (earliest && *earliest < now) return now;
  return earliest;
}

void AutomountEngine::ReloadConfig(AutomountTimePoint now) {
  if (!has_config_loader_) return;
  auto loaded = config_loader_();
  if (!loaded.ok()) {
    const std::string text = loaded.error().Format();
    status_.config_error = text;
    if (text != last_config_error_) {
      Log(AutomountLogLevel::kError, "config-load", "", "", "", "error", text);
      last_config_error_ = text;
    }
    return;
  }
  last_config_error_.clear();
  status_.config_error.clear();
  const AutomountConfigLoad& load = loaded.value();
  std::string fingerprint = SerializeAutomountConfig(load.config);
  for (const auto& warning : load.warnings) {
    fingerprint += "\nwarning:" + warning;
  }
  if (fingerprint == last_config_fingerprint_) return;

  config_ = load.config;
  last_config_fingerprint_ = fingerprint;
  logged_refusals_.clear();
  retries_.clear();
  exhausted_.clear();
  Log(AutomountLogLevel::kInfo, "config-loaded", "", "", "", "loaded",
      std::to_string(config_.entries.size()) + " filesystem entries");
  for (const auto& warning : load.warnings) {
    Log(AutomountLogLevel::kWarn, "config-warning", "", "", "", "ignored",
        warning);
  }
  (void)now;
}

bool AutomountEngine::RetryDue(const Uuid& uuid, AutomountTimePoint now) const {
  const auto it = retries_.find(UuidKey(uuid));
  return it == retries_.end() || now >= it->second.next_at;
}

bool AutomountEngine::ScheduleRetry(const Uuid& uuid, AutomountTimePoint now,
                                    uint32_t* attempts_out) {
  const std::string key = UuidKey(uuid);
  if (exhausted_.find(key) != exhausted_.end()) {
    retries_.erase(key);
    return false;
  }
  RetryState& retry = retries_[key];
  ++retry.attempts;
  retry.next_at = now + RetryDelay(settings_, retry.attempts);
  if (attempts_out) *attempts_out = retry.attempts;
  if (retry.attempts >= settings_.max_ready_retries) exhausted_[key] = true;
  return true;
}

void AutomountEngine::PruneRetries() {
  for (auto it = retries_.begin(); it != retries_.end();) {
    Uuid uuid{};
    if (!ParseUuid(it->first, &uuid) || !config_.Find(uuid) ||
        !config_.Find(uuid)->enabled) {
      it = retries_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = exhausted_.begin(); it != exhausted_.end();) {
    Uuid uuid{};
    if (!ParseUuid(it->first, &uuid) || !config_.Find(uuid) ||
        !config_.Find(uuid)->enabled) {
      it = exhausted_.erase(it);
    } else {
      ++it;
    }
  }
  // The restored-letter hint is session-scoped memory for an entry that is
  // still enabled.  A removed or disabled entry must forget it, exactly like
  // its retry state.
  for (auto it = restored_drives_.begin(); it != restored_drives_.end();) {
    Uuid uuid{};
    if (!ParseUuid(it->first, &uuid) || !config_.Find(uuid) ||
        !config_.Find(uuid)->enabled) {
      it = restored_drives_.erase(it);
    } else {
      ++it;
    }
  }
}

void AutomountEngine::Reconcile(AutomountTimePoint now, bool reload_config) {
  (void)reload_config;  // the config source is consulted every pass
  status_ = AutomountStatus{};
  status_.last_reconcile = now;
  status_.last_reconcile_valid = true;

  auto make_mount_status = [](const MountedSession& session) {
    AutomountMountStatus mount;
    mount.fs_uuid = session.fs_uuid;
    mount.label = session.label;
    mount.drive = session.drive;
    mount.session_id = session.session_id;
    mount.serial_number = session.location.serial_number;
    mount.stable_id = session.location.stable_id;
    mount.device_size_bytes = session.location.disk_size_bytes;
    mount.filesystem_bytes = session.filesystem_bytes;
    mount.free_bytes = session.free_bytes;
    mount.mode = session.mode;
    mount.configured_mode = session.configured_mode;
    // The published session snapshot is what `xfswin status` shows as the
    // mount's Note.  It has to include the drive-letter explanation (taken
    // over / preferred letter occupied), not only the read-only fallback.
    mount.mode_note = CombineNotes(session.drive_note, session.mode_note);
    mount.read_write = session.mode == AutomountMode::kReadWrite;
    mount.drive_taken_over = TakeoverWasOwned(session.takeover);
    mount.original_windows_drive = session.takeover.original_drive;
    mount.device_path = session.location.disk_path;
    return mount;
  };

  // A missing/changed configuration is picked up on every pass.  The service
  // additionally calls NotifySettingsChanged for immediate application.
  ReloadConfig(now);
  PruneRetries();
  RetryStoppingSessions(now);
  RetryPendingRestores(now);

  const auto period =
      std::chrono::seconds(std::max<uint32_t>(1, config_.reconcile_seconds));
  next_periodic_ = now + period;

  std::string discovery_error;
  auto discovered = platform_.Discover(settings_.discovery);
  if (!discovered.ok()) {
    status_.last_reconcile_valid = false;
    status_.discovery_incomplete = true;
    const std::string text = discovered.error().Format();
    AddWarning(&status_.warnings, text);
    Log(AutomountLogLevel::kError, "discovery", "", "", "", "failed", text);

    // Discovery failed, but a dead session is still ours to clean up.
    for (auto it = mounts_.begin(); it != mounts_.end();) {
      if (!platform_.IsSessionAlive(it->session_id)) {
        MountedSession dead = *it;
        it = mounts_.erase(it);
        Log(AutomountLogLevel::kWarn, "session-failed",
            UuidKey(dead.fs_uuid), dead.drive, LocationText(dead.location),
            "terminal", "the mount session is no longer running");
        StopSession(dead, now, "mount session is no longer running");
        ScheduleRetry(dead.fs_uuid, now, nullptr);
      } else {
        ++it;
      }
    }

    for (const auto& session : mounts_) {
      status_.mounts.push_back(make_mount_status(session));
    }
    for (const auto& entry : config_.entries) {
      AutomountEntryStatus es;
      es.fs_uuid = entry.fs_uuid;
      es.enabled = entry.enabled;
      es.mode = entry.mode;
      es.effective_mode = entry.mode;
      es.preferred_drive = entry.preferred_drive;
      const MountedSession* mounted = nullptr;
      for (const auto& current : mounts_) {
        if (UuidEqual(current.fs_uuid, entry.fs_uuid)) {
          mounted = &current;
          break;
        }
      }
      if (!entry.enabled) {
        es.state = AutomountEntryState::kDisabled;
      } else if (mounted) {
        es.state = AutomountEntryState::kMounted;
        es.drive = mounted->drive;
        es.label = mounted->label;
        es.effective_mode = mounted->mode;
        es.mode_note = mounted->mode_note;
        es.drive_taken_over = TakeoverWasOwned(mounted->takeover);
        es.original_windows_drive = mounted->takeover.original_drive;
        es.detail = CombineNotes(mounted->drive_note, mounted->mode_note);
      } else {
        es.state = AutomountEntryState::kWaitingForDevice;
        es.detail = "device discovery is unavailable";
      }
      status_.entries.push_back(std::move(es));
    }
    Log(AutomountLogLevel::kDebug, "reconcile", "", "", "", "completed",
        "periodic reconciliation scheduled");
    return;
  }

  const DiscoveryReport& report = discovered.value();
  status_.discovered = report.filesystems;
  status_.rejected = report.rejected;
  status_.warnings = report.warnings;
  status_.discovery_incomplete = report.incomplete;
  status_.discovery_retryable_failures = report.retryable_failures;

  std::map<std::string, std::vector<const DiscoveredFilesystem*>> by_uuid;
  for (const auto& filesystem : report.filesystems) {
    if (filesystem.probe.state != XfsProbeState::kValid) continue;
    by_uuid[UuidKey(filesystem.probe.superblock.uuid)].push_back(&filesystem);
  }

  // 1. Reconcile the sessions this engine owns.  Each is independent: one
  //    failing/removed device must not disturb another mount.
  std::vector<MountedSession> survivors;
  survivors.reserve(mounts_.size());
  for (auto& session : mounts_) {
    bool drop = false;
    std::string reason;
    if (!platform_.IsSessionAlive(session.session_id)) {
      drop = true;
      reason = "mount session is no longer running";
      Log(AutomountLogLevel::kWarn, "session-failed",
          UuidKey(session.fs_uuid), session.drive,
          LocationText(session.location), "terminal", reason);
      ScheduleRetry(session.fs_uuid, now, nullptr);
    } else {
      const AutomountEntry* entry = config_.Find(session.fs_uuid);
      if (!entry) {
        drop = true;
        reason = "the automount entry was removed";
      } else if (!entry->enabled) {
        drop = true;
        reason = "automatic mounting was disabled";
      } else if (entry->mode != session.configured_mode) {
        drop = true;
        reason = "the configured mount mode changed";
      } else if (entry->preferred_drive != session.preferred_drive) {
        drop = true;
        reason = "the configured drive letter changed";
      } else {
        bool still_present = false;
        const auto it = by_uuid.find(UuidKey(session.fs_uuid));
        if (it != by_uuid.end()) {
          for (const DiscoveredFilesystem* filesystem : it->second) {
            if (SamePhysicalIdentity(session.location,
                                      filesystem->location)) {
              still_present = true;
              break;
            }
          }
        }
        if (!still_present) {
          // A read-write session holds its raw disk exclusively, so discovery
          // cannot open that disk and reports an incomplete pass instead of
          // its filesystem.  That is not evidence that the device was
          // removed: dropping the session here would unmount and remount it
          // in a loop (and the remount can race the closing handle).  A
          // different device that reused the old disk number while also held
          // exclusively cannot be told apart without opening the disk; keeping
          // the session until the disk becomes readable is the safer failure
          // mode (a stale session with failed I/O, not a dropped live mount).
          const auto& unreadable_disks = discovered.value().incomplete_disks;
          if (std::find(unreadable_disks.begin(), unreadable_disks.end(),
                        session.location.disk_number) !=
              unreadable_disks.end()) {
            still_present = true;
            AddWarning(&status_.warnings,
                       "disk " +
                           std::to_string(session.location.disk_number) +
                           " could not be read while a mount is active; "
                           "keeping the live session");
          }
        }
        if (!still_present) {
          drop = true;
          reason = "the device was removed or re-enumerated";
        }
      }
    }
    if (drop) {
      StopSession(session, now, reason);
    } else {
      survivors.push_back(std::move(session));
    }
  }
  mounts_ = std::move(survivors);

  for (const auto& session : mounts_) {
    status_.mounts.push_back(make_mount_status(session));
  }

  // 2. Build a drive-letter view that includes Windows' mappings, our live
  //    sessions and any session that is still shutting down.
  DriveLetterUsage usage;
  bool usage_ready = true;
  std::string letter_error;
  const auto letters = platform_.ListDriveLetters(&letter_error);
  if (!letter_error.empty()) {
    usage_ready = false;
    AddWarning(&status_.warnings,
               "drive letters could not be enumerated: " + letter_error);
    Log(AutomountLogLevel::kError, "drive-letters", "", "", "", "failed",
        letter_error);
  }
  for (const auto& letter : letters) {
    if (letter.letter >= 'A' && letter.letter <= 'Z') {
      const size_t index = static_cast<size_t>(letter.letter - 'A');
      usage.used[index] = true;
      usage.kinds[index] = letter.kind;
      usage.targets[index] = letter.target;
    }
  }
  auto mark_used = [&usage](const std::string& drive) {
    if (drive.size() == 2 && drive[0] >= 'A' && drive[0] <= 'Z') {
      usage.used[static_cast<size_t>(drive[0] - 'A')] = true;
    }
  };
  for (const auto& session : mounts_) mark_used(session.drive);
  for (const auto& stopping : stopping_) mark_used(stopping.drive);

  // 3. Apply the configuration entry by entry.
  for (const auto& entry : config_.entries) {
    AutomountEntryStatus es;
    es.fs_uuid = entry.fs_uuid;
    es.enabled = entry.enabled;
    es.mode = entry.mode;
    es.effective_mode = entry.mode;
    es.preferred_drive = entry.preferred_drive;
    const std::string key = UuidKey(entry.fs_uuid);

    if (!entry.enabled) {
      es.state = AutomountEntryState::kDisabled;
      es.detail = "automatic mounting is disabled";
      status_.entries.push_back(std::move(es));
      continue;
    }

    const MountedSession* mounted = nullptr;
    for (const auto& session : mounts_) {
      if (UuidEqual(session.fs_uuid, entry.fs_uuid)) {
        mounted = &session;
        break;
      }
    }
    if (mounted) {
      es.state = AutomountEntryState::kMounted;
      es.drive = mounted->drive;
      es.label = mounted->label;
      es.effective_mode = mounted->mode;
      es.mode_note = mounted->mode_note;
      es.drive_taken_over = TakeoverWasOwned(mounted->takeover);
      es.original_windows_drive = mounted->takeover.original_drive;
      es.detail = CombineNotes(mounted->drive_note, mounted->mode_note);
      status_.entries.push_back(std::move(es));
      continue;
    }

    // Never start a second session for a UUID while the previous session is
    // still shutting down and has not released its device/drive letter.
    const bool stopping_previous = [&]() {
      for (const auto& stopping : stopping_) {
        if (stopping.uuid == key) return true;
      }
      return false;
    }();
    if (stopping_previous) {
      es.state = AutomountEntryState::kRetrying;
      es.detail = "waiting for the previous mount session to stop";
      status_.entries.push_back(std::move(es));
      continue;
    }

    // A failed restoration left the original Windows mount point detached.
    // Starting another WinFsp mount for the same filesystem would take a
    // second letter while the first restore is still outstanding, so
    // automatic mounting stays suspended until the restore succeeds.  The
    // retry itself runs at the top of every reconciliation.
    const bool has_pending_restore = [&]() {
      for (const auto& pending : pending_restores_) {
        if (pending.uuid == key) return true;
      }
      return false;
    }();
    if (has_pending_restore) {
      es.state = AutomountEntryState::kFailed;
      es.detail =
          "the original Windows drive letter could not be restored yet; "
          "automatic mounting is suspended until it is";
      status_.entries.push_back(std::move(es));
      continue;
    }

    const auto candidates_it = by_uuid.find(key);
    if (candidates_it == by_uuid.end() || candidates_it->second.empty()) {
      if (report.incomplete && report.retryable_failures > 0) {
        uint32_t attempts = 0;
        if (RetryDue(entry.fs_uuid, now)) {
          if (ScheduleRetry(entry.fs_uuid, now, &attempts)) {
            es.state = AutomountEntryState::kRetrying;
            es.retry_attempt = attempts;
            es.detail = "the device is not ready yet; retrying";
            Log(AutomountLogLevel::kWarn, "retry", key, "", "",
                "scheduled",
                "the device was not readable; attempt " +
                    std::to_string(attempts));
          } else {
            retries_.erase(key);
            es.state = AutomountEntryState::kFailed;
            es.detail =
                "the device did not become ready after the bounded retries";
            LogEntryRefusal(entry, "device-not-ready", "failed", es.detail);
          }
        } else {
          es.state = AutomountEntryState::kRetrying;
          const auto it = retries_.find(key);
          es.retry_attempt = it == retries_.end() ? 0 : it->second.attempts;
          es.detail = "waiting for the device to become ready";
        }
      } else {
        retries_.erase(key);
        exhausted_.erase(key);
        logged_refusals_.erase(key);
        es.state = AutomountEntryState::kWaitingForDevice;
        es.detail = "no matching XFS filesystem is attached";
      }
      status_.entries.push_back(std::move(es));
      continue;
    }

    std::string select_detail;
    const DiscoveredFilesystem* filesystem =
        SelectFilesystem(entry, candidates_it->second, &select_detail);
    if (!filesystem) {
      es.state = AutomountEntryState::kRefused;
      es.detail = select_detail;
      LogEntryRefusal(entry, "ambiguous:" + select_detail, "refused",
                      select_detail);
      status_.entries.push_back(std::move(es));
      continue;
    }
    es.label = filesystem->probe.label;

    if (filesystem->xfswin_mounted && !filesystem->xfswin_drive.empty()) {
      es.state = AutomountEntryState::kMountedElsewhere;
      es.drive = filesystem->xfswin_drive;
      es.detail = "already mounted by another xfswin instance";
      status_.entries.push_back(std::move(es));
      continue;
    }

    std::string mode_note;
    const AutomountMode effective_mode =
        EffectiveMode(entry, *filesystem, &mode_note);

    // A read-only entry with pins is a strict selection: a mismatch means the
    // attached device is not the one that was enrolled.  A read-write entry
    // instead falls back to read-only: writing to an unexpected device is
    // never acceptable, but hiding the data is not necessary either.  Policy
    // refusals below (journal, system/pagefile, fixed disk, unsupported
    // profile) remain absolute and are not softened by the fallback.
    std::string mismatch;
    if (!EntryIdentityMatches(entry, filesystem->location, &mismatch) &&
        entry.mode != AutomountMode::kReadWrite) {
      es.state = AutomountEntryState::kRefused;
      es.detail = mismatch;
      LogEntryRefusal(entry, "identity:" + mismatch, "refused", mismatch);
      status_.entries.push_back(std::move(es));
      continue;
    }

    std::string refusal;
    if (!EntryPolicyAccepts(entry, *filesystem, effective_mode, &refusal)) {
      es.state = AutomountEntryState::kRefused;
      es.detail = refusal;
      LogEntryRefusal(entry, "policy:" + refusal, "refused", refusal);
      status_.entries.push_back(std::move(es));
      continue;
    }
    es.effective_mode = effective_mode;
    es.mode_note = mode_note;

    if (!RetryDue(entry.fs_uuid, now)) {
      const auto it = retries_.find(key);
      es.state = AutomountEntryState::kRetrying;
      es.retry_attempt = it == retries_.end() ? 0 : it->second.attempts;
      es.detail = "waiting before the next mount attempt";
      status_.entries.push_back(std::move(es));
      continue;
    }

    DriveLetterChoice selection;
    std::string allocation_error;
    if (!ChooseDrive(*filesystem, entry, usage, usage_ready, &selection,
                     &allocation_error)) {
      es.state = AutomountEntryState::kFailed;
      es.detail = allocation_error;
      LogEntryRefusal(entry, "drive:" + allocation_error, "failed",
                      allocation_error);
      status_.entries.push_back(std::move(es));
      continue;
    }

    Log(AutomountLogLevel::kInfo, "mount-attempt", key, selection.drive,
        LocationText(filesystem->location), "starting",
        std::string("mode ") + ToString(effective_mode) +
            (mode_note.empty() ? "" : "; " + mode_note));

    // The takeover detaches the Windows mount point of the RAW volume, if any,
    // before the WinFsp mount.  It is a no-op (kNone) for a volume Windows has
    // not mounted.  The platform re-verifies the volume identity itself; the
    // engine never authorizes a takeover from a drive letter alone.
    TakeoverResult takeover =
        platform_.TakeOverDriveLetter(*filesystem, selection);
    if (!takeover.status.ok()) {
      if (TakeoverNeedsRestore(takeover.takeover)) {
        RestoreTakeover(&takeover.takeover, key, now,
                        "the drive-letter takeover failed");
      }
      es.state = AutomountEntryState::kFailed;
      es.detail = "the Windows drive letter could not be taken over: " +
                  takeover.status.error().Format();
      LogEntryRefusal(entry, "drive-takeover:" + es.detail, "failed",
                      es.detail);
      status_.entries.push_back(std::move(es));
      continue;
    }
    MarkTakeoverMounting(&takeover.takeover);
    if (TakeoverNeedsRestore(takeover.takeover)) {
      Log(AutomountLogLevel::kInfo, "drive-letter-takeover", key,
          takeover.takeover.original_drive,
          LocationText(filesystem->location), "taken-over",
          "released the Windows mount point " +
              takeover.takeover.mount_point + " of volume " +
              takeover.takeover.volume_name + " before mounting at " +
              selection.drive);
    }

    AutomountMountStart started = platform_.StartMount(
        *filesystem, entry, effective_mode, selection.drive);
    if (started.status.ok() && !started.session_id.empty()) {
      MarkTakeoverOwned(&takeover.takeover);
      MountedSession session;
      session.fs_uuid = entry.fs_uuid;
      session.label = filesystem->probe.label;
      session.drive = selection.drive;
      session.session_id = started.session_id;
      session.preferred_drive = entry.preferred_drive;
      session.configured_mode = entry.mode;
      session.mode = effective_mode;
      session.mode_note = mode_note;
      session.location = filesystem->location;
      session.filesystem_bytes = filesystem->probe.filesystem_bytes;
      session.free_bytes = filesystem->probe.free_bytes;
      session.takeover = takeover.takeover;
      if (selection.preferred_unavailable) {
        session.drive_note = "The configured drive letter " +
                             entry.preferred_drive +
                             " was already in use; mounted as " +
                             selection.drive + " instead.";
      }
      if (!selection.note.empty() &&
          !selection.preferred_unavailable) {
        session.drive_note = CombineNotes(session.drive_note, selection.note);
      }
      mounts_.push_back(session);
      retries_.erase(key);
      exhausted_.erase(key);
      // The session owns the letter now; the restore path records a fresh
      // hint when it ends.
      restored_drives_.erase(key);
      logged_refusals_.erase(key);

      es.state = AutomountEntryState::kMounted;
      es.drive = session.drive;
      es.effective_mode = session.mode;
      es.mode_note = session.mode_note;
      es.drive_taken_over = TakeoverWasOwned(session.takeover);
      es.original_windows_drive = session.takeover.original_drive;
      es.detail = CombineNotes(session.drive_note, session.mode_note);
      status_.mounts.push_back(make_mount_status(session));

      std::string detail = std::string("mounted ") + ToString(session.mode);
      if (TakeoverWasOwned(session.takeover)) {
        detail += "; took over the Windows drive letter " +
                  session.takeover.original_drive;
      }
      if (!session.drive_note.empty()) detail += "; " + session.drive_note;
      if (!session.mode_note.empty()) detail += "; " + session.mode_note;
      Log(AutomountLogLevel::kInfo, "mount-result", key, session.drive,
          LocationText(session.location), "mounted", detail);
      status_.entries.push_back(std::move(es));
      continue;
    }

    // The WinFsp mount did not produce a session.  Stop a partially created
    // one, then restore the original Windows mount point before deciding
    // whether a retry is allowed: mounting another letter while Windows still
    // has no mount point on the volume would leave an unusable RAW volume.
    const std::string failure = started.status.ok()
                                    ? "the mount session started without an id"
                                    : started.status.error().Format();
    if (!started.session_id.empty()) {
      Status stopped = platform_.StopMount(started.session_id);
      if (!stopped.ok()) {
        Log(AutomountLogLevel::kWarn, "mount-failed", key, selection.drive,
            LocationText(filesystem->location), "stop-pending",
            "the partially started mount session did not stop: " +
                stopped.error().Format());
      }
    }
    if (TakeoverNeedsRestore(takeover.takeover)) {
      RestoreTakeover(&takeover.takeover, key, now,
                      "the WinFsp mount failed");
    }
    const bool restore_pending = TakeoverNeedsRestore(takeover.takeover);

    uint32_t attempts = 0;
    if (started.retryable && !restore_pending &&
        ScheduleRetry(entry.fs_uuid, now, &attempts)) {
      es.state = AutomountEntryState::kRetrying;
      es.retry_attempt = attempts;
      es.detail = "the device is not ready yet; retrying";
      Log(AutomountLogLevel::kWarn, "mount-retry", key, selection.drive,
          LocationText(filesystem->location), "retry",
          failure + " (attempt " + std::to_string(attempts) + ")");
    } else {
      retries_.erase(key);
      if (restore_pending) {
        es.state = AutomountEntryState::kFailed;
        es.detail = "the original Windows drive letter could not be restored "
                    "yet; automatic mounting is suspended until it is";
        LogEntryRefusal(entry, "restore-pending:" + takeover.takeover.error,
                        "failed", es.detail);
      } else if (started.retryable) {
        es.state = AutomountEntryState::kFailed;
        es.detail =
            "the device did not become ready after the bounded retries";
        LogEntryRefusal(entry, "mount:" + failure, "failed", es.detail);
      } else {
        es.state = AutomountEntryState::kFailed;
        es.detail = failure;
        LogEntryRefusal(entry, "mount:" + failure, "failed", es.detail);
      }
    }
    status_.entries.push_back(std::move(es));
  }

  Log(AutomountLogLevel::kDebug, "reconcile", "", "", "", "completed",
      "periodic reconciliation scheduled");
}

bool AutomountEngine::TakeoverEligible(
    const DiscoveredFilesystem& filesystem) const {
  // The platform repeats every one of these checks against live state.  Here
  // they only decide whether the Windows drive letter is offered to the
  // allocator as take-over-able instead of "occupied".
  if (!filesystem.windows_volume_found) return false;
  if (!filesystem.volume_identity_exact) return false;
  if (!filesystem.volume_unrecognized_raw) return false;
  if (filesystem.volume_device_path.empty()) return false;
  // More than one drive letter on the volume: leave the configuration alone
  // and allocate a free letter instead of guessing which RAW root to detach.
  return !SingleDriveLetterMountPoint(filesystem.mount_points).empty();
}

bool AutomountEngine::ChooseDrive(const DiscoveredFilesystem& filesystem,
                                  const AutomountEntry& entry,
                                  DriveLetterUsage& usage, bool usage_ready,
                                  DriveLetterChoice* choice,
                                  std::string* error) {
  if (!usage_ready) {
    if (error) *error = "drive letters could not be enumerated";
    return false;
  }
  std::string existing_same_volume =
      TakeoverEligible(filesystem)
          ? SingleDriveLetterMountPoint(filesystem.mount_points)
          : std::string();
  // A pass that runs right after a failed mount restored the volume's Windows
  // mount point can see the Mount Manager's path list before it lists that
  // letter again.  The restored hint re-establishes the same-volume letter,
  // but only after checking live that the letter still resolves to the exact
  // volume GUID that was verified for it.  The takeover repeats every identity
  // check against live state before removing anything.
  std::string existing_live_volume;
  std::string restored_drive;
  const auto restored_it = restored_drives_.find(UuidKey(entry.fs_uuid));
  if (restored_it != restored_drives_.end()) {
    const RestoredDriveHint hint = restored_it->second;
    const bool letter_in_use =
        hint.drive.size() == 2 && usage.IsUsed(hint.drive[0]);
    if (!letter_in_use ||
        !platform_.DriveLetterResolvesToVolume(hint.drive,
                                               hint.volume_device_path)) {
      // The letter was released, or it now belongs to a different volume.
      // The hint is stale and must not influence the allocation.
      restored_drives_.erase(restored_it);
    } else if (existing_same_volume.empty()) {
      existing_same_volume = hint.drive;
      existing_live_volume = hint.volume_device_path;
      restored_drive = hint.drive;
    } else if (existing_same_volume != hint.drive) {
      // The volume now has two letters.  Never guess which one to detach: drop
      // the hint and let the live verification refuse the multiple letters.
      restored_drives_.erase(restored_it);
    }
  }
  std::string remembered;
  const auto remembered_it = remembered_drives_.find(UuidKey(entry.fs_uuid));
  if (remembered_it != remembered_drives_.end()) {
    remembered = remembered_it->second;
  }
  auto selected = ChooseDriveLetter(entry.preferred_drive, usage,
                                    existing_same_volume, remembered);
  if (!selected.ok()) {
    if (error) {
      *error = selected.error().code == XfsError::kNoSpace
                   ? "no free drive letter is available"
                   : selected.error().message;
    }
    return false;
  }
  if (choice) *choice = std::move(selected.value());
  if (choice && !restored_drive.empty() &&
      choice->release_drive == restored_drive) {
    // The chosen release letter comes from the hint, not from this pass's
    // discovery snapshot; tell the platform which volume it was verified
    // against so it can repeat the identity check live.
    choice->release_volume_device_path = existing_live_volume;
  }
  if (choice && choice->drive.size() == 2) {
    usage.used[static_cast<size_t>(choice->drive[0] - 'A')] = true;
  }
  return true;
}

AutomountMode AutomountEngine::EffectiveMode(
    const AutomountEntry& entry, const DiscoveredFilesystem& filesystem,
    std::string* note) const {
  if (entry.mode != AutomountMode::kReadWrite) {
    return AutomountMode::kReadOnly;
  }
  std::string mismatch;
  if (EntryIdentityMatches(entry, filesystem.location, &mismatch)) {
    return AutomountMode::kReadWrite;
  }
  if (note) {
    *note = "The configured read-write identity does not match this device (" +
            mismatch + "); mounted read-only.";
  }
  return AutomountMode::kReadOnly;
}

const DiscoveredFilesystem* AutomountEngine::SelectFilesystem(
    const AutomountEntry& entry,
    const std::vector<const DiscoveredFilesystem*>& candidates,
    std::string* detail) const {
  if (candidates.size() == 1) return candidates.front();

  std::vector<const DiscoveredFilesystem*> matching;
  for (const DiscoveredFilesystem* candidate : candidates) {
    std::string mismatch;
    if (EntryIdentityMatches(entry, candidate->location, &mismatch)) {
      matching.push_back(candidate);
    }
  }
  if (matching.size() == 1) return matching.front();
  if (detail) {
    if (matching.empty()) {
      *detail =
          "multiple attached filesystems share this XFS UUID and none "
          "matches the configured device identity";
    } else {
      *detail =
          "multiple attached filesystems share this XFS UUID; refusing to "
          "guess which one to mount";
    }
  }
  return nullptr;
}

bool AutomountEngine::EntryIdentityMatches(
    const AutomountEntry& entry, const PartitionLocation& location,
    std::string* mismatch) const {
  if (!entry.expected_serial.empty() &&
      location.serial_number != entry.expected_serial) {
    if (mismatch) {
      *mismatch = "the attached device's serial number does not match the "
                  "expected value";
    }
    return false;
  }
  if (!entry.expected_stable_id.empty() &&
      location.stable_id != entry.expected_stable_id) {
    if (mismatch) {
      *mismatch = "the attached device's stable id does not match the "
                  "expected value";
    }
    return false;
  }
  if (entry.expected_size_bytes != 0 &&
      location.disk_size_bytes != entry.expected_size_bytes) {
    if (mismatch) {
      *mismatch = "the attached device's size does not match the expected "
                  "value";
    }
    return false;
  }
  if (entry.expected_device_class != DeviceClass::kUnknown &&
      location.device_class != entry.expected_device_class) {
    if (mismatch) {
      *mismatch = "the attached device's class does not match the expected "
                  "value";
    }
    return false;
  }
  return true;
}

bool AutomountEngine::EntryPolicyAccepts(
    const AutomountEntry& entry, const DiscoveredFilesystem& filesystem,
    AutomountMode effective_mode, std::string* refusal) const {
  auto refuse = [refusal](const std::string& message) {
    if (refusal) *refusal = message;
    return false;
  };

  Status valid = ValidateAutomountEntry(entry);
  if (!valid.ok()) return refuse(valid.error().message);

  if (settings_.discovery.probe_journal) {
    if (filesystem.probe.log_state == LogState::kDirty) {
      return refuse(
          "the XFS journal is dirty; automatic mounting is refused until the "
          "filesystem has been recovered");
    }
    if (filesystem.probe.log_state == LogState::kUnknown) {
      return refuse(
          "the XFS journal state could not be confirmed; automatic mounting "
          "is refused");
    }
  }

  if (entry.mode != AutomountMode::kReadWrite) {
    return true;
  }
  if (effective_mode != AutomountMode::kReadWrite) {
    // The identity pins did not match, so the entry runs read-only.  The
    // read-only policy has no class, system/pagefile or writable-profile
    // requirements beyond the journal and XFS verification already checked.
    return true;
  }

  // A read-write open of the raw device locks and dismounts every volume on
  // the physical disk.  Refuse when a different volume on the same disk has a
  // Windows filesystem: that volume must never be disturbed by this mount.
  if (filesystem.disk_has_other_filesystems) {
    return refuse(
        "another volume on the same physical disk has a Windows filesystem; "
        "a read-write mount would dismount it");
  }

  switch (filesystem.location.device_class) {
    case DeviceClass::kFixedDisk:
      return refuse(
          "fixed physical disks are not qualified for read-write mounting");
    case DeviceClass::kRemovableDisk:
      if (!entry.allow_removable_disk) {
        return refuse(
            "read-write mounting of a removable disk requires the explicit "
            "removable-disk qualification opt-in");
      }
      break;
    case DeviceClass::kVirtualDisk:
      break;
    default:
      return refuse(
          "read-write mounting is not qualified for this device class");
  }
  if (filesystem.location.system_disk) {
    return refuse("the system disk is never mounted read-write");
  }
  if (filesystem.location.pagefile_disk) {
    return refuse("a disk holding the page file is never mounted read-write");
  }
  if (filesystem.location.read_only) {
    return refuse("the device reports read-only media");
  }
  if (settings_.discovery.probe_writable_profile &&
      !filesystem.probe.writable_profile) {
    std::string message =
        "the filesystem feature profile is not supported for writing";
    if (!filesystem.probe.writable_profile_detail.empty()) {
      message += ": " + filesystem.probe.writable_profile_detail;
    }
    return refuse(message);
  }
  return true;
}

Status AutomountEngine::StopSession(MountedSession& session,
                                    AutomountTimePoint now,
                                    const std::string& reason) {
  const std::string uuid = UuidKey(session.fs_uuid);
  Log(AutomountLogLevel::kInfo, "unmount", uuid,
      session.drive, LocationText(session.location), "stopping", reason);
  Status stopped = platform_.StopMount(session.session_id);
  if (!stopped.ok()) {
    Log(AutomountLogLevel::kWarn, "unmount", uuid,
        session.drive, LocationText(session.location), "pending",
        stopped.error().Format());
    StoppingSession stopping;
    stopping.session_id = session.session_id;
    stopping.drive = session.drive;
    stopping.uuid = uuid;
    stopping.last_error = stopped.error().Format();
    stopping.attempts = 1;
    stopping.takeover = session.takeover;
    stopping_.push_back(std::move(stopping));
    // A session that did not stop immediately still owns its drive letter.
    // Ask the loop to retry shortly instead of waiting for the next periodic
    // reconciliation.  The original Windows mount point is restored only
    // after the WinFsp letter is free.
    ScheduleReconcile(now + std::chrono::seconds(1));
    return stopped;
  }
  Log(AutomountLogLevel::kInfo, "unmount", uuid,
      session.drive, LocationText(session.location), "unmounted", reason);
  // Restore only after the WinFsp session is gone: SetVolumeMountPointW must
  // not race a WinFsp mount that still defines the same letter.
  RestoreTakeover(&session.takeover, uuid, now, reason);
  return OkStatus();
}

void AutomountEngine::RestoreTakeover(DriveLetterTakeover* takeover,
                                      const std::string& uuid,
                                      AutomountTimePoint now,
                                      const std::string& reason) {
  if (!takeover || !TakeoverNeedsRestore(*takeover)) return;
  const std::string original = takeover->original_drive;
  Status restored = platform_.RestoreDriveLetterTakeover(takeover);
  if (restored.ok()) {
    if (!takeover->volume_present && !takeover->drive.empty()) {
      // The volume disappeared while the session owned the letter.  Windows
      // will not recreate the deleted mount point, so remember the letter for
      // this filesystem and reuse it when the device is inserted again.
      remembered_drives_[uuid] = takeover->drive;
    }
    if (takeover->volume_present && !takeover->original_drive.empty() &&
        !takeover->volume_device_path.empty()) {
      // The volume has its Windows letter back.  A discovery pass triggered by
      // this restore can run before the Mount Manager refreshes the volume's
      // path list; remember the verified letter so that pass still takes it
      // over instead of allocating an unrelated free letter.
      restored_drives_[uuid] = RestoredDriveHint{takeover->original_drive,
                                                 takeover->volume_device_path};
    }
    std::string detail;
    if (takeover->volume_present) {
      detail = "restored the original Windows mount point " +
               takeover->mount_point + " of volume " + takeover->volume_name;
    } else {
      detail = "the volume was removed; the original Windows mount point " +
               takeover->mount_point +
               " was not re-created (the letter stays free instead of being "
               "bound to a dead volume)";
    }
    Log(AutomountLogLevel::kInfo, "drive-letter-restore", uuid, original, "",
        takeover->volume_present ? "restored" : "skipped-removed",
        detail + " (" + reason + ")");
    return;
  }
  PendingRestore pending;
  pending.takeover = *takeover;
  pending.uuid = uuid;
  pending.reason = reason;
  pending.attempts = 1;
  pending.next_at =
      now + std::chrono::seconds(std::max<uint32_t>(5,
                                                    config_.reconcile_seconds));
  pending_restores_.push_back(std::move(pending));
  Log(AutomountLogLevel::kError, "drive-letter-restore", uuid, original, "",
      "restore-pending",
      "the original Windows mount point " + takeover->mount_point +
          " could not be restored: " + restored.error().Format() +
          "; retrying during reconciliation (" + reason + ")");
  ScheduleReconcile(pending_restores_.back().next_at);
}

void AutomountEngine::RetryPendingRestores(AutomountTimePoint now) {
  if (pending_restores_.empty()) return;
  const auto period = std::chrono::seconds(
      std::max<uint32_t>(5, config_.reconcile_seconds));
  for (auto it = pending_restores_.begin(); it != pending_restores_.end();) {
    if (now < it->next_at) {
      ++it;
      continue;
    }
    Status restored = platform_.RestoreDriveLetterTakeover(&it->takeover);
    if (restored.ok()) {
      if (!it->takeover.volume_present && !it->takeover.drive.empty()) {
        // The device disappeared before the deferred restore succeeded, so
        // the deleted Windows mount point cannot be re-created.  Remember the
        // letter exactly as the immediate restore path does; otherwise the
        // reinserted device would land on an unrelated free letter.
        remembered_drives_[it->uuid] = it->takeover.drive;
      }
      if (it->takeover.volume_present && !it->takeover.original_drive.empty() &&
          !it->takeover.volume_device_path.empty()) {
        restored_drives_[it->uuid] =
            RestoredDriveHint{it->takeover.original_drive,
                              it->takeover.volume_device_path};
      }
      Log(AutomountLogLevel::kInfo, "drive-letter-restore", it->uuid,
          it->takeover.original_drive, "",
          it->takeover.volume_present ? "restored" : "skipped-removed",
          it->takeover.volume_present
              ? "the original Windows mount point " +
                    it->takeover.mount_point + " was restored after " +
                    std::to_string(it->attempts) + " attempt(s)"
              : "the volume was removed; the original Windows mount point " +
                    it->takeover.mount_point +
                    " was not re-created after " +
                    std::to_string(it->attempts) + " attempt(s)");
      it = pending_restores_.erase(it);
      continue;
    }
    ++it->attempts;
    it->next_at = now + period;
    Log(AutomountLogLevel::kError, "drive-letter-restore", it->uuid,
        it->takeover.original_drive, "", "restore-pending",
        "attempt " + std::to_string(it->attempts) + " failed: " +
            restored.error().Format());
    ++it;
  }
}

void AutomountEngine::RetryStoppingSessions(AutomountTimePoint now) {
  for (auto it = stopping_.begin(); it != stopping_.end();) {
    Status stopped = platform_.StopMount(it->session_id);
    if (stopped.ok()) {
      Log(AutomountLogLevel::kInfo, "unmount", it->uuid, it->drive, "",
          "unmounted", "the pending session released its drive letter");
      RestoreTakeover(&it->takeover, it->uuid, now,
                      "a pending unmount completed");
      it = stopping_.erase(it);
      continue;
    }
    it->last_error = stopped.error().Format();
    ++it->attempts;
    if (it->attempts >= 3) {
      Log(AutomountLogLevel::kError, "unmount", it->uuid, it->drive, "",
          "terminal", stopped.error().Format() +
                          " (giving up after repeated attempts)");
      // The WinFsp letter may still be defined.  Keep the restoration intent:
      // it is retried by reconciliation and refuses to overwrite a letter that
      // now belongs to another volume.
      RestoreTakeover(&it->takeover, it->uuid, now,
                      "the mount session could not be stopped");
      it = stopping_.erase(it);
      continue;
    }
    ++it;
  }
}

void AutomountEngine::OnSessionTerminated(AutomountTimePoint now,
                                          const std::string& session_id,
                                          const std::string& reason) {
  auto it = std::find_if(
      mounts_.begin(), mounts_.end(),
      [&session_id](const MountedSession& s) {
        return s.session_id == session_id;
      });
  if (it == mounts_.end()) {
    Log(AutomountLogLevel::kDebug, "session-failed", "", "", "", "ignored",
        "unknown mount session " + session_id);
    return;
  }
  MountedSession dead = std::move(*it);
  mounts_.erase(it);
  Log(AutomountLogLevel::kError, "session-failed", UuidKey(dead.fs_uuid),
      dead.drive, LocationText(dead.location), "terminal", reason);
  StopSession(dead, now, reason);
  ScheduleRetry(dead.fs_uuid, now, nullptr);

  const auto delay = std::chrono::milliseconds(
      std::max<uint32_t>(250, config_.debounce_ms));
  ScheduleReconcile(now + delay);
}

void AutomountEngine::LogEntryRefusal(const AutomountEntry& entry,
                                      const std::string& key,
                                      const std::string& result,
                                      const std::string& detail) {
  const std::string uuid = UuidKey(entry.fs_uuid);
  const auto it = logged_refusals_.find(uuid);
  if (it != logged_refusals_.end() && it->second == key) return;
  logged_refusals_[uuid] = key;
  Log(AutomountLogLevel::kWarn, "entry-refused", uuid, "", "", result, detail);
}

void AutomountEngine::Log(AutomountLogLevel level, std::string event,
                          std::string uuid, std::string drive,
                          std::string device, std::string result,
                          std::string detail) {
  AutomountLogRecord record;
  record.level = level;
  record.event = std::move(event);
  record.uuid = std::move(uuid);
  record.drive = std::move(drive);
  record.device = std::move(device);
  record.result = std::move(result);
  record.detail = std::move(detail);
  platform_.EmitLog(record);
}

}  // namespace xfs