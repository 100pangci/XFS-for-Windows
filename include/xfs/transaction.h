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
#include <vector>

#include "xfs/error.h"
#include "xfs/log_record.h"

namespace xfs {

// One metadata buffer that becomes visible when the transaction commits.
struct TransactionWrite {
  uint64_t offset = 0;          // device byte offset of the buffer
  std::vector<uint8_t> data;    // complete new contents (LSN already stamped)
  uint64_t ino = 0;             // subject inode's cache entries (0 = none)
  bool inode_metadata = false;  // invalidate parsed inode + file info
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

  const std::vector<LogOp>& ops() const { return ops_; }
  const std::vector<TransactionWrite>& writes() const { return writes_; }
  size_t log_bytes() const;

 private:
  friend class XfsFilesystem;
  uint32_t tid_ = 0;
  uint64_t lsn_ = 0;
  uint32_t cycle_ = 0;
  uint32_t block_ = 0;
  bool started_ = false;
  std::vector<LogOp> ops_;  // items only; start/trans-header/commit added later
  std::vector<TransactionWrite> writes_;
};

}  // namespace xfs
