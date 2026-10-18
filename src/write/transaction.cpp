// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - transaction commit and clean unmount record.
//
// Commit order (XFS write-ahead logging):
//   1. build the record (start op, transaction header, items, commit op)
//   2. write the record header block, then its data blocks
//   3. flush: the record must be durable *before* the metadata it describes
//   4. write the metadata buffers, flush
//   5. advance the write position and invalidate the caches
//
// The crash injection points sit between these steps; Linux recovery then has
// to produce either the old or the new state (docs/JOURNAL.md).
#include <algorithm>
#include <utility>

#include "xfs/crash.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/log.h"
#include "xfs/log_format.h"
#include "xfs/transaction.h"

namespace xfs {
namespace {

namespace lf = logfmt;

// Room reserved at BeginTransaction so the record never has to be split across
// the physical end of the log: header block + the largest record we can build
// (32 KiB of data, the limit before extended headers would be needed) + slack.
// The actual size is checked again at commit.
constexpr uint32_t kReservedRecordBlocks = 70;  // ~35 KiB of log space

std::string TxnText(const XfsTransaction& txn, const char* what) {
  return std::string(what) + " tid=" + std::to_string(txn.tid()) +
         " ino=" +
         (txn.writes().empty() ? std::string("-")
                               : std::to_string(txn.writes().front().ino)) +
         " lsn=0x" + [&] {
           char buf[32];
           std::snprintf(buf, sizeof(buf), "%llx",
                         static_cast<unsigned long long>(txn.lsn()));
           return std::string(buf);
         }();
}

}  // namespace

const std::vector<uint8_t>* XfsTransaction::QueuedBuffer(uint64_t offset) const {
  auto it = logged_buffers_.find(offset);
  if (it == logged_buffers_.end()) return nullptr;
  const size_t index = it->second.op_index;
  if (index + 1 >= ops_.size()) return nullptr;
  return &ops_[index + 1].payload;
}

size_t XfsTransaction::log_bytes() const {
  size_t bytes = 0;
  for (const LogOp& op : ops_) {
    bytes += lf::kOpHeaderSize + op.payload.size();
    bytes = RoundUp64(bytes, 4);
  }
  // start op + transaction header op + commit op
  bytes += (lf::kOpHeaderSize) + (lf::kOpHeaderSize + lf::kTransHeaderSize) +
           (lf::kOpHeaderSize);
  return bytes;
}

size_t XfsTransaction::record_bytes() const {
  return lf::kRecordHeaderSize + RoundUp64(log_bytes(), lf::kCycleBlockSize);
}

bool XfsTransaction::record_fits() const {
  return RoundUp64(log_bytes(), lf::kCycleBlockSize) <= lf::kHeaderCycleBytes;
}

Status XfsTransaction::AddMetadataBuffer(uint64_t offset,
                                         std::vector<uint8_t> data,
                                         uint16_t blf_type, size_t lsn_offset,
                                         size_t crc_offset,
                                         const char* what) {
  if (data.empty() || data.size() % 512 != 0 || data.size() > 0xffffull * 512) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "metadata buffer length must be a multiple of 512 bytes")
        .Offset(offset)
        .Metadata(what ? what : "metadata");
  }
  if (lsn_offset + 8 > data.size() || crc_offset + 4 > data.size()) {
    return XFS_ERR(XfsError::kInternal,
                   "metadata buffer too small for its lsn/crc fields")
        .Offset(offset)
        .Metadata(what ? what : "metadata");
  }
  if (offset % 512 != 0) {
    return XFS_ERR(XfsError::kInternal,
                   "metadata buffer is not sector aligned")
        .Offset(offset)
        .Metadata(what ? what : "metadata");
  }

  // On-disk LSN and CRC, exactly what the kernel's write verifier would stamp
  // when it flushes this buffer for our transaction.
  PutBE64(data.data(), lsn_offset, lsn_);
  UpdateXfsChecksum(data.data(), data.size(), crc_offset);

  // One buffer log format header plus the whole buffer as one contiguous run
  // of 128-byte chunks. `blf_size` counts the format plus the run(s).
  const size_t chunks = data.size() / lf::kBufChunkSize;
  const size_t map_words = (chunks + 31) / 32;
  std::vector<uint8_t> format(lf::kBufLogFormatSize + map_words * 4, 0);
  StoreLE16(format.data() + 0, lf::kItemBuf);
  StoreLE16(format.data() + 2, 2);
  StoreLE16(format.data() + 4,
            static_cast<uint16_t>(blf_type << lf::kBlftShift));
  StoreLE16(format.data() + 6, static_cast<uint16_t>(data.size() / 512));
  StoreLE32(format.data() + 8, static_cast<uint32_t>(offset / 512));
  StoreLE32(format.data() + 12,
            static_cast<uint32_t>((offset / 512) >> 32));
  StoreLE32(format.data() + 16, static_cast<uint32_t>(map_words));
  for (size_t i = 0; i < chunks; ++i) {
    // Host byte order bitmap (the kernel reads it with xfs_next_bit): bit i of
    // word i/32, least significant bit first.
    format[lf::kBufMapOffset + (i / 32) * 4 + (i % 32) / 8] |=
        static_cast<uint8_t>(1u << (i % 8));
  }

  // Replace a previous item for the same buffer (the caller re-dirtied it): the
  // log carries the final contents, once. A buffer always has the same size, so
  // the two operations can be replaced in place without shifting other items.
  auto existing = logged_buffers_.find(offset);
  if (existing != logged_buffers_.end()) {
    const size_t index = existing->second.op_index;
    ops_[index] = LogOp(0, std::move(format));
    ops_[index + 1] = LogOp(0, std::move(data));
    for (TransactionWrite& write : writes_) {
      if (write.offset == offset) {
        write.data = ops_[index + 1].payload;
        write.what = what ? what : "metadata";
        return OkStatus();
      }
    }
    TransactionWrite replacement;
    replacement.offset = offset;
    replacement.data = ops_[index + 1].payload;
    replacement.what = what ? what : "metadata";
    writes_.push_back(std::move(replacement));
    return OkStatus();
  }
  const size_t op_index = ops_.size();
  AddLogOp(std::move(format));
  AddLogOp(std::move(data));
  logged_buffers_[offset] = LoggedBufferRef{op_index};

  TransactionWrite write;
  write.offset = offset;
  write.data = ops_[op_index + 1].payload;
  write.what = what ? what : "metadata";
  writes_.push_back(std::move(write));
  return OkStatus();
}

Result<XfsTransaction> XfsFilesystem::BeginTransaction() {
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume,
                   "filesystem is mounted read-only")
        .Metadata("transaction");
  }
  if (!log_position_valid_) {
    auto position = ReadLogPosition(*device_, geometry_, superblock_);
    if (!position.ok()) return position.error();
    log_position_ = position.value();
    log_position_valid_ = true;
  }
  // Never split a record across the physical end of the log: if the reserved
  // space does not fit, wrap to block 0 and start a new cycle. The previous
  // record link stays valid (it points near the end of the log).
  if (static_cast<uint64_t>(log_position_.next_block) + kReservedRecordBlocks >
      log_position_.log_blocks) {
    XFS_DEBUG("log: wrapping to block 0 (cycle "
              << log_position_.cycle << " -> " << log_position_.cycle + 1
              << ")");
    ++log_position_.cycle;
    log_position_.next_block = 0;
    log_position_.tail_lsn = MakeLsn(log_position_.cycle, 0);
  }

  XfsTransaction txn;
  txn.tid_ = next_tid_++;
  if (next_tid_ == 0) next_tid_ = 1;
  txn.cycle_ = log_position_.cycle;
  txn.block_ = log_position_.next_block;
  txn.lsn_ = MakeLsn(txn.cycle_, txn.block_);
  txn.started_ = true;
  return txn;
}

Status XfsFilesystem::CommitTransaction(XfsTransaction& txn) {
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume, "filesystem is mounted read-only")
        .Metadata("transaction");
  }
  if (!txn.started_) {
    return XFS_ERR(XfsError::kInvalidArgument, "transaction was not started")
        .Metadata("transaction");
  }
  if (!log_position_valid_) {
    return XFS_ERR(XfsError::kInternal, "log position lost before commit")
        .Metadata("transaction");
  }

  // Frame the item operations: start op, transaction header, items, commit op.
  std::vector<LogOp> ops;
  ops.reserve(txn.ops_.size() + 3);
  ops.push_back(LogOp(lf::kFlagStartTrans, {}));
  std::vector<uint8_t> trans_header;
  trans_header.reserve(lf::kTransHeaderSize);
  AppendTransHeader(&trans_header, txn.tid_,
                    static_cast<uint32_t>(txn.ops_.size()));
  ops.push_back(LogOp(0, std::move(trans_header)));
  for (const LogOp& op : txn.ops_) ops.push_back(op);
  ops.push_back(LogOp(lf::kFlagCommitTrans, {}));

  LogRecordParams params;
  params.cycle = txn.cycle_;
  params.block = txn.block_;
  params.tid = txn.tid_;
  params.prev_block = log_position_.prev_block;
  params.tail_lsn = log_position_.tail_lsn;
  params.log_blocks = log_position_.log_blocks;
  params.record_size = log_position_.record_size;
  params.uuid = geometry_.meta_uuid;
  auto record = BuildLogRecord(params, ops);
  if (!record.ok()) return record.error();

  const size_t record_bytes = record.value().size();
  const uint32_t record_blocks =
      static_cast<uint32_t>(record_bytes / lf::kCycleBlockSize);
  if (static_cast<uint64_t>(txn.block_) + record_blocks >
      log_position_.log_blocks) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "transaction does not fit in the reserved log space")
        .Metadata("transaction")
        .Expected(kReservedRecordBlocks)
        .Actual(record_blocks);
  }
  const uint64_t record_offset =
      geometry_.FsbToByte(superblock_.logstart) +
      static_cast<uint64_t>(txn.block_) * lf::kCycleBlockSize;

  MaybeCrash(CrashPoint::kBeforeLogWrite, TxnText(txn, "commit"));
  if (!device_->Write(record_offset, record.value().data(),
                      lf::kRecordHeaderSize)) {
    return XFS_ERR(XfsError::kIoError,
                   "failed to write log record header: " +
                       device_->LastError())
        .Offset(record_offset)
        .Metadata("log");
  }
  // A torn record (header without data, or a partial data area) fails the
  // record CRC and is trimmed by recovery.
  MaybeCrash(CrashPoint::kMidLogRecord, TxnText(txn, "record-header"));
  if (record_bytes > lf::kRecordHeaderSize) {
    if (!device_->Write(record_offset + lf::kRecordHeaderSize,
                        record.value().data() + lf::kRecordHeaderSize,
                        record_bytes - lf::kRecordHeaderSize)) {
      return XFS_ERR(XfsError::kIoError,
                     "failed to write log record data: " +
                         device_->LastError())
          .Offset(record_offset + lf::kRecordHeaderSize)
          .Metadata("log");
    }
  }
  if (!device_->Flush()) {
    return XFS_ERR(XfsError::kIoError,
                   "failed to flush log record: " + device_->LastError())
        .Offset(record_offset)
        .Metadata("log");
  }
  MaybeCrash(CrashPoint::kAfterLogCommit, TxnText(txn, "committed"));

  // Metadata write-back. The transaction LSN is already stamped into every
  // buffer by the item builder, so recovery can skip older changes.
  for (size_t i = 0; i < txn.writes_.size(); ++i) {
    const TransactionWrite& write = txn.writes_[i];
    if (i == 0) {
      MaybeCrash(CrashPoint::kBeforeMetadataWrite, TxnText(txn, "metadata"));
    } else {
      MaybeCrash(CrashPoint::kMidMetadataWrite, TxnText(txn, "metadata"));
    }
    if (!device_->Write(write.offset, write.data.data(), write.data.size())) {
      return XFS_ERR(XfsError::kIoError,
                     std::string("failed to write ") + write.what + ": " +
                         device_->LastError())
          .Offset(write.offset)
          .Ino(write.ino)
          .Metadata(write.what);
    }
  }
  if (!txn.writes_.empty()) {
    if (!device_->Flush()) {
      return XFS_ERR(XfsError::kIoError,
                     "failed to flush metadata: " + device_->LastError())
          .Metadata("transaction");
    }
    MaybeCrash(CrashPoint::kAfterMetadataWrite, TxnText(txn, "metadata"));
  }

  // The transaction is durable; advance the write position. The next record's
  // tail is its own LSN because every buffer this one logged is now on disk.
  log_position_.next_block += record_blocks;
  if (log_position_.next_block >= log_position_.log_blocks) {
    log_position_.next_block = 0;
    ++log_position_.cycle;
  }
  log_position_.prev_block = txn.block_;
  log_position_.tail_lsn =
      MakeLsn(log_position_.cycle, log_position_.next_block);
  log_dirty_ = true;
  txn.started_ = false;

  for (const TransactionWrite& write : txn.writes_) {
    if (write.inode_metadata && write.ino != 0) {
      InvalidateCaches(write.data_fork_changed
                           ? CacheInvalidation::Extents(write.ino)
                           : CacheInvalidation::Inode(write.ino));
    }
  }
  XFS_TRACE("transaction committed: tid=" << txn.tid() << " lsn=0x" << std::hex
                                          << txn.lsn() << std::dec
                                          << " items=" << txn.ops_.size()
                                          << " writes=" << txn.writes_.size());
  return OkStatus();
}

Status XfsFilesystem::RunTransaction(
    const std::function<Status(XfsTransaction&)>& build) {
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume, "filesystem is mounted read-only")
        .Metadata("transaction");
  }
  std::lock_guard<std::mutex> lock(mutation_mutex_);
  auto txn = BeginTransaction();
  if (!txn.ok()) return txn.error();
  // If building fails nothing has been written: there is no record yet, so the
  // transaction simply never happened.
  Status status = build(txn.value());
  if (!status.ok()) return status;
  return CommitTransaction(txn.value());
}

Status XfsFilesystem::WriteCleanUnmountRecord() {
  if (!writable_) return OkStatus();
  std::lock_guard<std::mutex> lock(mutation_mutex_);
  if (!log_dirty_ && log_position_valid_) {
    return OkStatus();  // the log already ends with an unmount record
  }
  if (!log_position_valid_) {
    auto position = ReadLogPosition(*device_, geometry_, superblock_);
    if (!position.ok()) {
      // Nothing was written by us, or the log is already clean.
      if (position.error().code == XfsError::kDirtyLog) {
        XFS_ERROR("cannot write clean unmount record: "
                  << position.error().Format());
      }
      return position.error();
    }
    log_position_ = position.value();
    log_position_valid_ = true;
  }

  std::vector<LogOp> ops;
  ops.push_back(MakeUnmountOp());
  LogRecordParams params;
  params.cycle = log_position_.cycle;
  params.block = log_position_.next_block;
  params.tid = 0x55554e4d;  // 'UNMD': the unmount record has no transaction id
  params.prev_block = log_position_.prev_block;
  params.tail_lsn = log_position_.tail_lsn;
  params.log_blocks = log_position_.log_blocks;
  params.record_size = log_position_.record_size;
  params.uuid = geometry_.meta_uuid;
  auto record = BuildLogRecord(params, ops);
  if (!record.ok()) return record.error();
  const uint32_t record_blocks =
      static_cast<uint32_t>(record.value().size() / lf::kCycleBlockSize);
  const uint64_t record_offset =
      geometry_.FsbToByte(superblock_.logstart) +
      static_cast<uint64_t>(log_position_.next_block) *
          lf::kCycleBlockSize;
  MaybeCrash(CrashPoint::kBeforeLogWrite, "clean-unmount");
  if (!device_->Write(record_offset, record.value().data(),
                      record.value().size())) {
    return XFS_ERR(XfsError::kIoError,
                   "failed to write clean unmount record: " +
                       device_->LastError())
        .Offset(record_offset)
        .Metadata("log");
  }
  if (!device_->Flush()) {
    return XFS_ERR(XfsError::kIoError,
                   "failed to flush clean unmount record: " +
                       device_->LastError())
        .Offset(record_offset)
        .Metadata("log");
  }
  MaybeCrash(CrashPoint::kAfterLogCommit, "clean-unmount");
  log_position_.next_block += record_blocks;
  if (log_position_.next_block >= log_position_.log_blocks) {
    log_position_.next_block = 0;
    ++log_position_.cycle;
  }
  log_position_.prev_block = params.block;
  log_position_.tail_lsn =
      MakeLsn(log_position_.cycle, log_position_.next_block);
  log_dirty_ = false;
  log_position_valid_ = false;  // a new mutation rescans
  XFS_DEBUG("clean unmount record written at block " << params.block
                                                     << " cycle "
                                                     << params.cycle);
  return OkStatus();
}

}  // namespace xfs
