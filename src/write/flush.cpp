// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - flush semantics.
//
// A flush in this project means "everything this process has written has
// reached stable storage". It deliberately does *not* mean anything about
// Windows' own file cache (that is the WinFsp layer's business) and it is not
// the same as a transaction commit: with only in-place data writes there are
// no pending transactions, so a flush is exactly a device flush. Once metadata
// transactions exist (RW-1), committing and flushing the journal happens
// before the device flush; the ordering rule that emerges from that:
//
//   data writes (ordered before their metadata)
//     -> metadata write-back
//       -> log tail/head update
//         -> device flush
//
// and a failed flush is never reported as success.
#include "xfs/filesystem.h"

#include "xfs/crash.h"
#include "xfs/log.h"
#include "xfs/perf_stats.h"

namespace xfs {

Status XfsFilesystem::Flush() {
  PerfStats& perf = PerfStats::Get();
  if (perf.enabled()) {
    perf.flush_calls.fetch_add(1, std::memory_order_relaxed);
  }
  // A flush is a durability barrier, not a global I/O lock: it waits for
  // every file-data write that started before it, and for any metadata
  // transaction, then issues one device flush. New writes wait at the barrier
  // only for the duration of the flush itself.
  std::unique_lock<std::shared_mutex> barrier(write_barrier_);
  // A durability boundary also publishes every deferred inode timestamp
  // before the device flush, so FlushFileBuffers semantics stay intact.
  Status committed = CommitAllDeferredTimesLocked();
  if (!committed.ok()) return committed;
  std::lock_guard<std::mutex> lock(mutation_mutex_);
  MaybeCrash(CrashPoint::kBeforeDeviceFlush, device_->Info().description);
  if (!device_->Flush()) {
    return XFS_ERR(XfsError::kIoError,
                   "device flush failed: " + device_->LastError())
        .Metadata("device");
  }
  if (metadata_flush_pending_) {
    metadata_flush_pending_ = false;
    if (log_position_valid_ && !recovery_tail_pin_.has_value()) {
      log_position_.tail_lsn =
          MakeLsn(log_position_.cycle, log_position_.next_block);
    }
  }
  MaybeCrash(CrashPoint::kAfterDeviceFlush, device_->Info().description);
  return OkStatus();
}

}  // namespace xfs
