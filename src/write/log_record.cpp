// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/log_record.h"

#include <algorithm>
#include <cstring>

#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/log_format.h"

namespace xfs {
namespace {

namespace lf = logfmt;

// Payload data is padded so that the next op header starts 4-byte aligned.
constexpr size_t kOpAlign = 4;

void AppendZeroes(std::vector<uint8_t>* data, size_t count) {
  data->insert(data->end(), count, 0);
}

// CRC of the record: header (with the CRC field zeroed) + data area. Matches
// xlog_cksum(): seed ~0, streamed over header then payload, final inversion.
uint32_t RecordChecksum(const uint8_t* header, const uint8_t* data,
                        size_t data_len) {
  uint32_t crc = Crc32cUpdate(0xFFFFFFFFu, header, lf::kRecCrcCovered);
  if (data_len) crc = Crc32cUpdate(crc, data, data_len);
  return ~crc;
}

}  // namespace

void PutLE16(std::vector<uint8_t>* data, size_t off, uint16_t value) {
  if (data->size() < off + 2) data->resize(off + 2, 0);
  StoreLE16(data->data() + off, value);
}

void PutLE32(std::vector<uint8_t>* data, size_t off, uint32_t value) {
  if (data->size() < off + 4) data->resize(off + 4, 0);
  StoreLE32(data->data() + off, value);
}

void PutLE64(std::vector<uint8_t>* data, size_t off, uint64_t value) {
  if (data->size() < off + 8) data->resize(off + 8, 0);
  uint8_t* p = data->data() + off;
  StoreLE32(p, static_cast<uint32_t>(value & 0xffffffffu));
  StoreLE32(p + 4, static_cast<uint32_t>(value >> 32));
}

void AppendLogOp(std::vector<uint8_t>* data, uint32_t tid, const LogOp& op) {
  if (op.payload.size() > 0x7fffffffu) return;  // caller keeps ops small
  const size_t start = data->size();
  data->resize(start + lf::kOpHeaderSize, 0);
  uint8_t* p = data->data() + start;
  PutBE32(p, lf::kOpTid, tid);
  PutBE32(p, lf::kOpLen, static_cast<uint32_t>(op.payload.size()));
  p[lf::kOpClientId] = op.client_id;
  p[lf::kOpFlags] = op.flags;
  PutBE16(p, lf::kOpRes2, 0);
  data->insert(data->end(), op.payload.begin(), op.payload.end());
  // The next op header must be 4-byte aligned.
  const size_t used = data->size() - start;
  const size_t pad = (kOpAlign - (used % kOpAlign)) % kOpAlign;
  AppendZeroes(data, pad);
}

void AppendTransHeader(std::vector<uint8_t>* data, uint32_t tid,
                       uint32_t num_items) {
  const size_t off = data->size();
  data->resize(off + lf::kTransHeaderSize, 0);
  uint8_t* p = data->data() + off;
  StoreLE32(p + lf::kThMagic, lf::kTransHeaderMagic);
  StoreLE32(p + lf::kThType, lf::kTransTypeCheckpoint);
  StoreLE32(p + lf::kThTid, tid);
  StoreLE32(p + lf::kThNumItems, num_items);
}

LogOp MakeUnmountOp() {
  LogOp op;
  op.client_id = lf::kClientLog;
  op.flags = lf::kFlagUnmountTrans;
  // The unmount record is written by the log, not by a transaction, so it uses
  // the XFS_LOG client id. Its payload is xfs_unmount_log_format, whose magic
  // the kernel writes in host byte order (a plain assignment to a __be16
  // field); nothing reads it (recovery only looks at the flag), but matching
  // the kernel keeps log dumps identical.
  op.payload.assign(lf::kUnmountFormatSize, 0);
  StoreLE16(op.payload.data(), lf::kUnmountType);
  return op;
}

Result<std::vector<uint8_t>> BuildLogRecord(const LogRecordParams& params,
                                            const std::vector<LogOp>& ops) {
  if (params.log_blocks == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "log size not set")
        .Metadata("log record");
  }
  if (params.block >= params.log_blocks) {
    return XFS_ERR(XfsError::kOutOfRange, "log record block out of range")
        .Metadata("log record")
        .Expected(params.log_blocks)
        .Actual(params.block);
  }
  if (params.cycle == 0 || params.cycle == 0xFFFFFFFFu) {
    // 0 is used by zeroed (unwritten) log space, 0xFFFFFFFF marks an invalid
    // header cycle.
    return XFS_ERR(XfsError::kInvalidArgument, "invalid log cycle")
        .Metadata("log record")
        .Actual(params.cycle);
  }
  if (ops.empty()) {
    return XFS_ERR(XfsError::kInvalidArgument, "empty log record")
        .Metadata("log record");
  }

  // Assemble the operation chain.
  std::vector<uint8_t> data;
  data.reserve(4096);
  uint32_t num_ops = 0;
  for (const LogOp& op : ops) {
    AppendLogOp(&data, params.tid, op);
    ++num_ops;
  }
  // h_len is the data length rounded up so that header + data is a multiple of
  // the log sector size (512 in this profile); recovery computes the number of
  // data blocks as BTOBB(h_len), so it must be exact.
  const size_t rounded = RoundUp64(data.size(), lf::kCycleBlockSize);
  if (rounded > lf::kHeaderCycleBytes) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "log record larger than 32 KiB would need extension "
                   "headers")
        .Metadata("log record")
        .Expected(lf::kHeaderCycleBytes)
        .Actual(rounded);
  }
  AppendZeroes(&data, rounded - data.size());

  // The whole record (header + data) must fit before the physical end of the
  // log; the transaction layer never splits a record across the end.
  const uint64_t log_bytes =
      static_cast<uint64_t>(params.log_blocks) * lf::kCycleBlockSize;
  if (lf::kRecordHeaderSize + rounded > log_bytes) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "log record does not fit in the log")
        .Metadata("log record")
        .Expected(log_bytes)
        .Actual(lf::kRecordHeaderSize + rounded);
  }

  // Block cycle stamping: the first word of every 512-byte data block becomes
  // the record cycle; the original words go into the header.
  std::vector<uint32_t> saved_words(rounded / lf::kCycleBlockSize, 0);
  for (size_t i = 0; i < saved_words.size(); ++i) {
    uint8_t* block = data.data() + i * lf::kCycleBlockSize;
    saved_words[i] = GetBE32(block, 0);
    PutBE32(block, 0, params.cycle);
  }

  // Header block.
  std::vector<uint8_t> record(lf::kRecordHeaderSize + rounded, 0);
  uint8_t* h = record.data();
  PutBE32(h, lf::kRecMagic, lf::kRecordMagic);
  PutBE32(h, lf::kRecCycle, params.cycle);
  PutBE32(h, lf::kRecVersion, lf::kVersion2);
  PutBE32(h, lf::kRecLen, static_cast<uint32_t>(rounded));
  PutBE64(h, lf::kRecLsn, MakeLsn(params.cycle, params.block));
  PutBE64(h, lf::kRecTailLsn, params.tail_lsn);
  PutLE32(h, lf::kRecCrc, 0);
  PutBE32(h, lf::kRecPrevBlock, params.prev_block);
  PutBE32(h, lf::kRecNumLogOps, num_ops);
  for (size_t i = 0; i < saved_words.size(); ++i) {
    PutBE32(h, lf::kRecCycleData + i * 4, saved_words[i]);
  }
  PutBE32(h, lf::kRecFmt, lf::kFmtLinuxLe);
  std::memcpy(h + lf::kRecUuid, params.uuid.data(), 16);
  PutBE32(h, lf::kRecSize,
          params.record_size ? params.record_size : lf::kDefaultRecordSize);
  PutBE32(h, lf::kRecPad0, 0);
  std::memcpy(record.data() + lf::kRecordHeaderSize, data.data(), data.size());

  // CRC over the header (CRC field zero) plus the data area.
  uint32_t crc = RecordChecksum(record.data(), record.data() + lf::kRecordHeaderSize,
                                data.size());
  PutLE32(record.data(), lf::kRecCrc, crc);
  return record;
}

}  // namespace xfs
