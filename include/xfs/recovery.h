// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - internal log recovery (RW-5).
//
// This is the recovery engine, a faithful port of the kernel's log recovery
// (fs/xfs/xfs_log_recover.c and friends, see docs/RECOVERY.md). It is
// deliberately separate from CheckLogClean (src/core/log_checker.cpp), which
// stays a small "is the newest record an unmount record" classifier; recovery
// implements the real algorithms:
//
//   head/tail discovery      xlog_find_head / find_tail / find_zeroed
//   record validation        xlog_valid_rec_header / header_check_recover
//                            xlog_cksum (CRC over the cycle-stamped data)
//   cycle restoration        xlog_unpack_data / xlog_cycle_data
//   op stream reassembly     xlog_recover_process_data / process_ophdr
//                            add_to_trans / add_to_cont_trans
//   transaction commit       xlog_recover_commit_trans (PASS1/PASS2)
//   item replay              xfs_buf_item_recover.c / xfs_inode_item_recover.c
//                            xfs_icreate_item.c / xfs_extfree_item.c / ...
//
// The hard rule (docs/RECOVERY.md, "never partially recover"):
//
//   scan the entire recoverable log, validate every record, reconstruct every
//   committed transaction, inventory every item type, prove this build can
//   replay all of it - and only then perform the first metadata write.
//
// Anything else (unsupported item, corrupt transaction, invalid geometry, bad
// CRC inside the window, broken continuation chain) refuses *before* replay.
// A torn tail record at the head is not corruption: it is trimmed exactly like
// the kernel trims it (xlog_verify_head/xlog_verify_tail).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/log_checker.h"  // LogState
#include "xfs/superblock.h"

namespace xfs {

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
// The internal log is a ring of 512-byte basic blocks starting at
// sb_logstart. `sector_blocks` is the log sector size in basic blocks
// (sb_logsectsize / 512); I/O and cycle scanning happen in log sectors.
struct RecoveryGeometry {
  uint64_t log_start = 0;       // byte offset of the log in the device
  uint32_t log_blocks = 0;      // log size in 512-byte basic blocks
  uint32_t sector_blocks = 1;   // log sector size in basic blocks

  uint64_t BlockOffset(uint32_t block) const {
    return log_start + static_cast<uint64_t>(block) * 512;
  }
  uint32_t Wrap(uint32_t block) const { return block % log_blocks; }
};

// Derives the recovery geometry, refusing logs this build cannot treat as the
// writable profile's internal log.
Result<RecoveryGeometry> MakeRecoveryGeometry(const XfsSuperblock& sb,
                                              const XfsGeometry& geo);

// ---------------------------------------------------------------------------
// Item inventory (docs/RECOVERY.md has the full table)
// ---------------------------------------------------------------------------
enum class RecoveryItemClass {
  kBuffer,     // XFS_LI_BUF: physical buffer replay
  kInode,      // XFS_LI_INODE: logged inode core/forks
  kIcreate,    // XFS_LI_ICREATE: rebuild a freshly allocated inode chunk
  kIntent,     // XFS_LI_EFI / XFS_LI_BUI: start of a deferred operation
  kIntentDone, // XFS_LI_EFD / XFS_LI_BUD: cancel the matching intent
  kRefused,    // known type this build does not replay (rmap/reflink/quota/...)
  kUnknown,    // unknown type value
};

const char* ToString(RecoveryItemClass value);
// "XFS_LI_BUF", "XFS_LI_RUI", ... nullptr for unknown type values.
const char* LogItemTypeName(uint16_t type);
RecoveryItemClass ClassifyLogItem(uint16_t type);

// ---------------------------------------------------------------------------
// Scan result
// ---------------------------------------------------------------------------
// One committed log item as reconstructed from the operation stream: region 0
// is the item format header, the rest are the payload regions the format
// describes (exactly the ri_buf[] array xlog_recover_items_pass2 works on).
struct RecoveredItem {
  uint16_t type = 0;  // host byte order; first 16 bits of region 0
  uint32_t tid = 0;
  std::vector<std::vector<uint8_t>> regions;
};

// One committed transaction (`XLOG_COMMIT_TRANS` seen). The LSN is the LSN of
// the record in which the transaction started; that is the `current_lsn`
// recovery compares on-disk metadata against.
struct RecoveredTransaction {
  uint32_t tid = 0;
  uint64_t lsn = 0;
  std::vector<RecoveredItem> items;
};

// Diagnostics for one valid record in the replay window.
struct LogRecordInfo {
  uint64_t lsn = 0;
  uint32_t block = 0;         // header block
  uint32_t cycle = 0;
  uint32_t header_blocks = 1;
  uint32_t data_blocks = 0;
  uint32_t num_ops = 0;
  uint32_t prev_block = 0;
  uint64_t tail_lsn = 0;
  uint32_t computed_crc = 0;  // CRC as verified on read
  bool crc_stored = false;    // record carried a non-zero CRC
};

// The complete, read-only picture of an internal log.
struct LogScan {
  LogState state = LogState::kUnknown;
  RecoveryGeometry geometry;

  // Head/tail in the kernel's terms: the replay window is [tail, head).
  uint32_t tail_block = 0;
  uint32_t tail_cycle = 0;
  uint32_t head_block = 0;   // next write position
  uint32_t head_cycle = 0;   // cycle of the next record (bumped on wrap)
  uint32_t prev_block = 0;   // header block of the newest good record
  uint64_t newest_lsn = 0;   // h_lsn of the newest good record
  uint64_t tail_lsn = 0;     // h_tail_lsn of the newest good record
  bool wrapped = false;      // the newest record wrapped the physical end
  // kClean because the newest good record is the unmount record (as opposed to
  // kClean because the tail already reached the head): the log is closed and
  // an explicit recovery has nothing to append.
  bool ends_with_unmount_record = false;

  std::vector<LogRecordInfo> records;               // replay order
  std::vector<RecoveredTransaction> transactions;   // committed, replay order

  // Item types this build cannot replay, with record context. Non-empty means
  // "refuse": replaying part of the log would be worse than refusing.
  std::vector<std::string> unsupported_items;
  std::vector<std::string> warnings;
  std::string detail;

  uint64_t ItemCount() const;
  bool Recoverable() const { return unsupported_items.empty(); }
};

// Read-only: head/tail discovery, record validation, op reassembly and
// transaction reconstruction over the whole replay window. Never writes.
Result<LogScan> ScanLog(BlockDevice& dev, const XfsSuperblock& sb,
                        const XfsGeometry& geo);

// ---------------------------------------------------------------------------
// Replay (pass1/pass2) - declared here, implemented in src/recovery/replay.cpp
// ---------------------------------------------------------------------------
struct ReplayStats {
  uint64_t records_scanned = 0;         // records in the replay window
  uint64_t records_replayed = 0;
  uint64_t transactions_replayed = 0;
  uint64_t items_replayed = 0;
  uint64_t buffers_replayed = 0;
  uint64_t buffers_skipped_newer = 0;   // on-disk metadata was newer
  // Buffer cancellation accounting (xlog_recover_buf_commit_pass1/2).
  uint64_t cancels_seen = 0;            // cancel items in the replay window
  uint64_t cancels_consumed = 0;        // cancel references released
  uint64_t cancel_refs_unconsumed = 0;  // left over after pass 2
  uint64_t buffers_cancelled = 0;       // BUF items suppressed by a cancel
  uint64_t inode_items_cancelled = 0;   // INODE items suppressed
  uint64_t icreate_items_cancelled = 0; // ICREATE items suppressed
  // New owners: items replayed for a geometry whose cancel references were
  // already consumed (the reuse that the cancel was protecting).
  uint64_t buffers_after_cancel = 0;
  uint64_t inode_items_after_cancel = 0;
  uint64_t icreate_chunks_after_cancel = 0;
  uint64_t inodes_replayed = 0;
  uint64_t icreate_chunks_replayed = 0;
  uint64_t intents_pending = 0;         // unresolved EFI/BUI after replay
  uint64_t intent_done_items = 0;       // EFD/BUD items consumed
  uint64_t bytes_written = 0;
};

// The in-memory log state after a successful recovery, in the same shape as
// LogPosition: the next record goes to head_block/head_cycle, links back to
// prev_block and carries tail_lsn.
struct RecoveredLogState {
  uint32_t head_block = 0;
  uint32_t head_cycle = 0;
  uint32_t prev_block = 0;
  uint64_t tail_lsn = 0;
  uint32_t log_blocks = 0;
  uint32_t record_size = 0;
};

// Replays every committed transaction of `scan` (which must be Recoverable()).
// Metadata is patched directly - no high-level mutation APIs, no new log
// records. Order: pass1 (buffer cancellation table, intent inventory), pass2
// (buffer/inode/icreate replay with the metadata-LSN rules), then a final
// flush. Torn/uncommitted transactions are dropped, exactly like the kernel.
// On success `state` describes the position the writer continues from.
Result<ReplayStats> ReplayLog(BlockDevice& dev, const XfsSuperblock& sb,
                              const XfsGeometry& geo, const LogScan& scan,
                              RecoveredLogState* state);

// Pending intent operations found in the log that require post-replay work
// (deferred extent free / bmap update). Declared here; implemented with the
// filesystem-side completion pass.
struct PendingIntent {
  uint16_t type = 0;  // XFS_LI_EFI or XFS_LI_BUI
  uint32_t tid = 0;
  uint64_t lsn = 0;
  std::vector<std::vector<uint8_t>> regions;  // raw item regions
};

// Extracts the intents that survived replay (no matching done item) from the
// scan, in log order.
std::vector<PendingIntent> CollectPendingIntents(const LogScan& scan);

}  // namespace xfs
