// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/log.h"

#include <iostream>
#include <mutex>

namespace xfs::log {
namespace {
Level g_level = Level::kInfo;
std::ostream* g_out = &std::cerr;

const char* LevelName(Level level) {
  switch (level) {
    case Level::kError: return "error";
    case Level::kWarn: return "warn";
    case Level::kInfo: return "info";
    case Level::kDebug: return "debug";
    case Level::kTrace: return "trace";
  }
  return "?";
}
}  // namespace

void SetLevel(Level level) { g_level = level; }
Level GetLevel() { return g_level; }
bool Enabled(Level level) { return static_cast<int>(level) <= static_cast<int>(g_level); }

void SetOutputStream(std::ostream* stream) { g_out = stream ? stream : &std::cerr; }

void WriteLine(Level level, const std::string& text) {
  if (!Enabled(level)) return;
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  *g_out << "[" << LevelName(level) << "] " << text << "\n";
  g_out->flush();
}

}  // namespace xfs::log
