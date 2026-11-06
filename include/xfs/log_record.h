// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - log record assembly.
//
// A record consists of one fixed 512-byte xlog_rec_header plus zero or more
// 512-byte xlog_rec_ext_header blocks, followed by `h_len` bytes of log data
// (a multiple of 512), containing a chain of operations:
//
//   [op START_TRANS, len 0]                     opens the transaction
//   [op len 16][xfs_trans_header]               transaction header
//   [op len N][item format header]              one op per logged region
//   [op len M][item payload]
//   [op COMMIT_TRANS, len 0]                    commits it
//
// Records whose data needs more than the 64 cycle words in the fixed header
// use xlog_rec_ext_header blocks. BuildLogRecord can assemble a complete legal
// record image up to XLOG_MAX_RECORD_BSIZE; the transaction layer currently
// writes 32 KiB records with one header block and splits larger transactions.
//
// All header blocks must fit before the physical end of the log, but the data
// area may wrap across that boundary: data blocks that continue at the start
// of the ring carry the next cycle. Every 512-byte data block has its first
// word replaced by the cycle number of the generation that owns that physical
// block (recovery uses those words to find the log head); the original words
// are stored in h_cycle_data and the extension headers. The CRC covers the
// first 328 bytes of the fixed header (with the CRC field zeroed), each
// extension header's first 260 bytes, and the whole data area as stored
// (including the cycle stamps).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xfs/error.h"
#include "xfs/log_format.h"
#include "xfs/uuid.h"

namespace xfs {

// One log operation: a client id, a flag byte and the payload that follows the
// op header. Transaction items use XFS_TRANSACTION (0x69); the clean-unmount
// record uses XFS_LOG (0xaa).
struct LogOp {
  uint8_t client_id = logfmt::kClientTransaction;
  uint8_t flags = 0;
  std::vector<uint8_t> payload;

  LogOp() = default;
  LogOp(uint8_t op_flags, std::vector<uint8_t> data)
      : flags(op_flags), payload(std::move(data)) {}
};

struct LogRecordParams {
  uint32_t cycle = 0;         // record write cycle
  uint32_t block = 0;         // log block of the header (LSN block part)
  uint32_t tid = 0;           // transaction id used in every op header
  uint32_t prev_block = 0;    // block of the previous record (h_prev_block)
  uint64_t tail_lsn = 0;      // h_tail_lsn (oldest record still needed)
  uint32_t log_blocks = 0;    // total log size in 512-byte blocks
  uint32_t record_size = 0;   // ignored by BuildLogRecord; h_size is derived
                              // from the padded data length
  Uuid uuid{};
};

// Builds the complete in-memory image of one log record: the fixed header,
// any extension headers and the data area. `ops` must be the full operation
// chain, including the START and COMMIT ops (see the transaction layer); this
// function checks only that it is non-empty. The result is a multiple of 512
// bytes, laid out in logical record order. The header blocks start at
// `params.block` and must fit before the physical end of the log. The data
// area may continue at block 0 stamped with the next cycle, so a wrapping
// caller writes it in ring order rather than as one physical extent.
//
// Fails when:
//   - the log size is zero, or the record block is outside the log;
//   - the cycle is 0 or 0xFFFFFFFF, or `ops` is empty;
//   - the fixed/extension headers would cross the physical end of the log;
//   - the complete image (headers plus padded data) would exceed
//     XLOG_MAX_RECORD_BSIZE;
//   - the record would not fit in the log (it would lap the ring);
//   - the data needs more cycle-data slots than its headers provide.
Result<std::vector<uint8_t>> BuildLogRecord(const LogRecordParams& params,
                                            const std::vector<LogOp>& ops);

// Appends one op (header + payload, padded to 4 bytes) to a data buffer.
void AppendLogOp(std::vector<uint8_t>* data, uint32_t tid, const LogOp& op);

// Transaction header, host (little-endian) byte order.
void AppendTransHeader(std::vector<uint8_t>* data, uint32_t tid,
                       uint32_t num_items);

// Clean-unmount operation: one op, client XFS_LOG, payload xfs_unmount_log_format.
LogOp MakeUnmountOp();

// Builds the LSN value: (cycle << 32) | block.
inline uint64_t MakeLsn(uint32_t cycle, uint32_t block) {
  return (static_cast<uint64_t>(cycle) << 32) | block;
}

// Helpers for the item format headers (host byte order).
void PutLE16(std::vector<uint8_t>* data, size_t off, uint16_t value);
void PutLE32(std::vector<uint8_t>* data, size_t off, uint32_t value);
void PutLE64(std::vector<uint8_t>* data, size_t off, uint64_t value);

}  // namespace xfs
