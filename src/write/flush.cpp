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

namespace xfs {

Status XfsFilesystem::Flush() {
  // Wait for in-flight mutations so a flush never returns before the writes it
  // was asked to make durable.
  std::lock_guard<std::mutex> lock(mutation_mutex_);
  MaybeCrash(CrashPoint::kBeforeDeviceFlush, device_->Info().description);
  if (!device_->Flush()) {
    return XFS_ERR(XfsError::kIoError,
                   "device flush failed: " + device_->LastError())
        .Metadata("device");
  }
  MaybeCrash(CrashPoint::kAfterDeviceFlush, device_->Info().description);
  return OkStatus();
}

}  // namespace xfs
