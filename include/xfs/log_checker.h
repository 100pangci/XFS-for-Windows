// XFS for Windows - read-only log cleanliness check.
//
// This build never replays the journal. Before mounting we must therefore
// determine whether the log is clean (i.e. the newest log record is an
// unmount record, or the log is empty/zeroed). If it is not, the filesystem
// was not cleanly unmounted and read-only access could expose inconsistent
// metadata; mounting is refused unless --force-unsafe-ro is given.
//
// Implementation: scan the log for record headers (512-byte block aligned),
// select the newest by (cycle, block) LSN, and inspect the first log op of
// that record for the XLOG_UNMOUNT_TRANS flag. This is the same decision the
// kernel makes; we do not interpret any other log content.
#pragma once

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/superblock.h"

namespace xfs {

enum class LogState {
  kEmpty,  // log is entirely zeroed
  kClean,  // newest record is an unmount record
  kDirty,  // newest record is a normal transaction record
  kUnknown // could not be determined (no headers / unreadable)
};

struct LogCheckResult {
  LogState state = LogState::kUnknown;
  uint64_t newest_record_lsn = 0;
  std::string detail;
};

Result<LogCheckResult> CheckLogClean(BlockDevice& dev,
                                     const XfsGeometry& geo,
                                     const XfsSuperblock& sb);

const char* ToString(LogState state);

}  // namespace xfs
