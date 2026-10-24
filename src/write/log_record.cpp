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

// CRC of the record: the fixed header (with the CRC field zeroed), any
// xlog_rec_ext_header blocks, then the data area. Matches xlog_cksum(): each
// extension header contributes its 260 covered bytes, not the reserved tail.
uint32_t RecordChecksum(const uint8_t* header, size_t header_bytes,
                        const uint8_t* data, size_t data_len) {
  std::vector<uint8_t> fixed(header, header + lf::kRecCrcCovered);
  PutLE32(fixed.data(), lf::kRecCrc, 0);
  uint32_t crc = Crc32cUpdate(0xFFFFFFFFu, fixed.data(), fixed.size());
  for (size_t off = lf::kRecordHeaderSize; off + lf::kRecExtSize <= header_bytes;
       off += lf::kRecordHeaderSize) {
    crc = Crc32cUpdate(crc, header + off, lf::kRecExtSize);
  }
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
  // h_size is the iclog size. Records up to 32 KiB keep the historical
  // one-header-block shape; larger ones choose the smallest legal iclog size
  // (64/128/256 KiB) and carry xlog_rec_ext_header blocks for the extra cycle
  // words.  XLOG_MAX_RECORD_BSIZE bounds the whole record, not just the data.
  size_t hsize = lf::kDefaultRecordSize;
  size_t header_blocks = 1;
  if (rounded > lf::kHeaderCycleBytes) {
    while (true) {
      hsize *= 2;
      header_blocks = hsize / lf::kHeaderCycleBytes;
      if (hsize > lf::kMaxIclogBytes) {
        return XFS_ERR(XfsError::kUnsupportedFeature,
                       "log record exceeds XLOG_MAX_RECORD_BSIZE")
            .Metadata("log record")
            .Expected(lf::kMaxRecordBytes)
            .Actual(header_blocks * lf::kRecordHeaderSize + rounded);
      }
      if (header_blocks * lf::kRecordHeaderSize + rounded <= hsize) break;
    }
  }
  const size_t header_bytes = header_blocks * lf::kRecordHeaderSize;
  if (header_bytes + rounded > lf::kMaxRecordBytes) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "log record exceeds XLOG_MAX_RECORD_BSIZE")
        .Metadata("log record")
        .Expected(lf::kMaxRecordBytes)
        .Actual(header_bytes + rounded);
  }
  if (rounded / lf::kCycleBlockSize > header_blocks * lf::kRecCycleDataCount) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "log record needs more cycle-data slots than its headers")
        .Metadata("log record")
        .Actual(rounded / lf::kCycleBlockSize);
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

  // Header blocks: the fixed xlog_rec_header followed by one
  // xlog_rec_ext_header per additional 32 KiB of cycle-data words.
  std::vector<uint8_t> header(header_bytes, 0);
  uint8_t* h = header.data();
  PutBE32(h, lf::kRecMagic, lf::kRecordMagic);
  PutBE32(h, lf::kRecCycle, params.cycle);
  PutBE32(h, lf::kRecVersion, lf::kVersion2);
  PutBE32(h, lf::kRecLen, static_cast<uint32_t>(rounded));
  PutBE64(h, lf::kRecLsn, MakeLsn(params.cycle, params.block));
  PutBE64(h, lf::kRecTailLsn, params.tail_lsn);
  PutLE32(h, lf::kRecCrc, 0);
  PutBE32(h, lf::kRecPrevBlock, params.prev_block);
  PutBE32(h, lf::kRecNumLogOps, num_ops);
  PutBE32(h, lf::kRecFmt, lf::kFmtLinuxLe);
  std::memcpy(h + lf::kRecUuid, params.uuid.data(), 16);
  PutBE32(h, lf::kRecSize, static_cast<uint32_t>(hsize));
  PutBE32(h, lf::kRecPad0, 0);
  for (size_t i = 0; i < saved_words.size(); ++i) {
    if (i < lf::kRecCycleDataCount) {
      PutBE32(h, lf::kRecCycleData + i * 4, saved_words[i]);
    } else {
      const size_t ext = i / lf::kRecCycleDataCount;  // 1-based ext index
      const size_t slot = i % lf::kRecCycleDataCount;
      uint8_t* xh = header.data() + ext * lf::kRecordHeaderSize;
      if (slot == 0) PutBE32(xh, lf::kRecExtCycle, params.cycle);
      PutBE32(xh, lf::kRecExtData + slot * 4, saved_words[i]);
    }
  }

  std::vector<uint8_t> record(header.size() + rounded, 0);
  std::memcpy(record.data(), header.data(), header.size());
  std::memcpy(record.data() + header.size(), data.data(), data.size());

  // CRC over the fixed header (CRC field zero), extension headers and data.
  uint32_t crc = RecordChecksum(record.data(), header.size(), data.data(),
                                data.size());
  PutLE32(record.data(), lf::kRecCrc, crc);
  return record;
}

}  // namespace xfs
