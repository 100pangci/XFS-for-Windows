// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// libFuzzer target: log scan + transaction reconstruction + replay.
//
// The record framing is valid and stable; the fuzzer controls one item
// payload (its type and all fields). This reaches the item decoder in
// ScanLog/ReplayLog without spending mutations on log headers and CRCs.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xfs/endian.h"
#include "xfs/log_format.h"
#include "xfs/log_record.h"
#include "xfs/recovery.h"
#include "xfs/superblock.h"

#include "fuzz_support.h"

namespace {

using namespace xfs;
namespace lf = logfmt;

constexpr uint64_t kLogStartBlock = 8;   // fs block
constexpr uint32_t kLogBlocks = 8;       // fs blocks (32 KiB log)
constexpr uint32_t kLogBasicBlocks = (kLogBlocks * 4096) / 512;

XfsSuperblock MakeSuperblock() {
  XfsSuperblock sb;
  sb.blocksize = 4096;
  sb.blocklog = 12;
  sb.sectsize = 512;
  sb.sectlog = 9;
  sb.inodesize = 512;
  sb.inodelog = 9;
  sb.inopblock = 8;
  sb.inopblog = 3;
  sb.agblocks = 65536;
  sb.agblklog = 16;
  sb.agcount = 1;
  sb.dblocks = 65536;
  sb.logstart = kLogStartBlock;
  sb.logblocks = kLogBlocks;
  sb.logsectsize = 512;
  sb.logsectlog = 9;
  sb.versionnum = 0xb4a5;
  sb.uuid = fuzz::kFuzzUuid;
  sb.meta_uuid = fuzz::kFuzzUuid;
  return sb;
}

bool WriteAt(fuzz::MemoryDevice* device, uint64_t offset,
             const std::vector<uint8_t>& bytes) {
  if (offset > device->Size() || bytes.size() > device->Size() - offset) {
    return false;
  }
  std::memcpy(device->bytes().data() + offset, bytes.data(), bytes.size());
  return true;
}

void Exercise(const uint8_t* data, size_t size) {
  const XfsSuperblock sb = MakeSuperblock();
  const XfsGeometry geo = MakeGeometry(sb);
  const uint64_t log_offset = geo.FsbToByte(kLogStartBlock);

  // A clean unmount record establishes the log state a fresh mkfs leaves.
  LogRecordParams clean;
  clean.cycle = 1;
  clean.block = 0;
  clean.tid = 0xb0c0d0d0u;
  clean.prev_block = 0xffffffffu;
  clean.tail_lsn = MakeLsn(1, 0);
  clean.log_blocks = kLogBasicBlocks;
  clean.uuid = fuzz::kFuzzUuid;
  auto unmount = BuildLogRecord(clean, {MakeUnmountOp()});
  if (!unmount.ok()) return;

  // One transaction at block 2 with a single committed item. The item format
  // header is 4 bytes: type (LE16) and region count (LE16). Force a single
  // region so the rest of the fuzzed bytes are the item's own fields.
  std::vector<uint8_t> item(std::max<size_t>(size, 4), 0);
  if (size != 0) std::memcpy(item.data(), data, size);
  StoreLE16(item.data() + 2, 1);

  std::vector<uint8_t> transaction_header;
  AppendTransHeader(&transaction_header, 1, 1);
  std::vector<LogOp> ops;
  ops.emplace_back(lf::kFlagStartTrans, std::vector<uint8_t>{});
  ops.emplace_back(0, transaction_header);
  ops.emplace_back(0, item);
  ops.emplace_back(lf::kFlagCommitTrans, std::vector<uint8_t>{});

  LogRecordParams dirty;
  dirty.cycle = 1;
  dirty.block = 2;
  dirty.tid = 1;
  dirty.prev_block = 0;
  dirty.tail_lsn = MakeLsn(1, 2);
  dirty.log_blocks = kLogBasicBlocks;
  dirty.uuid = fuzz::kFuzzUuid;
  auto record = BuildLogRecord(dirty, ops);
  if (!record.ok()) return;

  fuzz::MemoryDevice device(log_offset + (kLogBlocks << 12));
  if (!WriteAt(&device, log_offset, unmount.value())) return;
  if (!WriteAt(&device, log_offset + 2 * 512, record.value())) return;

  auto scan = ScanLog(device, sb, geo);
  if (!scan.ok()) return;
  (void)CollectPendingIntents(scan.value());
  if (!scan.value().Recoverable()) return;

  RecoveredLogState state;
  (void)ReplayLog(device, sb, geo, scan.value(), &state);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  Exercise(data, size);
  return 0;
}
