// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <vector>

#include "mkfs/mkfs_internal.h"
#include "xfs/log_record.h"

namespace xfs::mkfs::internal {
namespace {

constexpr size_t kSectorSize = 512;
// The unmount record's op id. mkfs.xfs uses a fixed pattern so a record that
// never was a transaction is recognizable in log dumps.
constexpr uint32_t kUnmountTid = 0xb0c0d0d0u;

}  // namespace

Result<std::vector<uint8_t>> BuildLogRecord(const FormatGeometry& geo) {
  LogRecordParams params;
  params.cycle = 1;  // XLOG_INIT_CYCLE
  params.block = 0;
  params.tid = kUnmountTid;
  params.prev_block = 0xffffffffu;  // there is no previous record
  params.tail_lsn = MakeLsn(1, 0);  // the record is its own tail
  params.log_blocks =
      static_cast<uint32_t>((geo.logblocks << geo.blocklog) / kSectorSize);
  params.uuid = geo.uuid;

  std::vector<LogOp> ops;
  ops.push_back(MakeUnmountOp());
  return BuildLogRecord(params, ops);
}

}  // namespace xfs::mkfs::internal
