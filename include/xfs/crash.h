// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - crash injection for crash-consistency testing.
//
// A filesystem writer is only as good as its crash behaviour, so every
// mutation path carries injection points from the first day. Set
//
//   XFSWIN_CRASH_AT=<point>[:<hit>]
//
// to terminate the process (no clean shutdown, no unmount record, no buffered
// flush) when that point is reached. `<hit>` selects which occurrence crashes
// (default 1), which is how multi-transaction tests pin down a specific
// transaction.
//
//   XFSWIN_CRASH_AT=before_log_write     # crash before the log record lands
//   XFSWIN_CRASH_AT=after_metadata_write:3
//
// The test then boots Linux and requires the filesystem to be either in the
// state before the transaction or after it - never in between.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace xfs {

// Points are named after the phase they sit in, so a test can be written
// without reading the writer: before/mid/after each phase of a mutation.
enum class CrashPoint {
  kBeforeDataWrite,
  kMidDataWrite,
  kAfterDataWrite,
  kBeforeLogWrite,
  kMidLogRecord,
  kAfterLogCommit,
  kBeforeMetadataWrite,
  kMidMetadataWrite,
  kAfterMetadataWrite,
  kBeforeDeviceFlush,
  kAfterDeviceFlush,
  // Journal recovery (RW-5): the scan, the replay passes, the deferred
  // completion phases and the final log-state transition.
  kRecoveryScan,
  kRecoveryBeforePass2,
  kRecoveryAfterTransaction,
  kRecoveryMidBuffer,
  kRecoveryAfterMetadataFlush,
  kRecoveryBeforeIntents,
  kRecoveryMidIntents,
  kRecoveryBeforeIunlinks,
  kRecoveryMidIunlinks,
  kRecoveryBeforeLogClean,
  kRecoveryAfterLogClean,
  kCount,
};

const char* ToString(CrashPoint point);
bool ParseCrashPoint(std::string_view name, CrashPoint* out);

// Process exit code used for an injected crash (distinguishable from a real
// failure in the test harness).
constexpr uint32_t kCrashExitCode = 0x58435348u;  // 'XCSH'

// Configuration from the environment (XFSWIN_CRASH_AT). Off by default.
struct CrashConfig {
  bool enabled = false;
  CrashPoint point = CrashPoint::kCount;
  uint32_t hit = 1;
  std::string raw;  // original value, for diagnostics
};

// Reads XFSWIN_CRASH_AT (cached after the first call).
CrashConfig GetCrashConfig();
// Drops the cached configuration (tests).
void ReloadCrashConfig();
// Decides whether this occurrence of `point` should crash, and counts hits.
// Never terminates the process: unit tests use this.
bool ShouldCrash(CrashPoint point);
// Terminates the process when `point` is configured (see ShouldCrash). Writes
// one line to stderr first so the harness knows where the crash happened.
void MaybeCrash(CrashPoint point, const std::string& detail);

}  // namespace xfs
