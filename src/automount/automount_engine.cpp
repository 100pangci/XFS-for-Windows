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

void AutomountEngine::Stop(AutomountTimePoint now) {
  if (!running_) return;
  Log(AutomountLogLevel::kInfo, "engine-stop", "", "", "", "stopping",
      "unmounting every automount-owned session");
  for (auto& session : mounts_) {
    StopSession(session, now, "automount service is stopping");
  }
  mounts_.clear();
  RetryStoppingSessions(now);
  stopping_.clear();
  retries_.clear();
  exhausted_.clear();
  pending_reconcile_.reset();
  first_pending_event_.reset();
  next_periodic_.reset();
  running_ = false;
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
}

void AutomountEngine::Reconcile(AutomountTimePoint now, bool reload_config) {
  (void)reload_config;  // the config source is consulted every pass
  status_ = AutomountStatus{};
  status_.last_reconcile = now;
  status_.last_reconcile_valid = true;

  // A missing/changed configuration is picked up on every pass.  The service
  // additionally calls NotifySettingsChanged for immediate application.
  ReloadConfig(now);
  PruneRetries();
  RetryStoppingSessions(now);

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
      mount.read_write =
          session.mode == AutomountMode::kExperimentalReadWrite;
      mount.device_path = session.location.disk_path;
      status_.mounts.push_back(std::move(mount));
    }
    for (const auto& entry : config_.entries) {
      AutomountEntryStatus es;
      es.fs_uuid = entry.fs_uuid;
      es.enabled = entry.enabled;
      es.mode = entry.mode;
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
        es.detail = mounted->drive_note;
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
      } else if (entry->mode != session.mode) {
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
    mount.read_write = session.mode == AutomountMode::kExperimentalReadWrite;
    mount.device_path = session.location.disk_path;
    status_.mounts.push_back(std::move(mount));
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
      es.detail = mounted->drive_note;
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

    std::string mismatch;
    if (!EntryIdentityMatches(entry, filesystem->location, &mismatch)) {
      es.state = AutomountEntryState::kRefused;
      es.detail = mismatch;
      LogEntryRefusal(entry, "identity:" + mismatch, "refused", mismatch);
      status_.entries.push_back(std::move(es));
      continue;
    }

    std::string refusal;
    if (!EntryPolicyAccepts(entry, *filesystem, &refusal)) {
      es.state = AutomountEntryState::kRefused;
      es.detail = refusal;
      LogEntryRefusal(entry, "policy:" + refusal, "refused", refusal);
      status_.entries.push_back(std::move(es));
      continue;
    }

    if (!RetryDue(entry.fs_uuid, now)) {
      const auto it = retries_.find(key);
      es.state = AutomountEntryState::kRetrying;
      es.retry_attempt = it == retries_.end() ? 0 : it->second.attempts;
      es.detail = "waiting before the next mount attempt";
      status_.entries.push_back(std::move(es));
      continue;
    }

    std::string drive;
    bool preferred_unavailable = false;
    std::string allocation_error;
    if (!ChooseDrive(*filesystem, entry, usage, usage_ready, &drive,
                     &preferred_unavailable, &allocation_error)) {
      es.state = AutomountEntryState::kFailed;
      es.detail = allocation_error;
      LogEntryRefusal(entry, "drive:" + allocation_error, "failed",
                      allocation_error);
      status_.entries.push_back(std::move(es));
      continue;
    }

    Log(AutomountLogLevel::kInfo, "mount-attempt", key, drive,
        LocationText(filesystem->location), "starting",
        std::string("mode ") + ToString(entry.mode));
    AutomountMountStart started =
        platform_.StartMount(*filesystem, entry, drive);
    if (started.status.ok() && !started.session_id.empty()) {
      MountedSession session;
      session.fs_uuid = entry.fs_uuid;
      session.label = filesystem->probe.label;
      session.drive = drive;
      session.session_id = started.session_id;
      session.preferred_drive = entry.preferred_drive;
      session.mode = entry.mode;
      session.location = filesystem->location;
      session.filesystem_bytes = filesystem->probe.filesystem_bytes;
      session.free_bytes = filesystem->probe.free_bytes;
      if (preferred_unavailable) {
        session.drive_note = "The configured drive letter " +
                             entry.preferred_drive +
                             " was already in use; mounted as " + drive +
                             " instead.";
      }
      mounts_.push_back(session);
      retries_.erase(key);
      exhausted_.erase(key);
      logged_refusals_.erase(key);

      es.state = AutomountEntryState::kMounted;
      es.drive = drive;
      es.detail = session.drive_note;
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
      mount.read_write = session.mode == AutomountMode::kExperimentalReadWrite;
      mount.device_path = session.location.disk_path;
      status_.mounts.push_back(std::move(mount));

      std::string detail = std::string("mounted ") + ToString(entry.mode);
      if (preferred_unavailable) {
        detail += "; " + session.drive_note;
      }
      Log(AutomountLogLevel::kInfo, "mount-result", key, drive,
          LocationText(session.location), "mounted", detail);
      status_.entries.push_back(std::move(es));
      continue;
    }

    const std::string failure = started.status.ok()
                                    ? "the mount session started without an id"
                                    : started.status.error().Format();
    uint32_t attempts = 0;
    if (started.retryable &&
        ScheduleRetry(entry.fs_uuid, now, &attempts)) {
      es.state = AutomountEntryState::kRetrying;
      es.retry_attempt = attempts;
      es.detail = "the device is not ready yet; retrying";
      Log(AutomountLogLevel::kWarn, "mount-retry", key, drive,
          LocationText(filesystem->location), "retry",
          failure + " (attempt " + std::to_string(attempts) + ")");
    } else {
      retries_.erase(key);
      if (started.retryable) {
        es.state = AutomountEntryState::kFailed;
        es.detail =
            "the device did not become ready after the bounded retries";
      } else {
        es.state = AutomountEntryState::kFailed;
        es.detail = failure;
      }
      LogEntryRefusal(entry, "mount:" + failure, "failed", es.detail);
    }
    status_.entries.push_back(std::move(es));
  }

  Log(AutomountLogLevel::kDebug, "reconcile", "", "", "", "completed",
      "periodic reconciliation scheduled");
}

bool AutomountEngine::ChooseDrive(const DiscoveredFilesystem& filesystem,
                                  const AutomountEntry& entry,
                                  DriveLetterUsage& usage, bool usage_ready,
                                  std::string* drive,
                                  bool* preferred_unavailable,
                                  std::string* error) {
  (void)filesystem;
  if (!usage_ready) {
    if (error) *error = "drive letters could not be enumerated";
    return false;
  }
  auto choice = ChooseDriveLetter(entry.preferred_drive, usage);
  if (!choice.ok()) {
    if (error) {
      *error = choice.error().code == XfsError::kNoSpace
                   ? "no free drive letter is available"
                   : choice.error().message;
    }
    return false;
  }
  if (drive) *drive = choice.value().drive;
  if (preferred_unavailable) {
    *preferred_unavailable = choice.value().preferred_unavailable;
  }
  if (choice.value().drive.size() == 2) {
    usage.used[static_cast<size_t>(choice.value().drive[0] - 'A')] = true;
  }
  return true;
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
  return true;
}

bool AutomountEngine::EntryPolicyAccepts(
    const AutomountEntry& entry, const DiscoveredFilesystem& filesystem,
    std::string* refusal) const {
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

  if (entry.mode != AutomountMode::kExperimentalReadWrite) {
    return true;
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

void AutomountEngine::StopSession(MountedSession& session,
                                  AutomountTimePoint now,
                                  const std::string& reason) {
  Log(AutomountLogLevel::kInfo, "unmount", UuidKey(session.fs_uuid),
      session.drive, LocationText(session.location), "stopping", reason);
  Status stopped = platform_.StopMount(session.session_id);
  if (stopped.ok()) {
    Log(AutomountLogLevel::kInfo, "unmount", UuidKey(session.fs_uuid),
        session.drive, LocationText(session.location), "unmounted", reason);
    return;
  }
  Log(AutomountLogLevel::kWarn, "unmount", UuidKey(session.fs_uuid),
      session.drive, LocationText(session.location), "pending",
      stopped.error().Format());
  StoppingSession stopping;
  stopping.session_id = session.session_id;
  stopping.drive = session.drive;
  stopping.uuid = UuidKey(session.fs_uuid);
  stopping.attempts = 1;
  stopping_.push_back(std::move(stopping));
  // A session that did not stop immediately still owns its drive letter.
  // Ask the loop to retry shortly instead of waiting for the next periodic
  // reconciliation.
  ScheduleReconcile(now + std::chrono::seconds(1));
}

void AutomountEngine::RetryStoppingSessions(AutomountTimePoint now) {
  (void)now;
  for (auto it = stopping_.begin(); it != stopping_.end();) {
    Status stopped = platform_.StopMount(it->session_id);
    if (stopped.ok()) {
      Log(AutomountLogLevel::kInfo, "unmount", it->uuid, it->drive, "",
          "unmounted", "the pending session released its drive letter");
      it = stopping_.erase(it);
      continue;
    }
    ++it->attempts;
    if (it->attempts >= 3) {
      Log(AutomountLogLevel::kError, "unmount", it->uuid, it->drive, "",
          "terminal", stopped.error().Format() +
                          " (giving up after repeated attempts)");
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