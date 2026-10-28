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

#include "xfs/ag.h"
#include "xfs/crash.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/log.h"
#include "xfs/perf_stats.h"
#include "xfs/log_format.h"
#include "xfs/ondisk.h"
#include "xfs/transaction.h"

namespace xfs {
namespace {

namespace lf = logfmt;

// Room reserved at BeginTransaction so the record never has to be split across
// the physical end of the log.  XLOG_MAX_RECORD_BSIZE is 256 KiB; with the 8
// possible header blocks (main + 7 extension headers) the worst case is 520
// basic blocks.  The actual size is checked again at commit.
constexpr uint32_t kReservedRecordBlocks =
    (lf::kMaxRecordBytes + lf::kCycleBlockSize - 1) / lf::kCycleBlockSize;

// Data capacity of one 32 KiB log record (one 512-byte header block). The
// kernel writes all records with the mount's fixed iclog size; this writer
// keeps 32 KiB for compatibility with Linux-created logs and splits a large
// transaction across records instead of changing h_size mid-log.
constexpr size_t kMaxRecordData =
    lf::kHeaderCycleBytes - lf::kRecordHeaderSize;  // 32256

size_t OpWireSize(const LogOp& op) {
  return RoundUp64(lf::kOpHeaderSize + op.payload.size(), 4);
}

// Splits one transaction's item operations into a sequence of complete log
// records. The first record carries START + transaction header, the last one
// COMMIT. A split item payload uses CONTINUE on its first fragment and
// WAS_CONT on every later fragment, exactly the state machine the kernel and
// the recovery scanner implement.
std::vector<std::vector<LogOp>> FrameTransactionRecords(
    const std::vector<LogOp>& items, uint32_t tid) {
  std::vector<std::vector<LogOp>> records;
  std::vector<LogOp> current;
  size_t used = 0;

  auto flush = [&]() {
    if (!current.empty()) {
      records.push_back(std::move(current));
      current.clear();
      used = 0;
    }
  };
  auto append = [&](LogOp op) {
    used += OpWireSize(op);
    current.push_back(std::move(op));
  };
  auto max_fragment = [](size_t space) -> size_t {
    if (space <= lf::kOpHeaderSize) return 0;
    size_t take = space - lf::kOpHeaderSize;
    while (take > 0 &&
           RoundUp64(lf::kOpHeaderSize + take, 4) > space) {
      --take;
    }
    return take;
  };

  current.push_back(LogOp(lf::kFlagStartTrans, {}));
  used = OpWireSize(current.back());
  std::vector<uint8_t> trans_header;
  trans_header.reserve(lf::kTransHeaderSize);
  AppendTransHeader(&trans_header, tid, static_cast<uint32_t>(items.size()));
  append(LogOp(0, std::move(trans_header)));

  for (const LogOp& item : items) {
    const size_t wire = OpWireSize(item);
    if (used + wire <= kMaxRecordData) {
      append(item);
      continue;
    }
    if (item.payload.empty()) {
      flush();
      append(item);
      continue;
    }
    // Try to split the payload into the remaining space.
    size_t offset = 0;
    bool first_fragment = true;
    while (offset < item.payload.size()) {
      const size_t space =
          used < kMaxRecordData ? kMaxRecordData - used : 0;
      const size_t take =
          std::min(max_fragment(space), item.payload.size() - offset);
      if (take == 0) {
        flush();
        continue;
      }
      LogOp fragment;
      fragment.flags =
          item.flags |
          (first_fragment ? lf::kFlagContinueTrans : lf::kFlagWasContTrans);
      fragment.payload.assign(
          item.payload.begin() + static_cast<ptrdiff_t>(offset),
          item.payload.begin() +
              static_cast<ptrdiff_t>(offset + take));
      offset += take;
      first_fragment = false;
      append(std::move(fragment));
      if (offset < item.payload.size()) flush();
    }
  }

  LogOp commit(lf::kFlagCommitTrans, {});
  if (used + OpWireSize(commit) > kMaxRecordData) flush();
  append(std::move(commit));
  flush();
  return records;
}

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

size_t XfsTransaction::serialized_log_bytes() const {
  const auto records = FrameTransactionRecords(ops_, tid_);
  size_t total = 0;
  for (const auto& ops : records) {
    size_t data = 0;
    for (const LogOp& op : ops) data += OpWireSize(op);
    total += lf::kRecordHeaderSize + RoundUp64(data, lf::kCycleBlockSize);
  }
  return total;
}

bool XfsTransaction::record_fits() const {
  // The budget is the sum of the actual split records, including each
  // record's header block, capped at XLOG_MAX_RECORD_BSIZE.
  return serialized_log_bytes() <= lf::kMaxRecordBytes;
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
    if (recovery_tail_pin_.has_value()) {
      // The replayed window is still authoritative and must not be
      // overwritten.  Wrapping to block 0 is only safe when the pinned tail
      // starts after the reserved record space in the current cycle.  If the
      // active window already crosses block 0, the completion transaction
      // cannot be written until the intents are resolved; refusing is the
      // only crash-safe answer.
      const uint32_t pin_cycle =
          static_cast<uint32_t>(*recovery_tail_pin_ >> 32);
      const uint32_t pin_block = static_cast<uint32_t>(*recovery_tail_pin_);
      if (pin_cycle != log_position_.cycle ||
          pin_block < kReservedRecordBlocks) {
        return XFS_ERR(XfsError::kNoSpace,
                       "recovery tail is pinned inside the log wrap window; "
                       "cannot append a completion transaction without "
                       "overwriting unrecovered records")
            .Expected(log_position_.cycle)
            .Actual(pin_cycle)
            .Metadata("log");
      }
    }
    // Do not overwrite the records that still protect unflushed metadata.
    if (metadata_flush_pending_) {
      if (!device_->Flush()) {
        return XFS_ERR(XfsError::kIoError,
                       "failed to flush metadata before log wrap: " +
                           device_->LastError())
            .Metadata("log");
      }
      metadata_flush_pending_ = false;
      if (!recovery_tail_pin_.has_value()) {
        log_position_.tail_lsn =
            MakeLsn(log_position_.cycle, log_position_.next_block);
      }
    }
    XFS_DEBUG("log: wrapping to block 0 (cycle "
              << log_position_.cycle << " -> " << log_position_.cycle + 1
              << ")");
    ++log_position_.cycle;
    log_position_.next_block = 0;
    if (!recovery_tail_pin_.has_value()) {
      log_position_.tail_lsn = MakeLsn(log_position_.cycle, 0);
    }
  }

  PerfStats& perf = PerfStats::Get();
  if (perf.enabled()) {
    perf.journal_transactions.fetch_add(1, std::memory_order_relaxed);
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

  // Frame the item operations into one or more 32 KiB log records. The first
  // record carries START + the transaction header; the last one COMMIT. A
  // transaction larger than one record is split at op boundaries or inside a
  // payload (CONTINUE/WAS_CONT), never by changing h_size mid-log.
  const std::vector<std::vector<LogOp>> framed =
      FrameTransactionRecords(txn.ops_, txn.tid_);
  if (framed.empty()) {
    return XFS_ERR(XfsError::kInternal, "transaction produced no log records")
        .Metadata("transaction");
  }

  struct BuiltRecord {
    LogRecordParams params;
    std::vector<uint8_t> bytes;
  };
  std::vector<BuiltRecord> records;
  records.reserve(framed.size());
  uint32_t cycle = txn.cycle_;
  uint32_t block = txn.block_;
  uint32_t prev_block = log_position_.prev_block;
  uint32_t total_blocks = 0;
  for (size_t i = 0; i < framed.size(); ++i) {
    LogRecordParams params;
    params.cycle = cycle;
    params.block = block;
    params.tid = txn.tid_;
    params.prev_block = prev_block;
    // Every piece points at the oldest record still needed. During intent
    // completion that is the pinned original recovery window, not the first
    // completion piece: a crash before COMMIT must re-replay the RUI too.
    params.tail_lsn =
        recovery_tail_pin_.has_value()
            ? *recovery_tail_pin_
            : (metadata_flush_pending_ ? metadata_dirty_lsn_ : txn.lsn_);
    params.log_blocks = log_position_.log_blocks;
    params.record_size = lf::kDefaultRecordSize;
    params.uuid = geometry_.meta_uuid;
    auto built = BuildLogRecord(params, framed[i]);
    if (!built.ok()) return built.error();
    const uint32_t blocks =
        static_cast<uint32_t>(built.value().size() / lf::kCycleBlockSize);
    if (static_cast<uint64_t>(block) + blocks >
        log_position_.log_blocks) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "transaction does not fit in the reserved log space")
          .Metadata("transaction")
          .Expected(kReservedRecordBlocks)
          .Actual(total_blocks + blocks);
    }
    total_blocks += blocks;
    records.push_back(BuiltRecord{params, std::move(built.value())});
    prev_block = block;
    block += blocks;
    if (block == log_position_.log_blocks) {
      block = 0;
      ++cycle;
    } else if (block > log_position_.log_blocks) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "transaction log record crosses the log end")
          .Metadata("transaction");
    }
  }
  if (total_blocks > kReservedRecordBlocks) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "transaction exceeds the reserved log space")
        .Metadata("transaction")
        .Expected(kReservedRecordBlocks)
        .Actual(total_blocks);
  }

  MaybeCrash(CrashPoint::kBeforeLogWrite, TxnText(txn, "commit"));
  for (size_t i = 0; i < records.size(); ++i) {
    const BuiltRecord& record = records[i];
    const uint64_t record_offset =
        geometry_.FsbToByte(superblock_.logstart) +
        static_cast<uint64_t>(record.params.block) * lf::kCycleBlockSize;
    if (!device_->Write(record_offset, record.bytes.data(),
                        lf::kRecordHeaderSize)) {
      return XFS_ERR(XfsError::kIoError,
                     "failed to write log record header: " +
                         device_->LastError())
          .Offset(record_offset)
          .Metadata("log");
    }
    // A torn record (header without data, or a partial data area) fails the
    // record CRC and is trimmed by recovery. If an earlier piece was durable
    // but the COMMIT piece was not, the transaction is still uncommitted.
    // Keep this injection point per transaction (the harness uses hit counts
    // to select transactions), not per split record.
    if (i == 0) {
      MaybeCrash(CrashPoint::kMidLogRecord,
                 TxnText(txn, "record-header") + " records=" +
                     std::to_string(records.size()));
    }
    if (record.bytes.size() > lf::kRecordHeaderSize) {
      if (!device_->Write(record_offset + lf::kRecordHeaderSize,
                          record.bytes.data() + lf::kRecordHeaderSize,
                          record.bytes.size() - lf::kRecordHeaderSize)) {
        return XFS_ERR(XfsError::kIoError,
                       "failed to write log record data: " +
                           device_->LastError())
            .Offset(record_offset + lf::kRecordHeaderSize)
            .Metadata("log");
      }
    }
  }
  const uint64_t first_record_offset =
      geometry_.FsbToByte(superblock_.logstart) +
      static_cast<uint64_t>(txn.block_) * lf::kCycleBlockSize;
  if (!device_->Flush()) {
    return XFS_ERR(XfsError::kIoError,
                   "failed to flush log record: " + device_->LastError())
        .Offset(first_record_offset)
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
    // Metadata is written after the durable log record. The flush is delayed
    // until a durability boundary or log wrap: recovery replays the still
    // protected log records if power fails before this write-back reaches
    // stable storage. This is the XFS delayed-logging ordering, not a weaker
    // one.
    if (!metadata_flush_pending_) {
      metadata_flush_pending_ = true;
      metadata_dirty_lsn_ = txn.lsn_;
    }
    MaybeCrash(CrashPoint::kAfterMetadataWrite, TxnText(txn, "metadata"));
  }

  // The transaction is durable (its log record was flushed above). Advance the
  // write position. The tail stays at the oldest transaction whose metadata is
  // not yet on stable storage, so the log cannot be reused past it.
  log_position_.next_block = block;
  log_position_.cycle = cycle;
  if (log_position_.next_block >= log_position_.log_blocks) {
    log_position_.next_block = 0;
    ++log_position_.cycle;
  }
  log_position_.prev_block = records.back().params.block;
  log_position_.tail_lsn =
      recovery_tail_pin_.has_value()
          ? *recovery_tail_pin_
          : (metadata_flush_pending_
                 ? metadata_dirty_lsn_
                 : MakeLsn(log_position_.cycle, log_position_.next_block));
  log_dirty_ = true;
  txn.started_ = false;

  if (PerfStats::Get().enabled()) {
    PerfStats& perf = PerfStats::Get();
    perf.journal_commits.fetch_add(1, std::memory_order_relaxed);
    perf.journal_bytes.fetch_add(txn.log_bytes(),
                                 std::memory_order_relaxed);
    uint64_t metadata_bytes = 0;
    for (const TransactionWrite& write : txn.writes_) {
      metadata_bytes += write.data.size();
    }
    perf.metadata_write_calls.fetch_add(
        static_cast<uint64_t>(txn.writes_.size()),
        std::memory_order_relaxed);
    perf.metadata_write_bytes.fetch_add(metadata_bytes,
                                        std::memory_order_relaxed);
    // One durable log flush per commit. Metadata write-back is deliberately
    // batched until Flush/log-wrap/clean-unmount, so it is not a second log
    // barrier; device_flush_calls records the actual barriers.
    perf.journal_flushes.fetch_add(1, std::memory_order_relaxed);
  }

  for (const TransactionWrite& write : txn.writes_) {
    if (write.inode_metadata && write.ino != 0) {
      InvalidateCaches(write.data_fork_changed
                           ? CacheInvalidation::Extents(write.ino)
                           : CacheInvalidation::Inode(write.ino));
      // Any committed inode metadata carries a timestamp of its own; an older
      // deferred update must not overwrite it later.
      std::lock_guard<std::mutex> deferred_lock(deferred_mutex_);
      deferred_times_.erase(write.ino);
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
  // A clean log must not be followed by pending timestamp transactions.
  std::unique_lock<std::shared_mutex> barrier(write_barrier_);
  Status deferred = CommitAllDeferredTimesLocked();
  if (!deferred.ok()) return deferred;
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

  if (metadata_flush_pending_) {
    if (!device_->Flush()) {
      return XFS_ERR(XfsError::kIoError,
                     "failed to flush metadata before clean unmount: " +
                         device_->LastError())
          .Metadata("transaction");
    }
    metadata_flush_pending_ = false;
    if (!recovery_tail_pin_.has_value()) {
      log_position_.tail_lsn =
          MakeLsn(log_position_.cycle, log_position_.next_block);
    }
  }

  std::vector<LogOp> ops;
  ops.push_back(MakeUnmountOp());

  // Every durability step below is a separate write.  Before the log becomes
  // clean, make the on-disk superblock cover all metadata LSNs already written
  // by transactions; otherwise a crash between the unmount record and the
  // final stamp leaves a clean log whose metadata LSNs are ahead of sb_lsn.
  // The kernel has the same ordering: it covers the log (logging the
  // superblock) before writing the unmount record.
  if (log_dirty_) {
    Status stamped = StampSuperblockLsn(
        MakeLsn(log_position_.cycle, log_position_.next_block));
    if (!stamped.ok()) return stamped;
  }

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
  // The kernel stamps the superblock with the covering LSN when it closes the
  // log; every metadata LSN has to stay at or below it. Do the same before the
  // position moves on, so a later check of this clean image is consistent.
  Status stamped = StampSuperblockLsn(
      MakeLsn(params.cycle, params.block) /* the record just written */);
  if (!stamped.ok()) return stamped;
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

Status XfsFilesystem::FinishRecoveryClean() {
  // This method is deliberately restricted to the standalone recovery shape:
  // an ordinary writable mount keeps writing until its caller unmounts and
  // must not be silently downgraded.
  if (!options_.recovery_only) {
    return XFS_ERR(XfsError::kInternal,
                   "FinishRecoveryClean is recovery-only")
        .Metadata("recovery");
  }
  const bool was_writable = writable_;
  writable_ = true;
  Status status = OkStatus();

  if (!log_position_valid_) {
    auto position = ReadLogPosition(*device_, geometry_, superblock_);
    if (!position.ok()) {
      status = position.error();
    } else {
      log_position_ = position.value();
      log_position_valid_ = true;
      log_dirty_ = !position.value().clean;
      if (position.value().clean) {
        // The post-recovery cleanup either wrote nothing or already closed
        // the log; do not append a redundant unmount record.
        writable_ = was_writable;
        return OkStatus();
      }
    }
  }
  if (status.ok()) status = WriteCleanUnmountRecord();
  if (status.ok()) status = Flush();
  writable_ = was_writable;
  return status;
}

Status XfsFilesystem::RefreshSuperblockCounters() {
  // xfs_initialize_perag_data(): lazy superblock counters are not
  // authoritative after a crash. Sum the per-AG headers and adopt the result.
  uint64_t ifree = 0;
  uint64_t icount = 0;
  uint64_t fdblocks = 0;
  for (uint32_t agno = 0; agno < geometry_.agcount; ++agno) {
    auto agf = ReadAgf(*device_, geometry_, agno);
    if (!agf.ok()) return agf.error();
    auto agi = ReadAgi(*device_, geometry_, agno);
    if (!agi.ok()) return agi.error();
    ifree += agi.value().freecount;
    icount += agi.value().count;
    fdblocks += static_cast<uint64_t>(agf.value().freeblks) +
                agf.value().flcount + agf.value().btreeblks;
  }
  if (fdblocks > superblock_.dblocks || ifree > icount) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "AG headers disagree with the superblock counters")
        .Expected(superblock_.dblocks)
        .Actual(fdblocks)
        .Metadata("recovery");
  }
  superblock_.ifree = ifree;
  superblock_.icount = icount;
  superblock_.fdblocks = fdblocks;
  // The completion transactions read the counters back from the on-disk
  // superblock sector (ApplySuperblockDeltas), so persist the rebuilt values
  // now. The LSN stays as recovered; the clean close stamps it afterwards.
  const size_t sector = geometry_.sector_size;
  if (sector < ondisk::sb::kLsn + 8 || sector > (64u << 10)) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "cannot rewrite the superblock counters with this sector "
                   "size")
        .Expected(sector)
        .Metadata("superblock");
  }
  std::vector<uint8_t> buf(sector);
  if (!device_->Read(0, buf.data(), sector)) {
    return XFS_ERR(XfsError::kIoError,
                   "failed to read the superblock: " + device_->LastError())
        .Metadata("superblock");
  }
  PutBE64(buf.data(), ondisk::sb::kIcount, icount);
  PutBE64(buf.data(), ondisk::sb::kIfree, ifree);
  PutBE64(buf.data(), ondisk::sb::kFdblocks, fdblocks);
  UpdateXfsChecksum(buf.data(), sector, ondisk::sb::kCrc);
  if (!device_->Write(0, buf.data(), sector) || !device_->Flush()) {
    return XFS_ERR(XfsError::kIoError,
                   "failed to write the superblock counters: " +
                       device_->LastError())
        .Metadata("superblock");
  }
  XFS_SB_INFO("rebuilt superblock counters: icount=" << icount << " ifree="
                                                     << ifree << " fdblocks="
                                                     << fdblocks);
  return OkStatus();
}

Status XfsFilesystem::StampSuperblockLsn(uint64_t lsn) {
  const size_t sector = geometry_.sector_size;
  if (sector < ondisk::sb::kLsn + 8 || sector > (64u << 10)) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "cannot stamp the superblock LSN with this sector size")
        .Expected(sector)
        .Metadata("superblock");
  }
  std::vector<uint8_t> buf(sector);
  if (!device_->Read(0, buf.data(), sector)) {
    return XFS_ERR(XfsError::kIoError,
                   "failed to read the superblock: " + device_->LastError())
        .Metadata("superblock");
  }
  if (GetBE32(buf.data(), ondisk::sb::kMagic) != ondisk::kSbMagic) {
    return XFS_ERR(XfsError::kBadMagic, "superblock magic mismatch")
        .Metadata("superblock")
        .Expected(ondisk::kSbMagic)
        .Actual(GetBE32(buf.data(), ondisk::sb::kMagic));
  }
  PutBE64(buf.data(), ondisk::sb::kLsn, lsn);
  UpdateXfsChecksum(buf.data(), sector, ondisk::sb::kCrc);
  if (!device_->Write(0, buf.data(), sector) || !device_->Flush()) {
    return XFS_ERR(XfsError::kIoError,
                   "failed to write the superblock LSN: " +
                       device_->LastError())
        .Metadata("superblock");
  }
  superblock_.lsn = lsn;
  XFS_DEBUG("superblock LSN stamped: 0x" << std::hex << lsn << std::dec);
  return OkStatus();
}

}  // namespace xfs
