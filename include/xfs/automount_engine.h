// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - UX-1 automount event engine.
//
// The engine is platform independent: discovery, drive-letter enumeration and
// mount-session ownership are injected.  That keeps the event/debounce/retry
// policy testable on CI without USB hardware, and lets the Windows service
// remain a thin shell around Win32 device notifications and WinFsp mounts.
//
// Invariants the engine maintains:
//   * filesystems are identified by their XFS UUID, never by disk number;
//   * a mount is owned by exactly one session and a session is never reused
//     after its device disappears;
//   * retries are bounded and apply only to a device that is not ready yet,
//     never to a filesystem/policy refusal;
//   * a failure on one filesystem never unmounts another;
//   * removal stops the session, releases its drive letter, and a later
//     arrival starts a fresh discovery/mount cycle.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "xfs/automount_config.h"
#include "xfs/device_discovery.h"
#include "xfs/drive_letter.h"
#include "xfs/drive_letter_takeover.h"
#include "xfs/error.h"

namespace xfs {

using AutomountClock = std::chrono::steady_clock;
using AutomountTimePoint = AutomountClock::time_point;

enum class AutomountLogLevel : uint8_t {
  kDebug = 0,
  kInfo,
  kWarn,
  kError,
};

const char* ToString(AutomountLogLevel level);

// One structured record.  Fields are intentionally fixed instead of a free
// text blob so the service log stays greppable and the CLI/tests can assert
// the important facts (UUID, physical identity, drive, result).
struct AutomountLogRecord {
  AutomountLogLevel level = AutomountLogLevel::kInfo;
  std::string event;   // device-change, mount-attempt, mount-result, ...
  std::string uuid;    // formatted XFS UUID when known
  std::string drive;   // "E:" when known
  std::string device;  // disk path / serial / stable id when known
  std::string result;  // mounted, unmounted, refused, retry, ...
  std::string detail;  // human-readable extra context
};

struct AutomountMountStart {
  Status status;
  std::string session_id;
  // True only when the device was not ready yet and a bounded retry may work.
  // A filesystem error or a policy refusal must set this to false.
  bool retryable = false;
};

// The platform boundary.  Implementations must be safe to call from the
// service worker thread.  StopMount must not return success until the session
// has released its drive letter (or has definitively failed); the engine
// relies on that to avoid allocating the same letter twice.
class AutomountPlatform {
 public:
  virtual ~AutomountPlatform() = default;

  virtual Result<DiscoveryReport> Discover(
      const DiscoveryOptions& options) = 0;
  virtual std::vector<DriveLetterInfo> ListDriveLetters(std::string* error) = 0;
  // Takes over the Windows drive letter of the RAW volume, if the selection
  // asked for one (choice.release_drive non-empty).  Returns a kNone record
  // when there is nothing to take over.  On failure the record still describes
  // what Windows state may need to be restored; the engine schedules the retry.
  virtual TakeoverResult TakeOverDriveLetter(
      const DiscoveredFilesystem& filesystem,
      const DriveLetterChoice& choice) = 0;
  // Restores an original Windows mount point after the session ended (or after
  // a failed mount).  Safe to call repeatedly.
  virtual Status RestoreDriveLetterTakeover(DriveLetterTakeover* takeover) = 0;
  virtual AutomountMountStart StartMount(const DiscoveredFilesystem& filesystem,
                                         const AutomountEntry& entry,
                                         AutomountMode mode,
                                         const std::string& drive) = 0;
  virtual Status StopMount(const std::string& session_id) = 0;
  virtual bool IsSessionAlive(const std::string& session_id) = 0;
  virtual void EmitLog(const AutomountLogRecord& record) = 0;
};

struct AutomountSettings {
  DiscoveryOptions discovery;
  uint32_t initial_retry_ms = 500;
  uint32_t max_retry_ms = 8000;
  uint32_t max_ready_retries = 5;
  // Upper bound on how long a burst of device notifications may postpone the
  // coalesced discovery pass.
  uint32_t max_debounce_wait_ms = 3000;
};

struct AutomountMountStatus {
  Uuid fs_uuid{};
  std::string label;
  std::string drive;
  std::string session_id;
  std::string serial_number;
  std::string stable_id;
  uint64_t device_size_bytes = 0;
  uint64_t filesystem_bytes = 0;
  uint64_t free_bytes = 0;
  // Effective mode.  It is read-only when a configured read-write entry fell
  // back because the attached device no longer matches its identity pins.
  AutomountMode mode = AutomountMode::kReadOnly;
  AutomountMode configured_mode = AutomountMode::kReadOnly;
  std::string mode_note;
  bool read_write = false;
  // True when the drive letter was taken over from the RAW Windows volume,
  // false when it was freshly allocated.
  bool drive_taken_over = false;
  std::string original_windows_drive;  // "F:" when taken over
  std::string device_path;
};

enum class AutomountEntryState : uint8_t {
  kDisabled = 0,
  kWaitingForDevice,
  kRetrying,
  kMounted,
  kMountedElsewhere,
  kRefused,
  kFailed,
};

const char* ToString(AutomountEntryState state);

struct AutomountEntryStatus {
  Uuid fs_uuid{};
  bool enabled = false;
  AutomountMode mode = AutomountMode::kReadOnly;  // configured
  AutomountMode effective_mode = AutomountMode::kReadOnly;
  std::string mode_note;
  std::string preferred_drive;
  AutomountEntryState state = AutomountEntryState::kWaitingForDevice;
  std::string label;
  std::string drive;
  bool drive_taken_over = false;
  std::string original_windows_drive;
  std::string detail;
  uint32_t retry_attempt = 0;
};

struct AutomountStatus {
  std::vector<AutomountMountStatus> mounts;
  std::vector<AutomountEntryStatus> entries;
  std::vector<DiscoveredFilesystem> discovered;
  std::vector<RejectedFilesystem> rejected;
  std::vector<std::string> warnings;
  std::string config_error;
  bool discovery_incomplete = false;
  uint64_t discovery_retryable_failures = 0;
  bool last_reconcile_valid = false;
  AutomountTimePoint last_reconcile{};
};

// The service-layer configuration source.  Returning an error keeps the
// previous configuration active; the engine reports the error in its status
// instead of silently dropping the running mounts.
using AutomountConfigLoader =
    std::function<Result<AutomountConfigLoad>()>;

class AutomountEngine {
 public:
  AutomountEngine(AutomountPlatform& platform,
                  AutomountSettings settings = {});
  ~AutomountEngine();

  AutomountEngine(const AutomountEngine&) = delete;
  AutomountEngine& operator=(const AutomountEngine&) = delete;

  void SetConfig(AutomountConfig config);
  void SetConfigLoader(AutomountConfigLoader loader);
  const AutomountConfig& config() const { return config_; }

  // Starts the event loop: performs the initial discovery without waiting for
  // a device notification, which covers service restart / Windows boot with
  // the device already inserted.
  void Start(AutomountTimePoint now);
  // Stops every session owned by this engine.  Used for service stop/uninstall.
  void Stop(AutomountTimePoint now);
  bool running() const { return running_; }

  // A Windows device arrival/removal notification.  The hint is diagnostic
  // only (an interface path); discovery is authoritative.  Bursts are
  // coalesced, and the retry backoff is reset because the device state may
  // have changed.  Returns the time at which Tick must next be called.
  void NotifyDeviceChanged(AutomountTimePoint now, std::string hint = {});
  // The configuration changed on disk (service control or file watcher).
  void NotifySettingsChanged(AutomountTimePoint now);

  // Runs all work that is due at `now` (debounced device pass, periodic
  // reconciliation, retry).  Safe to call as often as the service loop wakes.
  void Tick(AutomountTimePoint now);
  // Absolute time of the next due work, or nullopt when stopped / nothing is
  // scheduled.  The service uses this as its WaitForSingleObject timeout; it
  // is not a polling interval.
  std::optional<AutomountTimePoint> NextWake(AutomountTimePoint now) const;

  const AutomountStatus& status() const { return status_; }
  // Used by the service when a session reports a terminal device failure.
  // The engine removes its record and schedules a reconciliation; the session
  // itself is never reused.
  void OnSessionTerminated(AutomountTimePoint now, const std::string& session_id,
                           const std::string& reason);

 private:
  struct MountedSession {
    Uuid fs_uuid{};
    std::string label;
    std::string drive;
    std::string session_id;
    std::string preferred_drive;
    AutomountMode configured_mode = AutomountMode::kReadOnly;
    AutomountMode mode = AutomountMode::kReadOnly;  // effective
    std::string mode_note;
    PartitionLocation location;
    uint64_t filesystem_bytes = 0;
    uint64_t free_bytes = 0;
    std::string drive_note;
    DriveLetterTakeover takeover;
  };

  struct RetryState {
    uint32_t attempts = 0;
    AutomountTimePoint next_at{};
  };

  struct StoppingSession {
    std::string session_id;
    std::string drive;
    std::string uuid;
    uint32_t attempts = 0;
    // Restored only after StopMount confirms the WinFsp letter is free.  The
    // volume may be gone by then; the restore operation decides.
    DriveLetterTakeover takeover;
  };

  struct PendingRestore {
    DriveLetterTakeover takeover;
    std::string uuid;
    std::string reason;
    uint32_t attempts = 0;
    AutomountTimePoint next_at{};
  };

  void Reconcile(AutomountTimePoint now, bool reload_config);
  void ReloadConfig(AutomountTimePoint now);
  void ScheduleReconcile(AutomountTimePoint at);
  bool ScheduleRetry(const Uuid& uuid, AutomountTimePoint now,
                     uint32_t* attempts_out);
  bool RetryDue(const Uuid& uuid, AutomountTimePoint now) const;
  void PruneRetries();
  void StopSession(MountedSession& session, AutomountTimePoint now,
                   const std::string& reason);
  void RetryStoppingSessions(AutomountTimePoint now);
  // Restores after a session ends.  Failures become pending restores that are
  // retried by reconciliation; success is logged, never assumed.
  void RestoreTakeover(DriveLetterTakeover* takeover, const std::string& uuid,
                       AutomountTimePoint now, const std::string& reason);
  void RetryPendingRestores(AutomountTimePoint now);
  bool ChooseDrive(const DiscoveredFilesystem& filesystem,
                   const AutomountEntry& entry, DriveLetterUsage& usage,
                   bool usage_ready, DriveLetterChoice* choice,
                   std::string* error);
  AutomountMode EffectiveMode(const AutomountEntry& entry,
                              const DiscoveredFilesystem& filesystem,
                              std::string* note) const;
  bool TakeoverEligible(const DiscoveredFilesystem& filesystem) const;
  void Log(AutomountLogLevel level, std::string event, std::string uuid,
           std::string drive, std::string device, std::string result,
           std::string detail);
  void LogEntryRefusal(const AutomountEntry& entry, const std::string& key,
                       const std::string& result, const std::string& detail);
  bool EntryPolicyAccepts(const AutomountEntry& entry,
                          const DiscoveredFilesystem& filesystem,
                          AutomountMode effective_mode,
                          std::string* refusal) const;
  bool EntryIdentityMatches(const AutomountEntry& entry,
                            const PartitionLocation& location,
                            std::string* mismatch) const;
  const DiscoveredFilesystem* SelectFilesystem(
      const AutomountEntry& entry,
      const std::vector<const DiscoveredFilesystem*>& candidates,
      std::string* detail) const;

  AutomountPlatform& platform_;
  AutomountSettings settings_;
  AutomountConfig config_;
  AutomountConfigLoader config_loader_;
  bool has_config_loader_ = false;
  bool running_ = false;

  std::vector<MountedSession> mounts_;
  std::vector<StoppingSession> stopping_;
  std::vector<PendingRestore> pending_restores_;
  std::map<std::string, RetryState> retries_;
  std::map<std::string, bool> exhausted_;
  // Last refusal text per entry, so an unchanged refusal is logged once
  // instead of every reconciliation pass.
  std::map<std::string, std::string> logged_refusals_;
  // The drive letter each filesystem used before its device was removed.
  // A takeover deletes the Windows mount point, and Windows does not recreate
  // it on reinsertion; this session-scoped memory lets the device come back
  // at the same letter instead of racing an unrelated allocation.  It is
  // never persisted and never overrides an explicit preference or a letter
  // Windows did assign.
  std::map<std::string, std::string> remembered_drives_;
  std::string last_config_fingerprint_;
  std::string last_config_error_;

  std::optional<AutomountTimePoint> pending_reconcile_;
  std::optional<AutomountTimePoint> first_pending_event_;
  std::optional<AutomountTimePoint> next_periodic_;
  bool settings_changed_ = false;

  AutomountStatus status_;
};

}  // namespace xfs