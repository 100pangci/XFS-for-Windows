// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - log record assembly.
//
// A record is one 512-byte header block followed by `h_len` data bytes
// (a multiple of 512), containing a chain of operations:
//
//   [op START_TRANS, len 0]                     opens the transaction
//   [op len 16][xfs_trans_header]               transaction header
//   [op len N][item format header]              one op per logged region
//   [op len M][item payload]
//   [op COMMIT_TRANS, len 0]                    commits it
//
// Every 512-byte data block has its first word replaced by the record cycle
// number (recovery uses those words to find the log head); the original words
// are stored in the header. The CRC covers the header (with the CRC field
// zeroed) plus the whole data area.
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
  uint32_t record_size = 0;   // h_size (0 = default 32 KiB)
  Uuid uuid{};
};

// Builds the complete on-disk image of one log record (header + data).
// `ops` must be the full operation chain, including the START and COMMIT ops
// (see the transaction layer). The result is a multiple of 512 bytes and can
// be written at the record's block. Fails when the record does not fit the
// log or would need extension header blocks (records above 32 KiB, which this
// writer never produces).
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
