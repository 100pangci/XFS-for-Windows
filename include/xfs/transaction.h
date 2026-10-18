// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - transaction layer.
//
// Every metadata mutation goes through a transaction:
//
//   XfsTransaction      in-memory: log items + the metadata buffers they protect
//        |
//        v  XfsFilesystem::CommitTransaction
//   log record written and flushed   (write-ahead: the log record must be
//        |                            durable before the metadata it describes)
//        v
//   metadata buffers written and flushed
//        |
//        v
//   log position advanced, caches invalidated
//
// A crash between any two steps leaves either the old state (record not
// committed) or the committed state (record replayable), never a half-applied
// transaction, because recovery only applies complete, committed records.
//
// Callers must hold the filesystem's mutation lock while a transaction is open
// (all built-in mutations do; RunTransaction takes it for them).
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "xfs/error.h"
#include "xfs/log_record.h"

namespace xfs {

class BlockDevice;

// One metadata buffer that becomes visible when the transaction commits.
struct TransactionWrite {
  uint64_t offset = 0;          // device byte offset of the buffer
  std::vector<uint8_t> data;    // complete new contents (LSN already stamped)
  uint64_t ino = 0;             // subject inode's cache entries (0 = none)
  bool inode_metadata = false;  // invalidate parsed inode + file info
  // True when the inode's data fork changed (extent map): all parsed extent
  // lists and file data chunks of that inode must be dropped as well.
  bool data_fork_changed = false;
  const char* what = "metadata";
};

class XfsTransaction {
 public:
  XfsTransaction() = default;

  bool started() const { return started_; }
  uint32_t tid() const { return tid_; }
  // LSN of the record that will carry this transaction. Metadata written by
  // the transaction must store this value in its on-disk LSN field.
  uint64_t lsn() const { return lsn_; }
  uint32_t cycle() const { return cycle_; }
  uint32_t block() const { return block_; }

  // Appends one logged region (an item format header or an item payload) as a
  // log operation. The transaction framing (start/commit) is added at commit.
  void AddLogOp(std::vector<uint8_t> payload) {
    ops_.push_back(LogOp(0, std::move(payload)));
  }
  void AddWrite(TransactionWrite write) { writes_.push_back(std::move(write)); }

  // Adds one metadata buffer to the transaction as an XFS_LI_BUF item:
  //   - stamps the transaction LSN (big-endian) at `lsn_offset` - the value the
  //     kernel's buffer write verifier would store,
  //   - recomputes the checksum at `crc_offset`,
  //   - logs only the 128-byte chunks whose contents differ from the on-disk
  //     image, exactly the dirty regions the kernel would log (the difference
  //     to disk is what this transaction changed, because the previous
  //     transaction wrote and flushed its metadata before this one was built),
  //   - queues the buffer write-back after the log record is durable.
  // Adding the same device offset twice keeps the latest contents and logs the
  // item once, so callers may re-queue a buffer they modified again.
  Status AddMetadataBuffer(uint64_t offset, std::vector<uint8_t> data,
                           uint16_t blf_type, size_t lsn_offset,
                           size_t crc_offset, const char* what);
  // Queues a metadata buffer for write-back without a physical log item.
  // Newly allocated inode clusters use this: the XFS_LI_ICREATE item in the
  // same record is the logical intent and replay rebuilds the cluster from
  // it, so physically logging 16 KiB per cluster would only consume the
  // record budget (the kernel marks these buffers "ordered" for the same
  // reason). The write still happens after the log record is durable, in
  // CommitTransaction's metadata phase. `ino` is used only to drop the
  // 16 KiB inode read-ahead chunk that now holds different bytes.
  Status AddOrderedBuffer(uint64_t offset, std::vector<uint8_t> data,
                          uint64_t ino, const char* what);

  const std::vector<LogOp>& ops() const { return ops_; }
  const std::vector<TransactionWrite>& writes() const { return writes_; }
  // Contents of a metadata buffer already queued in this transaction, or
  // nullptr when the buffer is not part of it. Lets several callers (the block
  // allocator's sb_fdblocks, the inode allocator's sb_icount/sb_ifree) apply
  // deltas to the same superblock buffer without losing each other's changes.
  const std::vector<uint8_t>* QueuedBuffer(uint64_t offset) const;
  size_t log_bytes() const;
  // Total size of the framed log record (header block + rounded data).
  size_t record_bytes() const;
  // False when the item list would produce a record larger than this writer
  // can emit (32 KiB of data; bigger records need extension headers).
  bool record_fits() const;

 private:
  friend class XfsFilesystem;
  uint32_t tid_ = 0;
  uint64_t lsn_ = 0;
  uint32_t cycle_ = 0;
  uint32_t block_ = 0;
  bool started_ = false;
  BlockDevice* device_ = nullptr;  // for the dirty-chunk diff (null in tests)
  std::vector<LogOp> ops_;  // items only; start/trans-header/commit added later
  std::vector<TransactionWrite> writes_;
  // Logged buffer items by device offset (dedup for AddMetadataBuffer): the
  // value is (first log op index, format size) so re-adding a buffer can
  // replace the previously logged region instead of logging it twice.
  struct LoggedBufferRef {
    size_t op_index = 0;  // index into ops_ of the format header
  };
  std::map<uint64_t, LoggedBufferRef> logged_buffers_;
};

}  // namespace xfs
