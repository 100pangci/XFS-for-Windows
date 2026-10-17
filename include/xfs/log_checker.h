// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - internal log state: cleanliness check + next write position.
//
// This build does not replay the journal, so before mounting we determine
// whether the log is clean (the newest record is an unmount record, or the log
// is empty/zeroed). A read-write mount additionally needs to know where the
// next record goes, so the scan also reports the newest record's cycle, block,
// size, previous-block link and tail LSN. The read-write writer uses exactly
// the state the kernel establishes after finding a clean log
// (xlog_find_tail/xlog_check_unmount_rec).
//
// Implementation: scan the log for record headers (512-byte aligned), select
// the newest by (cycle, block) LSN, and inspect that record.
#pragma once

#include <cstdint>

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
  // Geometry of the newest record (filled for kClean and kDirty).
  uint32_t cycle = 0;        // write cycle of the newest record
  uint32_t block = 0;        // its header block (log block = 512 bytes)
  uint32_t blocks = 0;       // header blocks + data blocks it occupies
  uint32_t num_ops = 0;      // h_num_logops
  uint32_t prev_block = 0;   // its h_prev_block
  uint64_t lsn = 0;          // its h_lsn
};

Result<LogCheckResult> CheckLogClean(BlockDevice& dev,
                                     const XfsGeometry& geo,
                                     const XfsSuperblock& sb);

const char* ToString(LogState state);

// ---------------------------------------------------------------------------
// Write position for the read-write writer
// ---------------------------------------------------------------------------
// The state the kernel has after mounting a cleanly unmounted filesystem:
// the next record header goes to `next_block` with `cycle`, links back to
// `prev_block`, and carries `tail_lsn`. All blocks are log blocks (512 bytes).
struct LogPosition {
  uint32_t cycle = 0;
  uint32_t next_block = 0;
  uint32_t prev_block = 0;
  uint64_t tail_lsn = 0;
  uint32_t log_blocks = 0;
  uint32_t record_size = 0;  // h_size the writer should put in its records
  bool clean = false;
};

// Scans the log and returns the next write position. Requires a clean log: a
// dirty log means the previous owner did not unmount cleanly, and this build
// cannot replay it yet.
Result<LogPosition> ReadLogPosition(BlockDevice& dev, const XfsGeometry& geo,
                                    const XfsSuperblock& sb);

}  // namespace xfs
