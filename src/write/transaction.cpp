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
#include <cstring>
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
  // The write-back copy is always the complete buffer: the log item only holds
  // the chunks that differ from disk.
  for (const TransactionWrite& write : writes_) {
    if (write.offset == offset) return &write.data;
  }
  return nullptr;
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

Status XfsTransaction::AddOrderedBuffer(uint64_t offset,
                                        std::vector<uint8_t> data,
                                        uint64_t ino, const char* what) {
  if (data.empty() || data.size() % 512 != 0 ||
      data.size() > 0xffffull * 512) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "metadata buffer length must be a multiple of 512 bytes")
        .Offset(offset)
        .Metadata(what ? what : "metadata");
  }
  if (offset % 512 != 0) {
    return XFS_ERR(XfsError::kInternal,
                   "metadata buffer is not sector aligned")
        .Offset(offset)
        .Metadata(what ? what : "metadata");
  }
  TransactionWrite write;
  write.offset = offset;
  write.data = std::move(data);
  write.ino = ino;
  write.inode_metadata = ino != 0;
  write.data_fork_changed = false;
  write.what = what ? what : "metadata";
  writes_.push_back(std::move(write));
  return OkStatus();
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

  // Dirty chunks: those that differ from the on-disk image. By the time a
  // transaction is built, every earlier transaction has already written and
  // flushed its metadata, so the difference to disk is exactly what this
  // transaction changed - the same set of regions the kernel would have
  // dirtied and logged. Logging the whole buffer instead would exceed the log
  // record limit as soon as one transaction touches several b-trees.
  const size_t chunks = data.size() / lf::kBufChunkSize;
  const size_t map_words = (chunks + 31) / 32;
  std::vector<uint8_t> map(map_words * 4, 0);
  std::vector<uint8_t> logged;
  // Contiguous runs of dirty chunks. The kernel logs one region per run
  // (xfs_buf_item_format_segment) and recovery consumes one region per run
  // (xlog_recover_do_reg_buffer), so the two have to match exactly.
  struct ChunkRun {
    size_t first;
    size_t count;
  };
  std::vector<ChunkRun> runs;
  const bool have_disk = device_ != nullptr;
  std::vector<uint8_t> disk;
  if (have_disk) {
    disk.assign(data.size(), 0);
    if (!device_->Read(offset, disk.data(), disk.size())) {
      return XFS_ERR(XfsError::kIoError,
                     "failed to read a metadata buffer before logging it: " +
                         device_->LastError())
          .Offset(offset)
          .Metadata(what ? what : "metadata");
    }
  }
  for (size_t i = 0; i < chunks; ++i) {
    const size_t at = i * lf::kBufChunkSize;
    if (have_disk && std::memcmp(disk.data() + at, data.data() + at,
                                 lf::kBufChunkSize) == 0) {
      continue;
    }
    map[(i / 32) * 4 + (i % 32) / 8] |= static_cast<uint8_t>(1u << (i % 8));
    if (!runs.empty() && runs.back().first + runs.back().count == i) {
      ++runs.back().count;
    } else {
      runs.push_back(ChunkRun{i, 1});
    }
    logged.insert(logged.end(), data.data() + at,
                  data.data() + at + lf::kBufChunkSize);
  }

  // Replace a previous item for the same buffer (the caller re-dirtied it): the
  // log carries the final contents, once. A buffer always has the same size, so
  // the format stays the same size; the number of regions can shrink or grow,
  // which the splice below accounts for.
  auto existing = logged_buffers_.find(offset);
  if (runs.empty()) {
    if (existing == logged_buffers_.end()) {
      // Nothing changed on disk and nothing was logged before: no item is
      // needed. The write-back still happens (the caller may have queued the
      // buffer for its cache handling and for the LSN stamp).
      for (TransactionWrite& write : writes_) {
        if (write.offset == offset) {
          write.data = std::move(data);
          write.what = what ? what : "metadata";
          return OkStatus();
        }
      }
      TransactionWrite write;
      write.offset = offset;
      write.data = std::move(data);
      write.what = what ? what : "metadata";
      writes_.push_back(std::move(write));
      return OkStatus();
    }
    // A previously logged version of this buffer is already in the record, but
    // the buffer came back to its on-disk contents. Log one chunk of the
    // current contents: replaying it is a no-op, and the item stays
    // well-formed.
    logged.assign(data.data(), data.data() + lf::kBufChunkSize);
    map[0] |= 1;
    runs.push_back(ChunkRun{0, 1});
  }

  // One buffer log format header plus one region per contiguous run.
  std::vector<uint8_t> format(lf::kBufLogFormatSize + map_words * 4, 0);
  StoreLE16(format.data() + 0, lf::kItemBuf);
  StoreLE16(format.data() + 2, static_cast<uint16_t>(1 + runs.size()));
  StoreLE16(format.data() + 4,
            static_cast<uint16_t>(blf_type << lf::kBlftShift));
  StoreLE16(format.data() + 6, static_cast<uint16_t>(data.size() / 512));
  StoreLE32(format.data() + 8, static_cast<uint32_t>(offset / 512));
  StoreLE32(format.data() + 12,
            static_cast<uint32_t>((offset / 512) >> 32));
  StoreLE32(format.data() + 16, static_cast<uint32_t>(map_words));
  std::memcpy(format.data() + lf::kBufMapOffset, map.data(), map.size());

  // The item's payloads: the format header followed by one region per run, in
  // map order.
  std::vector<std::vector<uint8_t>> item;
  item.push_back(std::move(format));
  size_t run_at = 0;
  for (const ChunkRun& run : runs) {
    const size_t bytes = run.count * lf::kBufChunkSize;
    item.emplace_back(logged.begin() + static_cast<ptrdiff_t>(run_at),
                      logged.begin() + static_cast<ptrdiff_t>(run_at + bytes));
    run_at += bytes;
  }

  if (existing != logged_buffers_.end()) {
    const size_t old_index = existing->second.op_index;
    const size_t old_count = existing->second.op_count;
    ops_.erase(ops_.begin() + static_cast<ptrdiff_t>(old_index),
               ops_.begin() + static_cast<ptrdiff_t>(old_index + old_count));
    auto at = ops_.begin() + static_cast<ptrdiff_t>(old_index);
    for (std::vector<uint8_t>& payload : item) {
      at = ops_.insert(at, LogOp(0, std::move(payload)));
      ++at;
    }
    const ptrdiff_t delta = static_cast<ptrdiff_t>(item.size()) -
                            static_cast<ptrdiff_t>(old_count);
    if (delta != 0) {
      for (auto& entry : logged_buffers_) {
        if (entry.second.op_index > old_index) {
          entry.second.op_index = static_cast<size_t>(
              static_cast<ptrdiff_t>(entry.second.op_index) + delta);
        }
      }
    }
    existing->second.op_count = item.size();
    for (TransactionWrite& write : writes_) {
      if (write.offset == offset) {
        write.data = std::move(data);
        write.what = what ? what : "metadata";
        return OkStatus();
      }
    }
    TransactionWrite replacement;
    replacement.offset = offset;
    replacement.data = std::move(data);
    replacement.what = what ? what : "metadata";
    writes_.push_back(std::move(replacement));
    return OkStatus();
  }
  const size_t op_index = ops_.size();
  for (std::vector<uint8_t>& payload : item) {
    AddLogOp(std::move(payload));
  }
  logged_buffers_[offset] = LoggedBufferRef{op_index, item.size()};

  TransactionWrite write;
  write.offset = offset;
  write.data = std::move(data);
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
  txn.device_ = device_.get();
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
  // tail is its own LSN because every buffer this one logged is now on disk -
  // unless a recovered intent is still unresolved: then the tail stays pinned
  // to the replayed window so a crash here re-replays it (and the done item
  // this transaction logged releases the intent).
  log_position_.next_block += record_blocks;
  if (log_position_.next_block >= log_position_.log_blocks) {
    log_position_.next_block = 0;
    ++log_position_.cycle;
  }
  log_position_.prev_block = txn.block_;
  log_position_.tail_lsn = recovery_tail_pin_.has_value()
                               ? *recovery_tail_pin_
                               : MakeLsn(log_position_.cycle,
                                         log_position_.next_block);
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
