// XFS for Windows - structured debug logging.
//
// Usage:
//   XFS_INFO("filesystem UUID " << uuid);
//   XFS_DEBUG("level=" << level << " block=" << blk);
//
// Normal mode prints WARN/ERROR/INFO only, --verbose adds INFO detail and
// --debug adds traversal output. Log lines are prefixed with a component tag
// as described in the project spec, e.g. "[SB]", "[AG]", "[INO]", "[BTREE]".
#pragma once

#include <sstream>
#include <string>
#include <utility>

namespace xfs::log {

enum class Level { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3, kTrace = 4 };

void SetLevel(Level level);
Level GetLevel();
bool Enabled(Level level);

// Where to write (defaults to stderr). Not thread safe to change while
// logging from another thread; the CLI sets this up before doing work.
void SetOutputStream(std::ostream* stream);

void WriteLine(Level level, const std::string& text);

// Accumulates a line and emits it on destruction if the level is enabled.
class Line {
 public:
  explicit Line(Level level, const char* component)
      : level_(level), component_(component) {}

  ~Line() {
    if (Enabled(level_)) WriteLine(level_, os_.str());
  }

  template <typename T>
  Line& operator<<(const T& value) {
    if (Enabled(level_)) os_ << value;
    return *this;
  }

 private:
  Level level_;
  const char* component_;
  std::ostringstream os_;
};

}  // namespace xfs::log

#define XFS_LOG(level, tag, ...)                    \
  do {                                              \
    ::xfs::log::Line _xfs_log_line((level), (tag));  \
    _xfs_log_line << __VA_ARGS__;                   \
  } while (0)

#define XFS_ERROR(...) XFS_LOG(::xfs::log::Level::kError, "ERR", __VA_ARGS__)
#define XFS_WARN(...) XFS_LOG(::xfs::log::Level::kWarn, "WRN", __VA_ARGS__)
#define XFS_INFO(...) XFS_LOG(::xfs::log::Level::kInfo, "INF", __VA_ARGS__)
#define XFS_DEBUG(...) XFS_LOG(::xfs::log::Level::kDebug, "DBG", __VA_ARGS__)
#define XFS_TRACE(...) XFS_LOG(::xfs::log::Level::kTrace, "TRC", __VA_ARGS__)

// Component-scoped logging used by the core parser.
#define XFS_SB_INFO(...) XFS_LOG(::xfs::log::Level::kInfo, "SB", __VA_ARGS__)
#define XFS_SB_DEBUG(...) XFS_LOG(::xfs::log::Level::kDebug, "SB", __VA_ARGS__)
#define XFS_AG_DEBUG(...) XFS_LOG(::xfs::log::Level::kDebug, "AG", __VA_ARGS__)
#define XFS_INO_DEBUG(...) XFS_LOG(::xfs::log::Level::kDebug, "INO", __VA_ARGS__)
#define XFS_BTREE_DEBUG(...) \
  XFS_LOG(::xfs::log::Level::kDebug, "BTREE", __VA_ARGS__)
#define XFS_DIR_DEBUG(...) XFS_LOG(::xfs::log::Level::kDebug, "DIR", __VA_ARGS__)
#define XFS_EXT_DEBUG(...) XFS_LOG(::xfs::log::Level::kDebug, "EXT", __VA_ARGS__)
#define XFS_MKFS_INFO(...) XFS_LOG(::xfs::log::Level::kInfo, "MKFS", __VA_ARGS__)
#define XFS_MKFS_DEBUG(...) \
  XFS_LOG(::xfs::log::Level::kDebug, "MKFS", __VA_ARGS__)
