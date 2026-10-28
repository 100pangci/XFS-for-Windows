// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/crash.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace xfs {
namespace {

struct PointName {
  CrashPoint point;
  const char* name;
};

// Keep the names short and stable: they appear in CI logs and scripts.
constexpr PointName kPointNames[] = {
    {CrashPoint::kBeforeDataWrite, "before_data_write"},
    {CrashPoint::kMidDataWrite, "mid_data_write"},
    {CrashPoint::kAfterDataWrite, "after_data_write"},
    {CrashPoint::kBeforeLogWrite, "before_log_write"},
    {CrashPoint::kMidLogRecord, "mid_log_record"},
    {CrashPoint::kAfterLogCommit, "after_log_commit"},
    {CrashPoint::kBeforeMetadataWrite, "before_metadata_write"},
    {CrashPoint::kMidMetadataWrite, "mid_metadata_write"},
    {CrashPoint::kAfterMetadataWrite, "after_metadata_write"},
    {CrashPoint::kBeforeDeviceFlush, "before_device_flush"},
    {CrashPoint::kAfterDeviceFlush, "after_device_flush"},
    {CrashPoint::kCowStaged, "cow_staged"},
    {CrashPoint::kBeforeCowDataWrite, "before_cow_data_write"},
    {CrashPoint::kMidCowDataWrite, "mid_cow_data_write"},
    {CrashPoint::kAfterCowDataWrite, "after_cow_data_write"},
    {CrashPoint::kBeforeCowRemapCommit, "before_cow_remap_commit"},
    {CrashPoint::kAfterCowRemap, "after_cow_remap"},
    {CrashPoint::kAfterRefcountUpdate, "after_refcount_update"},
    {CrashPoint::kAfterRmapUpdate, "after_rmap_update"},
    {CrashPoint::kAfterBmapSwitch, "after_bmap_switch"},
    {CrashPoint::kBeforeOldRangeRelease, "before_old_range_release"},
    {CrashPoint::kAfterOldRangeRelease, "after_old_range_release"},
    {CrashPoint::kBeforeIntentDone, "before_intent_done"},
    {CrashPoint::kAfterIntentDone, "after_intent_done"},
    {CrashPoint::kRecoveryScan, "recovery_scan"},
    {CrashPoint::kRecoveryBeforePass2, "recovery_before_pass2"},
    {CrashPoint::kRecoveryAfterTransaction, "recovery_after_transaction"},
    {CrashPoint::kRecoveryMidBuffer, "recovery_mid_buffer"},
    {CrashPoint::kRecoveryAfterMetadataFlush, "recovery_after_metadata_flush"},
    {CrashPoint::kRecoveryBeforeIntents, "recovery_before_intents"},
    {CrashPoint::kRecoveryMidIntents, "recovery_mid_intents"},
    {CrashPoint::kRecoveryBeforeIunlinks, "recovery_before_iunlinks"},
    {CrashPoint::kRecoveryMidIunlinks, "recovery_mid_iunlinks"},
    {CrashPoint::kRecoveryBeforeLogClean, "recovery_before_log_clean"},
    {CrashPoint::kRecoveryAfterLogClean, "recovery_after_log_clean"},
};

std::mutex g_config_mutex;
bool g_loaded = false;
std::atomic<bool> g_config_ready{false};
CrashConfig g_config;
std::unordered_map<int, uint32_t> g_hits;

// Parses "<point>[:<hit>]".
bool ParseConfigValue(std::string_view value, CrashConfig* out) {
  if (value.empty() || value == "0" || value == "none" || value == "off") {
    return false;
  }
  std::string_view point = value;
  size_t colon = value.find(':');
  if (colon != std::string_view::npos) {
    point = value.substr(0, colon);
    std::string_view hit_text = value.substr(colon + 1);
    if (hit_text.empty()) return false;
    uint32_t hit = 0;
    for (char c : hit_text) {
      if (c < '0' || c > '9') return false;
      hit = hit * 10 + static_cast<uint32_t>(c - '0');
      if (hit > 1000000) return false;
    }
    if (hit == 0) return false;
    out->hit = hit;
  }
  CrashPoint parsed = CrashPoint::kCount;
  if (!ParseCrashPoint(point, &parsed)) return false;
  out->enabled = true;
  out->point = parsed;
  return true;
}

void LoadConfigLocked() {
  if (g_loaded) return;
  g_loaded = true;
  g_hits.clear();
  CrashConfig config;
  std::string raw;
  if (const char* env = std::getenv("XFSWIN_CRASH_AT")) raw = env;
  if (raw.empty()) {
    g_config = config;
    g_config_ready.store(true, std::memory_order_release);
    return;
  }
  config.raw = raw;
  if (!ParseConfigValue(raw, &config)) {
    std::fprintf(stderr,
                 "xfswin: warning: XFSWIN_CRASH_AT='%s' is not a known crash "
                 "point[:hit]; crash injection disabled\n",
                 raw.c_str());
    g_config = CrashConfig{};
    g_config_ready.store(true, std::memory_order_release);
    return;
  }
  g_config = config;
  std::fprintf(stderr, "xfswin: crash injection armed at %s (hit %u)\n",
               ToString(config.point), config.hit);
  g_config_ready.store(true, std::memory_order_release);
}

bool ShouldCrashLocked(CrashPoint point) {
  if (!g_config.enabled || g_config.point != point) return false;
  uint32_t& hits = g_hits[static_cast<int>(point)];
  ++hits;
  return hits == g_config.hit;
}

}  // namespace

const char* ToString(CrashPoint point) {
  for (const auto& entry : kPointNames) {
    if (entry.point == point) return entry.name;
  }
  return "?";
}

bool ParseCrashPoint(std::string_view name, CrashPoint* out) {
  for (const auto& entry : kPointNames) {
    if (name == entry.name) {
      *out = entry.point;
      return true;
    }
  }
  return false;
}

CrashConfig GetCrashConfig() {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  LoadConfigLocked();
  return g_config;
}

bool CrashPointArmed(CrashPoint point) {
  if (!g_config_ready.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    LoadConfigLocked();
  }
  // g_config is immutable after the release-store above (ReloadCrashConfig is
  // only used by single-threaded tests).
  return g_config.enabled && g_config.point == point;
}

void ReloadCrashConfig() {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  g_loaded = false;
  g_config_ready.store(false, std::memory_order_release);
  g_config = CrashConfig{};
  g_hits.clear();
}

bool ShouldCrash(CrashPoint point) {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  LoadConfigLocked();
  return ShouldCrashLocked(point);
}

void MaybeCrash(CrashPoint point, const std::string& detail) {
  if (!ShouldCrash(point)) return;
  std::fprintf(stderr, "XFSWIN CRASH INJECTION at %s%s%s\n", ToString(point),
               detail.empty() ? "" : ": ", detail.c_str());
  std::fflush(stderr);
  // Terminate immediately: destructors, atexit handlers and unmount logic must
  // not run, so the on-disk state is exactly what the crash point produced.
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), kCrashExitCode);
  // TerminateProcess does not return; if it somehow failed, end the process
  // without running any destructor.
  std::_Exit(static_cast<int>(kCrashExitCode));
#else
  _exit(static_cast<int>(kCrashExitCode));
#endif
}

}  // namespace xfs
