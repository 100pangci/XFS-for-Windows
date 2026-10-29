// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/log_checker.h"

#include <cstring>
#include <vector>

#include "xfs/endian.h"
#include "xfs/log.h"

namespace xfs {
namespace {

constexpr uint32_t kLogMagic = 0xFEEDBABE;
constexpr size_t kSectorSize = 512;
constexpr size_t kRecHeaderSize = 512;
constexpr uint8_t kUnmountTransFlag = 0x20;

// Record header field offsets (xlog_rec_header).
constexpr size_t kOffMagic = 0;      // be32
constexpr size_t kOffCycle = 4;      // be32
constexpr size_t kOffVersion = 8;    // be32
constexpr size_t kOffLen = 12;       // be32 (data bytes after header blocks)
constexpr size_t kOffLsn = 16;       // be64
constexpr size_t kOffPrevBlock = 36; // be32
constexpr size_t kOffNumLogops = 40;
constexpr size_t kOffCycleData = 44; // be32[64]
constexpr size_t kOffUuid = 304;     // 16 bytes
constexpr size_t kOffSize = 320;     // be32

struct RecordCandidate {
  uint64_t offset = 0;  // byte offset in device
  uint32_t cycle = 0;
  uint32_t block = 0;   // log-relative block number
  uint32_t len = 0;
  uint32_t hdrs = 1;    // header blocks (1 + ext headers)
  uint32_t num_ops = 0;
  uint32_t prev_block = 0;
  uint64_t lsn = 0;
};

struct ScanState {
  bool all_zero = true;
  std::vector<RecordCandidate> records;
};

bool ScanChunk(const uint8_t* buf, size_t len, uint64_t base_offset,
               uint64_t log_start, uint32_t log_blocks, const Uuid& uuid,
               ScanState* state) {
  for (size_t i = 0; i < len; ++i) {
    if (buf[i] != 0) {
      state->all_zero = false;
      break;
    }
  }
  // Record headers must be 512-byte aligned within the log.
  for (size_t off = 0; off + kRecHeaderSize <= len; off += kSectorSize) {
    uint64_t dev_off = base_offset + off;
    if (dev_off < log_start) continue;
    uint64_t rel = dev_off - log_start;
    if (rel % kSectorSize != 0) continue;
    const uint8_t* p = buf + off;
    if (GetBE32(p + kOffMagic) != kLogMagic) continue;
    uint32_t version = GetBE32(p + kOffVersion);
    if (version != 1 && version != 2) continue;
    uint32_t cycle = GetBE32(p + kOffCycle);
    if (cycle == 0) continue;
    uint32_t hlen = GetBE32(p + kOffLen);
    if (hlen < 512 || hlen > 256 * 1024) continue;
    uint64_t lsn = GetBE64(p + kOffLsn);
    uint32_t lsn_block = static_cast<uint32_t>(lsn & 0xffffffffu);
    if (lsn_block >= log_blocks) continue;
    // A record header's LSN points at its own first block.
    if (static_cast<uint64_t>(rel / kSectorSize) != lsn_block) continue;
    const uint8_t* h_uuid = p + kOffUuid;
    bool uuid_ok = std::memcmp(h_uuid, uuid.data(), 16) == 0;
    if (!uuid_ok) continue;
    // Extended headers for sunit > 32k (log v2).
    uint32_t hdrs = 1;
    uint32_t hsize = GetBE32(p + kOffSize);
    if (version == 2 && hsize > 32 * 1024) {
      hdrs = hsize / (32 * 1024);
      if (hsize % (32 * 1024)) hdrs++;
    }
    RecordCandidate c;
    c.offset = dev_off;
    c.cycle = cycle;
    c.block = lsn_block;
    c.len = hlen;
    c.hdrs = hdrs;
    c.num_ops = GetBE32(p + kOffNumLogops);
    c.prev_block = GetBE32(p + kOffPrevBlock);
    c.lsn = lsn;
    state->records.push_back(c);
  }
  return true;
}

}  // namespace

const char* ToString(LogState state) {
  switch (state) {
    case LogState::kEmpty: return "empty";
    case LogState::kClean: return "clean";
    case LogState::kDirty: return "dirty";
    case LogState::kUnknown: return "unknown";
  }
  return "?";
}

Result<LogCheckResult> CheckLogClean(BlockDevice& dev,
                                     const XfsGeometry& geo,
                                     const XfsSuperblock& sb) {
  if (sb.logstart == 0 || sb.logblocks == 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "no internal log to check")
        .Metadata("log");
  }
  uint64_t log_start = geo.FsbToByte(sb.logstart);
  uint64_t log_bytes = static_cast<uint64_t>(sb.logblocks) << geo.block_log;
  if (log_start + log_bytes > dev.Size()) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "log extends past the end of the device")
        .Metadata("log")
        .Offset(log_start);
  }

  ScanState state;
  constexpr size_t kChunk = 4 * 1024 * 1024;
  std::vector<uint8_t> buf(kChunk);
  for (uint64_t off = 0; off < log_bytes; off += kChunk) {
    size_t to_read =
        static_cast<size_t>(std::min<uint64_t>(kChunk, log_bytes - off));
    if (!dev.Read(log_start + off, buf.data(), to_read)) {
      return XFS_ERR(XfsError::kIoError, "failed to read log")
          .Metadata("log")
          .Offset(log_start + off);
    }
    // h_lsn and record positions count 512-byte basic blocks; sb_logblocks
    // counts filesystem blocks. Do not discard headers in the later log area.
    if (!ScanChunk(buf.data(), to_read, log_start + off, log_start,
                   static_cast<uint32_t>(log_bytes / kSectorSize), sb.uuid,
                   &state)) {
      return XFS_ERR(XfsError::kIoError, "log scan failed")
          .Metadata("log");
    }
  }

  LogCheckResult result;
  if (state.records.empty()) {
    if (state.all_zero) {
      result.state = LogState::kEmpty;
      result.detail = "log is zeroed (freshly formatted)";
    } else {
      result.state = LogState::kUnknown;
      result.detail = "log contains data but no valid record headers";
    }
    return result;
  }

  // Newest record: max (cycle, block); cycle wrap-around at 2^32 is not a
  // practical concern (kernel compares the same way).
  const RecordCandidate* newest = &state.records[0];
  for (const auto& c : state.records) {
    if (c.cycle > newest->cycle ||
        (c.cycle == newest->cycle && c.block > newest->block)) {
      newest = &c;
    }
  }
  result.newest_record_lsn =
      (static_cast<uint64_t>(newest->cycle) << 32) | newest->block;

  // Read the first op header of the newest record. The first word of each
  // record data sector is replaced by the cycle value on disk; the original
  // word is stored in h_cycle_data[0] of the record header.
  std::vector<uint8_t> hdr(kRecHeaderSize);
  std::vector<uint8_t> data(kSectorSize + 16);
  if (!dev.Read(newest->offset, hdr.data(), hdr.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read log record header")
        .Metadata("log")
        .Offset(newest->offset);
  }
  uint64_t data_off = newest->offset + uint64_t(newest->hdrs) * kSectorSize;
  if (!dev.Read(data_off, data.data(), data.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read log record data")
        .Metadata("log")
        .Offset(data_off);
  }
  // Geometry of the newest record, needed by the read-write writer.
  result.cycle = newest->cycle;
  result.block = newest->block;
  result.blocks = newest->hdrs + newest->len / kSectorSize;
  result.num_ops = newest->num_ops;
  result.prev_block = newest->prev_block;
  result.lsn = newest->lsn;
  // Restore the first word of the first data sector.
  uint32_t original_first_word = GetBE32(hdr.data() + kOffCycleData);
  StoreBE32(data.data(), original_first_word);

  uint8_t flags = GetU8(data.data() + 9);
  uint32_t oh_len = GetBE32(data.data() + 4);
  result.state = (flags & kUnmountTransFlag) ? LogState::kClean : LogState::kDirty;
  result.detail = result.state == LogState::kClean
                      ? "newest log record is an unmount record"
                      : "newest log record is a transaction record";
  if (result.state == LogState::kDirty) {
    result.detail += " (op flags=0x" + std::to_string(flags) +
                     ", len=" + std::to_string(oh_len) + ")";
  }
  XFS_DEBUG("log check: records=" << state.records.size()
                                  << " newest lsn=0x" << std::hex
                                  << result.newest_record_lsn << std::dec
                                  << " state=" << ToString(result.state));
  return result;
}

Result<LogPosition> ReadLogPosition(BlockDevice& dev, const XfsGeometry& geo,
                                    const XfsSuperblock& sb) {
  const uint64_t log_blocks =
      (static_cast<uint64_t>(sb.logblocks) << geo.block_log) / 512;
  if (log_blocks == 0 || log_blocks > 0x7fffffff) {
    return XFS_ERR(XfsError::kUnsupportedGeometry, "invalid log size")
        .Metadata("log")
        .Actual(log_blocks);
  }
  auto check = CheckLogClean(dev, geo, sb);
  if (!check.ok()) return check.error();
  const LogCheckResult& r = check.value();

  LogPosition position;
  position.log_blocks = static_cast<uint32_t>(log_blocks);
  position.record_size = 32 * 1024;  // XLOG_BIG_RECORD_BSIZE, one header block
  if (r.state == LogState::kEmpty) {
    // All-zero log: the kernel starts a fresh log at block 0, cycle 1, and the
    // previous block link is "none" (the mkfs record does the same).
    position.cycle = 1;
    position.next_block = 0;
    position.prev_block = 0xffffffffu;
    position.tail_lsn = (uint64_t{1} << 32);
    position.clean = false;
    return position;
  }
  if (r.state != LogState::kClean) {
    return XFS_ERR(XfsError::kDirtyLog,
                   "log is not clean; the journal must be replayed before it "
                   "can be written")
        .Metadata("log")
        .Actual(r.newest_record_lsn);
  }
  if (r.blocks == 0) {
    return XFS_ERR(XfsError::kCorruptSuperblock, "log record has zero size")
        .Metadata("log")
        .Actual(r.block);
  }

  // The next record starts right after the unmount record. If that crosses the
  // physical end of the log, the write position wraps to block 0 and the cycle
  // advances (this is what xlog_set_state does through its bump_cycle).
  uint64_t end = static_cast<uint64_t>(r.block) + r.blocks;
  uint32_t cycle = r.cycle;
  if (end >= log_blocks) {
    end -= log_blocks;
    ++cycle;
  }
  position.cycle = cycle;
  position.next_block = static_cast<uint32_t>(end);
  position.prev_block = r.block;
  position.tail_lsn = (static_cast<uint64_t>(cycle) << 32) | position.next_block;
  position.clean = true;
  XFS_DEBUG("log position: cycle=" << cycle << " next=" << position.next_block
                                   << " prev=" << r.block << " clean=yes");
  return position;
}

}  // namespace xfs
