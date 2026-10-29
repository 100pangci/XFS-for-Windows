// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - internal log scanning, head/tail discovery and
// transaction reconstruction (RW-5).
//
// Faithful port of the read side of the kernel's log recovery
// (fs/xfs/xfs_log_recover.c, see docs/RECOVERY.md):
//
//   xlog_find_zeroed / find_cycle_start / find_verify_cycle
//   xlog_find_verify_log_record / find_head / find_tail
//   xlog_rseek_logrec_hdr / seek_logrec_hdr
//   xlog_verify_head / verify_tail
//   xlog_do_recovery_pass / recover_process / recover_process_data
//   xlog_recover_process_ophdr / add_to_trans / add_to_cont_trans
//
// Everything here is read-only. Replay lives in replay.cpp.
#include "xfs/recovery.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/log_format.h"
#include "xfs/bmap_intent.h"
#include "xfs/refcount_intent.h"
#include "xfs/rmap_intent.h"
#include "xfs/xattr_intent.h"

namespace xfs {
namespace {

namespace lf = logfmt;

// Kernel constants (fs/xfs/libxfs/xfs_log_format.h):
//   XLOG_MAX_ICLOGS         8
//   XLOG_MAX_RECORD_BSIZE   256 KiB
//   XLOG_HEADER_CYCLE_SIZE  32 KiB
//   XLOG_REC_SHIFT          BTOBB(1 << XLOG_MAX_RECORD_BSHIFT) = 512 BB
//   XLOG_TOTAL_REC_SHIFT    BTOBB(8 << 18) = 4096 BB
constexpr uint32_t kMaxIclogs = 8;
constexpr uint32_t kMaxRecordBytes = 256 * 1024;
constexpr uint32_t kHeaderCycleBytes = 32 * 1024;
constexpr uint32_t kRecScanBlocks = kMaxRecordBytes / 512;
constexpr uint32_t kTotalRecScanBlocks = kMaxIclogs * kMaxRecordBytes / 512;
constexpr uint32_t kLogVersion2Bit = 0x0400;  // XFS_SB_VERSION_LOGV2BIT
constexpr uint32_t kVersion2 = 2;             // XLOG_VERSION_2
constexpr uint32_t kMaxRegionsInItem =
    64 * 1024 / lf::kBufChunkSize / 2 + 1;  // XLOG_MAX_REGIONS_IN_ITEM (257)
constexpr uint32_t kInvalidBlock = 0xffffffffu;

// The kernel's xfs_log_check... advisory: this build only supports v5, where
// the log is always version 2.
bool HasLogV2(const XfsSuperblock& sb) { return (sb.versionnum & kLogVersion2Bit) != 0; }

std::string Hex(uint64_t value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%llx",
                static_cast<unsigned long long>(value));
  return buf;
}

std::string LsnText(uint64_t lsn) {
  return Hex(lsn) + " (cycle " + std::to_string(lsn >> 32) + " block " +
         std::to_string(static_cast<uint32_t>(lsn & 0xffffffffu)) + ")";
}

// The log item format headers (EFI/EFD/BUI/BUD, ...) are host byte order.
uint64_t LoadLE64(const uint8_t* p) {
  return static_cast<uint64_t>(LoadLE32(p)) |
         (static_cast<uint64_t>(LoadLE32(p + 4)) << 32);
}

// ---------------------------------------------------------------------------
// Block-addressed access to the logical ring
// ---------------------------------------------------------------------------
class LogImage {
 public:
  LogImage(BlockDevice& dev, const RecoveryGeometry& geometry)
      : dev_(dev), geometry_(geometry) {}

  const RecoveryGeometry& geometry() const { return geometry_; }
  uint32_t blocks() const { return geometry_.log_blocks; }

  bool ReadBlock(uint32_t block, uint8_t* out) {
    return dev_.Read(geometry_.BlockOffset(geometry_.Wrap(block)), out, 512);
  }

  // xlog_get_cycle(): a record header block leads with the magic and stores
  // the cycle in the next word; every other block leads with its cycle.
  static uint32_t CycleOfBlock(const uint8_t* block) {
    const uint32_t first = GetBE32(block);
    return first == lf::kRecordMagic ? GetBE32(block + 4) : first;
  }
  Result<uint32_t> ReadCycle(uint32_t block) {
    uint8_t buf[512];
    if (!ReadBlock(block, buf)) {
      return XFS_ERR(XfsError::kIoError, "failed to read a log block")
          .Offset(geometry_.BlockOffset(geometry_.Wrap(block)))
          .Metadata("log");
    }
    return CycleOfBlock(buf);
  }

  // Reads `count` blocks starting at `block`, wrapping at the end of the ring
  // (the caller uses this for records that straddle the physical end).
  bool ReadRange(uint32_t block, uint32_t count, uint8_t* out) {
    uint32_t done = 0;
    while (done < count) {
      const uint32_t logical = block + done;
      const uint32_t wrapped = geometry_.Wrap(logical);
      const uint32_t run = std::min(count - done, geometry_.log_blocks - wrapped);
      if (!dev_.Read(geometry_.BlockOffset(wrapped), out + done * 512,
                     static_cast<size_t>(run) * 512)) {
        return false;
      }
      done += run;
    }
    return true;
  }

  // Reads a range without wrapping (fails if it would cross the end); used
  // where the kernel's xlog_bread reads physical log blocks.
  bool ReadPhysical(uint32_t block, uint32_t count, uint8_t* out) {
    if (block + count > geometry_.log_blocks) return false;
    return dev_.Read(geometry_.BlockOffset(block), out,
                     static_cast<size_t>(count) * 512);
  }

  bool WritePhysical(uint32_t block, uint32_t count, const uint8_t* in) {
    if (block + count > geometry_.log_blocks) return false;
    return dev_.Write(geometry_.BlockOffset(block), in,
                      static_cast<size_t>(count) * 512);
  }

  bool Flush() { return dev_.Flush(); }

 private:
  BlockDevice& dev_;
  RecoveryGeometry geometry_;
};

// Errors that mean "this log cannot be parsed as a log": the mount policy
// reports them as an unknown (refused) log state rather than a hard error, so
// `xfswin check` and forced read-only mounts behave like they always have.
bool IsLogConsistencyError(XfsError code) {
  switch (code) {
    case XfsError::kCorruptBtree:
    case XfsError::kBadCrc:
    case XfsError::kBadMagic:
    case XfsError::kBadUuid:
    case XfsError::kBadVersion:
    case XfsError::kBadLength:
    case XfsError::kBadValue:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Record header parsing
// ---------------------------------------------------------------------------
struct RecordHeader {
  bool magic = false;
  uint32_t version = 0;
  uint32_t cycle = 0;
  uint32_t len = 0;      // h_len (data bytes)
  uint32_t hsize = 0;    // h_size (iclog size)
  uint32_t num_ops = 0;
  uint64_t lsn = 0;
  uint64_t tail_lsn = 0;
  uint32_t prev_block = 0;
  uint32_t header_blocks = 1;  // 1 + extension headers

  uint32_t data_blocks() const { return (len + 511) / 512; }
};

// xlog_logrec_hblks(): records whose iclog size exceeds one 32 KiB header
// carry additional header blocks holding cycle data.
uint32_t HeaderBlocks(const RecordHeader& header, bool logv2) {
  if (logv2) {
    if ((header.version & kVersion2) && header.hsize > kHeaderCycleBytes) {
      return (header.hsize + kHeaderCycleBytes - 1) / kHeaderCycleBytes;
    }
  }
  return 1;
}

// Parses the fixed part of a record header from the first 512-byte block.
// Does not require the magic to be present (callers check).
RecordHeader ParseHeader(const uint8_t* block) {
  RecordHeader h;
  h.magic = GetBE32(block, lf::kRecMagic) == lf::kRecordMagic;
  h.cycle = GetBE32(block, lf::kRecCycle);
  h.version = GetBE32(block, lf::kRecVersion);
  h.len = GetBE32(block, lf::kRecLen);
  h.lsn = GetBE64(block, lf::kRecLsn);
  h.tail_lsn = GetBE64(block, lf::kRecTailLsn);
  h.prev_block = GetBE32(block, lf::kRecPrevBlock);
  h.num_ops = GetBE32(block, lf::kRecNumLogOps);
  h.hsize = GetBE32(block, lf::kRecSize);
  return h;
}

// xlog_valid_rec_header() with the iclog size determined by the pass.
Status ValidateRecordHeader(const RecordHeader& header, uint32_t block,
                            bool logv2, uint32_t logger_size,
                            const char* what) {
  if (!header.magic) {
    return XFS_ERR(XfsError::kBadMagic, std::string(what) + ": not a record")
        .Actual(block)
        .Metadata("log");
  }
  if (logv2) {
    if (header.version != kVersion2) {
      return XFS_ERR(XfsError::kBadVersion,
                     std::string(what) + ": bad log record version")
          .Expected(kVersion2)
          .Actual(header.version)
          .Metadata("log");
    }
  } else if (header.version != 1) {
    return XFS_ERR(XfsError::kBadVersion,
                   std::string(what) + ": bad log record version")
        .Expected(1)
        .Actual(header.version)
        .Metadata("log");
  }
  if (header.len == 0 || header.len > logger_size) {
    return XFS_ERR(XfsError::kBadLength,
                   std::string(what) + ": bad log record length")
        .Expected(logger_size)
        .Actual(header.len)
        .Metadata("log");
  }
  return OkStatus();
}

// Full validation including the log size bound.
Status ValidateRecordAt(const RecordHeader& header, uint32_t block,
                        bool logv2, uint32_t log_blocks, uint32_t logger_size,
                        const char* what) {
  Status s = ValidateRecordHeader(header, block, logv2, logger_size, what);
  if (!s.ok()) return s;
  if (block > log_blocks) {
    return XFS_ERR(XfsError::kOutOfRange, std::string(what) + ": bad block")
        .Expected(log_blocks)
        .Actual(block)
        .Metadata("log");
  }
  return OkStatus();
}

// xlog_header_check_recover(): the log must be replayable by this build.
Status HeaderCheckRecover(const uint8_t* bytes, const XfsSuperblock& sb) {
  const uint32_t fmt = GetBE32(bytes, lf::kRecFmt);
  if (fmt != lf::kFmtLinuxLe) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "dirty log written in an incompatible format")
        .Expected(lf::kFmtLinuxLe)
        .Actual(fmt)
        .Metadata("log");
  }
  if (std::memcmp(bytes + lf::kRecUuid, sb.uuid.data(), 16) != 0) {
    return XFS_ERR(XfsError::kBadUuid,
                   "dirty log record has a mismatched uuid")
        .Metadata("log");
  }
  return OkStatus();
}

// xlog_header_check_mount(): tolerate the null uuid of an IRIX-era log.
bool HeaderCheckMount(const uint8_t* bytes, const XfsSuperblock& sb,
                      std::vector<std::string>* warnings) {
  static const uint8_t kNullUuid[16] = {};
  if (std::memcmp(bytes + lf::kRecUuid, kNullUuid, 16) == 0) {
    if (warnings) warnings->push_back("null uuid in log - IRIX style log");
    return true;
  }
  return std::memcmp(bytes + lf::kRecUuid, sb.uuid.data(), 16) == 0;
}

// xlog_cksum(): the CRC covers the fixed header (up to XLOG_REC_SIZE, with the
// CRC field zeroed) plus the record data exactly as written, i.e. with the
// cycle words stamped in. A zero stored CRC is accepted (mkfs history).
uint32_t ComputeRecordCrc(const uint8_t* header_bytes, size_t header_size,
                          size_t header_covered, const uint8_t* data,
                          size_t data_len) {
  std::vector<uint8_t> fixed(header_bytes, header_bytes + header_covered);
  PutLE32(fixed.data(), lf::kRecCrc, 0);
  uint32_t crc = Crc32cUpdate(0xFFFFFFFFu, fixed.data(), fixed.size());
  // xlog_cksum() streams each extension header's 260 covered bytes; the 252
  // reserved bytes are not part of the checksum.
  for (size_t off = lf::kRecordHeaderSize;
       off + lf::kRecExtSize <= header_size; off += lf::kRecordHeaderSize) {
    crc = Crc32cUpdate(crc, header_bytes + off, lf::kRecExtSize);
  }
  if (data_len) crc = Crc32cUpdate(crc, data, data_len);
  return ~crc;
}

// Verifies a record's CRC. Returns false when the stored CRC is set and does
// not match either historical fixed-header coverage size.
bool VerifyRecordCrc(const std::vector<uint8_t>& header_bytes,
                     const uint8_t* data, size_t data_len, uint32_t* computed) {
  const uint32_t stored = GetLE32(header_bytes.data(), lf::kRecCrc);
  // XLOG_REC_SIZE (328, the non-i386 layout) and XLOG_REC_SIZE_OTHER (324).
  uint32_t crc = ComputeRecordCrc(header_bytes.data(), header_bytes.size(),
                                  lf::kRecCrcCovered, data, data_len);
  if (stored == 0 || crc == stored) {
    if (computed) *computed = crc;
    return true;
  }
  uint32_t other = ComputeRecordCrc(header_bytes.data(), header_bytes.size(),
                                    324, data, data_len);
  if (computed) *computed = other;
  return other == stored;
}

// xlog_unpack_data(): restores the first word of every data block from the
// record header's cycle-data arrays.
void UnpackCycleData(const std::vector<uint8_t>& header_bytes,
                     std::vector<uint8_t>* data) {
  const uint32_t blocks = static_cast<uint32_t>(data->size() / 512);
  for (uint32_t i = 0; i < blocks; ++i) {
    uint32_t saved = 0;
    if (i < lf::kRecCycleDataCount) {
      saved = GetBE32(header_bytes.data(), lf::kRecCycleData + i * 4);
    } else {
      // xlog_cycle_data(): cycles beyond the first 64 live in the extension
      // header blocks (block j of the header, 4 bytes in).
      const uint32_t ext = i / lf::kRecCycleDataCount;      // 1-based
      const uint32_t k = i % lf::kRecCycleDataCount;
      const size_t at = static_cast<size_t>(ext) * 512 + 4 + k * 4;
      if (at + 4 <= header_bytes.size()) saved = GetBE32(header_bytes.data(), at);
    }
    StoreBE32(data->data() + static_cast<size_t>(i) * 512, saved);
  }
}

// ---------------------------------------------------------------------------
// Head/tail discovery (read-only)
// ---------------------------------------------------------------------------
// xlog_find_cycle_start(): binary search for the first block with `cycle`.
Result<uint32_t> FindCycleStart(LogImage& img, uint32_t first_blk,
                                uint32_t last_blk, uint32_t cycle) {
  uint32_t end_blk = last_blk;
  uint32_t mid_blk = (first_blk + end_blk) / 2;
  while (mid_blk != first_blk && mid_blk != end_blk) {
    auto mid_cycle = img.ReadCycle(mid_blk);
    if (!mid_cycle.ok()) return mid_cycle.error();
    if (mid_cycle.value() == cycle) {
      end_blk = mid_blk;
    } else {
      first_blk = mid_blk;
    }
    mid_blk = (first_blk + end_blk) / 2;
  }
  return end_blk;
}

// xlog_find_verify_cycle(): first block in [start, start+count) whose cycle is
// `stop_on_cycle_no`, or kInvalidBlock.
Result<uint32_t> FindVerifyCycle(LogImage& img, uint32_t start, uint32_t count,
                                 uint32_t stop_on_cycle_no) {
  const uint32_t kInvalid = kInvalidBlock;
  std::vector<uint8_t> buffer(512);
  for (uint32_t i = 0; i < count; ++i) {
    if (!img.ReadBlock(start + i, buffer.data())) {
      return XFS_ERR(XfsError::kIoError, "failed to read the log")
          .Offset(img.geometry().BlockOffset(img.geometry().Wrap(start + i)))
          .Metadata("log");
    }
    if (LogImage::CycleOfBlock(buffer.data()) == stop_on_cycle_no) {
      return start + i;
    }
  }
  return kInvalid;
}

// xlog_find_verify_log_record(): potentially back up over a partial record
// write. Returns 0 on success, 1 when the search hit the beginning of the log.
Result<int> FindVerifyLogRecord(LogImage& img, uint32_t start_blk,
                                uint32_t* last_blk, int extra_bblks,
                                bool logv2, const XfsSuperblock& sb,
                                std::vector<std::string>* warnings) {
  if (*last_blk <= start_blk) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "log inconsistent (didn't find previous header)")
        .Metadata("log");
  }
  if (*last_blk > img.blocks()) {
    return XFS_ERR(XfsError::kOutOfRange, "log scan out of range")
        .Actual(*last_blk)
        .Metadata("log");
  }
  const uint32_t num_blks = *last_blk - start_blk;
  std::vector<uint8_t> buffer(static_cast<size_t>(num_blks) * 512);
  if (!img.ReadPhysical(start_blk, num_blks, buffer.data())) {
    return XFS_ERR(XfsError::kIoError, "failed to read the log for scanning")
        .Offset(img.geometry().BlockOffset(start_blk))
        .Metadata("log");
  }
  int64_t i = static_cast<int64_t>(*last_blk) - 1;
  const uint8_t* found = nullptr;
  for (; i >= 0; --i) {
    if (i < static_cast<int64_t>(start_blk)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "log inconsistent (didn't find previous header)")
          .Metadata("log");
    }
    const uint8_t* block = buffer.data() + static_cast<size_t>(i - start_blk) * 512;
    if (GetBE32(block, lf::kRecMagic) == lf::kRecordMagic) {
      found = block;
      break;
    }
  }
  if (i == -1) return 1;  // hit the beginning of the log

  if (!HeaderCheckMount(found, sb, warnings)) {
    return XFS_ERR(XfsError::kBadUuid, "mismatched uuid in a log record header")
        .Metadata("log");
  }
  const RecordHeader header = ParseHeader(found);
  const uint32_t xhdrs = HeaderBlocks(header, logv2);
  if (static_cast<int64_t>(*last_blk) - i + extra_bblks !=
      static_cast<int64_t>(header.data_blocks()) + xhdrs) {
    *last_blk = static_cast<uint32_t>(i);
  }
  return 0;
}

// xlog_find_zeroed(): 0 = the log is completely written to, 1 = partially or
// completely zeroed and *first_blk is the first zeroed block.
Result<int> FindZeroed(LogImage& img, uint32_t* first_blk, bool logv2,
                       const XfsSuperblock& sb,
                       std::vector<std::string>* warnings) {
  *first_blk = 0;
  auto first_cycle = img.ReadCycle(0);
  if (!first_cycle.ok()) return first_cycle.error();
  if (first_cycle.value() == 0) {
    *first_blk = 0;
    return 1;  // completely zeroed log
  }
  const uint32_t last = img.blocks() - 1;
  auto last_cycle = img.ReadCycle(last);
  if (!last_cycle.ok()) return last_cycle.error();
  if (last_cycle.value() != 0) return 0;  // log completely written to

  uint32_t last_blk = last;
  auto cycle_start = FindCycleStart(img, 0, last_blk, 0);
  if (!cycle_start.ok()) return cycle_start.error();
  last_blk = cycle_start.value();

  uint32_t num_scan_bblks = std::min(img.blocks(), kTotalRecScanBlocks);
  if (last_blk < num_scan_bblks) num_scan_bblks = last_blk;
  const uint32_t start_blk = last_blk - num_scan_bblks;
  auto new_blk = FindVerifyCycle(img, start_blk, num_scan_bblks, 0);
  if (!new_blk.ok()) return new_blk.error();
  if (new_blk.value() != kInvalidBlock) last_blk = new_blk.value();

  auto verified = FindVerifyLogRecord(img, start_blk, &last_blk, 0, logv2, sb,
                                      warnings);
  if (!verified.ok()) return verified.error();
  if (verified.value() == 1) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "log scan hit the beginning of the log without a record")
        .Metadata("log");
  }
  *first_blk = last_blk;
  return 1;
}

// xlog_find_head().
Result<uint32_t> FindHead(LogImage& img, bool logv2, const XfsSuperblock& sb,
                          std::vector<std::string>* warnings) {
  const uint32_t log_bbnum = img.blocks();
  uint32_t first_blk = 0;
  auto zeroed = FindZeroed(img, &first_blk, logv2, sb, warnings);
  if (!zeroed.ok()) return zeroed.error();
  if (zeroed.value() == 1) {
    if (first_blk == 0) warnings->push_back("totally zeroed log");
    return first_blk;
  }

  auto first_half = img.ReadCycle(0);
  if (!first_half.ok()) return first_half.error();
  uint32_t last_blk = log_bbnum - 1;
  auto last_half = img.ReadCycle(last_blk);
  if (!last_half.ok()) return last_half.error();

  uint32_t head_blk = 0;
  uint32_t stop_on_cycle = 0;
  if (first_half.value() == last_half.value()) {
    head_blk = log_bbnum;
    stop_on_cycle = last_half.value() - 1;
  } else {
    stop_on_cycle = last_half.value();
    auto start = FindCycleStart(img, 0, last_blk, last_half.value());
    if (!start.ok()) return start.error();
    head_blk = start.value();
  }

  uint32_t num_scan_bblks = std::min(log_bbnum, kTotalRecScanBlocks);
  if (head_blk >= num_scan_bblks) {
    const uint32_t start_blk = head_blk - num_scan_bblks;
    auto new_blk = FindVerifyCycle(img, start_blk, num_scan_bblks, stop_on_cycle);
    if (!new_blk.ok()) return new_blk.error();
    if (new_blk.value() != kInvalidBlock) head_blk = new_blk.value();
  } else {
    // Two-part scan around the physical end of the log.
    const uint32_t start_blk = log_bbnum - (num_scan_bblks - head_blk);
    auto new_blk = FindVerifyCycle(img, start_blk, num_scan_bblks - head_blk,
                                   stop_on_cycle - 1);
    if (!new_blk.ok()) return new_blk.error();
    bool validated = false;
    if (new_blk.value() != kInvalidBlock) {
      head_blk = new_blk.value();
    } else {
      new_blk = FindVerifyCycle(img, 0, head_blk, stop_on_cycle);
      if (!new_blk.ok()) return new_blk.error();
      if (new_blk.value() != kInvalidBlock) head_blk = new_blk.value();
      validated = true;
    }
    (void)validated;
  }

  // Make sure head_blk is not in the middle of a log record.
  num_scan_bblks = kRecScanBlocks;
  if (head_blk >= num_scan_bblks) {
    const uint32_t start_blk = head_blk - num_scan_bblks;
    auto verified = FindVerifyLogRecord(img, start_blk, &head_blk, 0, logv2, sb,
                                        warnings);
    if (!verified.ok()) return verified.error();
    if (verified.value() == 1) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "log inconsistent while finding the head")
          .Metadata("log");
    }
  } else {
    const uint32_t start_blk = 0;
    auto verified = FindVerifyLogRecord(img, start_blk, &head_blk, 0, logv2, sb,
                                        warnings);
    if (!verified.ok()) return verified.error();
    if (verified.value() == 1) {
      const uint32_t wrapped_start = log_bbnum - (num_scan_bblks - head_blk);
      uint32_t new_blk = log_bbnum;
      auto check = FindVerifyLogRecord(img, wrapped_start, &new_blk,
                                       static_cast<int>(head_blk), logv2, sb,
                                       warnings);
      if (!check.ok()) return check.error();
      if (check.value() == 1) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "log inconsistent while finding the head")
            .Metadata("log");
      }
      if (new_blk != log_bbnum) head_blk = new_blk;
    }
  }
  return head_blk == log_bbnum ? 0 : head_blk;
}

// xlog_rseek_logrec_hdr(): walk backwards for `count` record headers.
struct SeekResult {
  int found = 0;
  uint32_t block = 0;
  RecordHeader header;
  bool wrapped = false;
};
Result<SeekResult> RseekLogrecHdr(LogImage& img, uint32_t head_blk,
                                  uint32_t tail_blk, int count) {
  SeekResult out;
  const uint32_t log_bbnum = img.blocks();
  const uint32_t end_blk = head_blk > tail_blk ? tail_blk : 0;
  uint8_t block[512];
  for (int64_t i = static_cast<int64_t>(head_blk) - 1;
       i >= static_cast<int64_t>(end_blk); --i) {
    if (!img.ReadBlock(static_cast<uint32_t>(i), block)) {
      return XFS_ERR(XfsError::kIoError, "failed to read the log")
          .Metadata("log");
    }
    if (GetBE32(block, lf::kRecMagic) == lf::kRecordMagic) {
      out.block = static_cast<uint32_t>(i);
      out.header = ParseHeader(block);
      if (++out.found == count) return out;
    }
  }
  if (tail_blk >= head_blk && out.found != count) {
    for (int64_t i = static_cast<int64_t>(log_bbnum) - 1;
         i >= static_cast<int64_t>(tail_blk); --i) {
      if (!img.ReadBlock(static_cast<uint32_t>(i), block)) {
        return XFS_ERR(XfsError::kIoError, "failed to read the log")
            .Metadata("log");
      }
      if (GetBE32(block, lf::kRecMagic) == lf::kRecordMagic) {
        out.wrapped = true;
        out.block = static_cast<uint32_t>(i);
        out.header = ParseHeader(block);
        if (++out.found == count) return out;
      }
    }
  }
  return out;
}

// xlog_seek_logrec_hdr(): walk forward for `count` record headers.
Result<SeekResult> SeekLogrecHdr(LogImage& img, uint32_t head_blk,
                                 uint32_t tail_blk, int count) {
  SeekResult out;
  const uint32_t log_bbnum = img.blocks();
  const uint32_t end_blk = head_blk > tail_blk ? head_blk : log_bbnum - 1;
  uint8_t block[512];
  for (uint32_t i = tail_blk; i <= end_blk; ++i) {
    if (!img.ReadBlock(i, block)) {
      return XFS_ERR(XfsError::kIoError, "failed to read the log")
          .Metadata("log");
    }
    if (GetBE32(block, lf::kRecMagic) == lf::kRecordMagic) {
      out.block = i;
      out.header = ParseHeader(block);
      if (++out.found == count) return out;
    }
  }
  if (tail_blk > head_blk && out.found != count) {
    for (uint32_t i = 0; i < head_blk; ++i) {
      if (!img.ReadBlock(i, block)) {
        return XFS_ERR(XfsError::kIoError, "failed to read the log")
            .Metadata("log");
      }
      if (GetBE32(block, lf::kRecMagic) == lf::kRecordMagic) {
        out.wrapped = true;
        out.block = i;
        out.header = ParseHeader(block);
        if (++out.found == count) return out;
      }
    }
  }
  return out;
}

// xlog_tail_distance().
uint32_t TailDistance(const LogImage& img, uint32_t head_blk,
                      uint32_t tail_blk) {
  if (head_blk < tail_blk) return tail_blk - head_blk;
  return tail_blk + (img.blocks() - head_blk);
}

// Forward declaration: the record iterator below is used by the verification
// passes.
enum class RecordPass { kCrc, kReconstruct };
using RecordHandler = std::function<Status(const LogRecordInfo&,
                                           const std::vector<uint8_t>&)>;

Result<int> IterateRecords(LogImage& img, uint32_t head_blk, uint32_t tail_blk,
                           RecordPass pass, bool logv2, uint32_t logbsize,
                           const XfsSuperblock& sb, const RecordHandler& handler,
                           uint32_t* first_bad, bool* first_bad_set,
                           std::vector<std::string>* warnings);

// xlog_verify_tail().
Result<Status> VerifyTail(LogImage& img, uint32_t head_blk, uint32_t* tail_blk,
                          uint32_t hsize, bool logv2, uint32_t logbsize,
                          const XfsSuperblock& sb,
                          std::vector<std::string>* warnings) {
  const uint32_t orig_tail = *tail_blk;
  if (head_blk == *tail_blk) return OkStatus();  // empty window
  auto seek = SeekLogrecHdr(img, head_blk, *tail_blk, 1);
  if (!seek.ok()) return seek.error();
  if (seek.value().found > 0 && seek.value().block != *tail_blk) {
    *tail_blk = seek.value().block;
  }

  // CRC check from tail to head; a failure within max-iclogs distance of the
  // head is treated as a torn/overwritten tail and the tail moves forward.
  uint32_t first_bad = 0;
  bool first_bad_set = false;
  auto iter = IterateRecords(img, head_blk, *tail_blk, RecordPass::kCrc, logv2,
                             logbsize, sb, nullptr, &first_bad, &first_bad_set,
                             warnings);
  Status error = iter.ok() ? OkStatus() : Status(iter.error());
  while (!error.ok() && first_bad_set) {
    const uint64_t max_distance =
        (static_cast<uint64_t>(kMaxIclogs) * hsize) / 512;
    const uint32_t distance = TailDistance(img, head_blk, first_bad);
    if (distance > max_distance) break;

    auto next = SeekLogrecHdr(img, head_blk, first_bad, 2);
    if (!next.ok()) return next.error();
    if (next.value().found < 2) break;
    *tail_blk = next.value().block;
    first_bad = 0;
    first_bad_set = false;
    auto retry = IterateRecords(img, head_blk, *tail_blk, RecordPass::kCrc,
                                logv2, logbsize, sb, nullptr, &first_bad,
                                &first_bad_set, warnings);
    error = retry.ok() ? OkStatus() : Status(retry.error());
  }
  if (error.ok() && *tail_blk != orig_tail && warnings) {
    warnings->push_back("tail block " + Hex(orig_tail) +
                        " overwrite detected; updated to " + Hex(*tail_blk));
  }
  return error;
}

// xlog_verify_head(): detect and trim torn writes at the head.
struct NewestRecord {
  uint32_t block = 0;
  RecordHeader header;
  bool wrapped = false;
};

Result<Status> VerifyHead(LogImage& img, uint32_t* head_blk, uint32_t* tail_blk,
                          bool logv2, uint32_t logbsize,
                          const XfsSuperblock& sb, NewestRecord* newest,
                          std::vector<std::string>* warnings) {
  // Search backwards up to XLOG_MAX_ICLOGS records.
  auto tmp = RseekLogrecHdr(img, *head_blk, *tail_blk, kMaxIclogs);
  if (!tmp.ok()) return tmp.error();
  if (tmp.value().found == 0) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "no log record found before the head")
        .Metadata("log");
  }
  const uint32_t tmp_rhead_blk = tmp.value().block;
  if (tmp_rhead_blk == *head_blk || *tail_blk == *head_blk) {
    return OkStatus();
  }

  uint32_t first_bad = 0;
  bool first_bad_set = false;
  auto iter = IterateRecords(img, *head_blk, tmp_rhead_blk, RecordPass::kCrc,
                             logv2, logbsize, sb, nullptr, &first_bad,
                             &first_bad_set, warnings);
  if (!iter.ok() && first_bad_set) {
    // A torn write: walk the head back to the last good record.
    if (warnings) {
      warnings->push_back("torn write (CRC failure) detected at log block " +
                          Hex(first_bad) + "; truncating head from " +
                          Hex(*head_blk));
    }
    auto last_good = RseekLogrecHdr(img, first_bad, *tail_blk, 1);
    if (!last_good.ok()) return last_good.error();
    if (last_good.value().found == 0) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "torn write at the head with no good record before it")
          .Metadata("log");
    }
    newest->block = last_good.value().block;
    newest->header = last_good.value().header;
    newest->wrapped = last_good.value().wrapped;
    *head_blk = first_bad;
    *tail_blk = static_cast<uint32_t>(newest->header.tail_lsn & 0xffffffffu);
    if (*head_blk == *tail_blk) {
      return OkStatus();  // kernel: bails out to repair
    }
  } else if (!iter.ok()) {
    return Status(iter.error());
  }
  return VerifyTail(img, *head_blk, tail_blk, newest->header.hsize, logv2,
                    logbsize, sb, warnings);
}

static bool UnmountRecordAt(LogImage& img, uint32_t rhead_blk,
                            const RecordHeader& header, uint32_t head_blk,
                            uint32_t* new_tail, std::vector<std::string>* warnings) {
  const uint32_t hblks = header.header_blocks;
  const uint32_t after_umount_blk =
      img.geometry().Wrap(rhead_blk + hblks + header.data_blocks());
  if (head_blk != after_umount_blk || header.num_ops != 1) return false;
  const uint32_t umount_data_blk = img.geometry().Wrap(rhead_blk + hblks);
  uint8_t op[512];
  if (!img.ReadBlock(umount_data_blk, op)) {
    if (warnings) warnings->push_back("failed to read the unmount record data");
    return false;
  }
  const uint8_t flags = op[lf::kOpFlags];
  if ((flags & lf::kFlagUnmountTrans) != 0) {
    *new_tail = after_umount_blk;
    return true;
  }
  return false;
}

struct DiscoveredLog {
  LogState state = LogState::kUnknown;
  uint32_t head_block = 0;
  uint32_t head_cycle = 0;
  uint32_t tail_block = 0;
  uint32_t tail_cycle = 0;
  uint32_t prev_block = 0;
  uint64_t newest_lsn = 0;
  uint64_t newest_tail_lsn = 0;
  bool wrapped = false;
};

// xlog_find_tail().
Result<DiscoveredLog> FindTail(LogImage& img, bool logv2, uint32_t logbsize,
                               const XfsSuperblock& sb,
                               std::vector<std::string>* warnings) {
  DiscoveredLog out;
  auto head = FindHead(img, logv2, sb, warnings);
  if (!head.ok()) return head.error();
  uint32_t head_blk = head.value();

  if (head_blk == 0) {
    auto cycle0 = img.ReadCycle(0);
    if (!cycle0.ok()) return cycle0.error();
    if (cycle0.value() == 0) {
      // Either nothing was ever written (empty log) or the space in front of
      // the head is garbage that happens to end in a zero cycle. The kernel
      // would treat both as "no recovery needed"; this build keeps the old
      // refusal for the second case, so look at the whole ring.
      std::vector<uint8_t> block(512);
      bool all_zero = true;
      for (uint32_t i = 0; i < img.blocks() && all_zero; ++i) {
        if (!img.ReadBlock(i, block.data())) {
          return XFS_ERR(XfsError::kIoError, "failed to read the log")
              .Metadata("log");
        }
        for (uint8_t byte : block) {
          if (byte != 0) {
            all_zero = false;
            break;
          }
        }
      }
      if (all_zero) {
        out.state = LogState::kEmpty;
        out.head_block = 0;
        out.tail_block = 0;
        return out;
      }
      out.state = LogState::kUnknown;
      out.head_block = 0;
      out.tail_block = 0;
      if (warnings) {
        warnings->push_back(
            "log contains data but no valid record headers");
      }
      return out;
    }
  }

  auto sync = RseekLogrecHdr(img, head_blk, head_blk, 1);
  if (!sync.ok()) return sync.error();
  if (sync.value().found == 0) {
    // Data in the ring but no record header anywhere: not a log this build
    // can trust. The mount policy treats this like an unknown (dirty) log.
    out.state = LogState::kUnknown;
    out.head_block = 0;
    out.tail_block = 0;
    if (warnings) {
      warnings->push_back("log contains data but no valid record headers");
    }
    return out;
  }
  const uint32_t rhead_blk = sync.value().block;
  RecordHeader rhead = sync.value().header;
  const bool wrapped = sync.value().wrapped;

  out.tail_block = static_cast<uint32_t>(rhead.tail_lsn & 0xffffffffu);
  out.tail_cycle = static_cast<uint32_t>(rhead.tail_lsn >> 32);
  out.head_block = head_blk;
  out.head_cycle = rhead.cycle + (wrapped ? 1 : 0);
  out.prev_block = rhead_blk;
  out.newest_lsn = rhead.lsn;
  out.newest_tail_lsn = rhead.tail_lsn;
  out.wrapped = wrapped;

  bool clean = false;
  uint32_t tail_blk = out.tail_block;
  if (UnmountRecordAt(img, rhead_blk, rhead, head_blk, &tail_blk, warnings)) {
    clean = true;
    out.tail_block = tail_blk;
    out.tail_cycle = out.head_cycle;
  }

  if (!clean) {
    const uint32_t orig_head = head_blk;
    NewestRecord newest{rhead_blk, rhead, wrapped};
    auto verified = VerifyHead(img, &head_blk, &tail_blk, logv2, logbsize, sb,
                               &newest, warnings);
    if (!verified.ok()) return verified.error();
    if (!verified.value().ok()) return verified.value().error();
    if (head_blk != orig_head) {
      out.head_block = head_blk;
      out.prev_block = newest.block;
      out.head_cycle = newest.header.cycle + (newest.wrapped ? 1 : 0);
      out.newest_lsn = newest.header.lsn;
      out.newest_tail_lsn = newest.header.tail_lsn;
      out.wrapped = newest.wrapped;
      out.tail_block = static_cast<uint32_t>(newest.header.tail_lsn & 0xffffffffu);
      out.tail_cycle = static_cast<uint32_t>(newest.header.tail_lsn >> 32);
      uint32_t new_tail = out.tail_block;
      if (UnmountRecordAt(img, newest.block, newest.header, head_blk, &new_tail,
                          warnings)) {
        clean = true;
        out.tail_block = new_tail;
        out.tail_cycle = out.head_cycle;
      } else {
        tail_blk = out.tail_block;
      }
    }
  }

  out.state = clean ? LogState::kClean : LogState::kDirty;
  return out;
}

// ---------------------------------------------------------------------------
// Record iteration (xlog_do_recovery_pass)
// ---------------------------------------------------------------------------
Result<int> IterateRecords(LogImage& img, uint32_t head_blk, uint32_t tail_blk,
                           RecordPass pass, bool logv2, uint32_t logbsize,
                           const XfsSuperblock& sb, const RecordHandler& handler,
                           uint32_t* first_bad, bool* first_bad_set,
                           std::vector<std::string>* warnings) {
  if (head_blk == tail_blk) {
    return XFS_ERR(XfsError::kInternal, "empty recovery window")
        .Metadata("log");
  }
  const uint32_t log_bbnum = img.blocks();

  // Determine h_size and the header blocks from the tail record.
  uint32_t h_size = 32 * 1024;
  uint32_t hblks = 1;
  uint8_t first[512];
  if (!img.ReadBlock(tail_blk, first)) {
    return XFS_ERR(XfsError::kIoError, "failed to read the log")
        .Metadata("log");
  }
  if (logv2) {
    RecordHeader h = ParseHeader(first);
    h_size = h.hsize;
    const uint32_t h_len = h.len;
    if (h_len > h_size && h_len <= logbsize && h.num_ops == 1) {
      if (warnings) {
        warnings->push_back("invalid iclog size " + std::to_string(h_size) +
                            " bytes, using log buffer size " +
                            std::to_string(logbsize));
      }
      h_size = logbsize;
    }
    auto valid = ValidateRecordAt(h, tail_blk, logv2, log_bbnum, h_size,
                                  "log record");
    if (!valid.ok()) return valid.error();
    if (h_size > kHeaderCycleBytes && (h.version & kVersion2)) {
      hblks = (h_size + kHeaderCycleBytes - 1) / kHeaderCycleBytes;
    }
  }

  std::vector<uint8_t> hbp(static_cast<size_t>(hblks) * 512);
  std::vector<uint8_t> dbp(static_cast<size_t>(h_size));
  std::vector<uint8_t> data;

  auto fail_record = [&](uint32_t at_block) {
    if (first_bad) *first_bad = at_block;
    if (first_bad_set) *first_bad_set = true;
  };

  // Processes one fully read record. `raw` holds the data area as written.
  auto process = [&](uint32_t rblk, uint32_t blk_no, RecordHeader header,
                     std::vector<uint8_t>* raw) -> Status {
    auto valid = ValidateRecordAt(header, blk_no, logv2, log_bbnum, h_size,
                                  "log record");
    if (!valid.ok()) {
      fail_record(rblk);
      return valid.error();
    }
    uint32_t computed = 0;
    if (!VerifyRecordCrc(hbp, raw->data(), raw->size(), &computed)) {
      fail_record(rblk);
      return XFS_ERR(XfsError::kBadCrc, "log record CRC mismatch")
          .Expected(GetLE32(hbp.data(), lf::kRecCrc))
          .Actual(computed)
          .Metadata("log");
    }
    if (pass == RecordPass::kCrc) return OkStatus();

    auto header_ok = HeaderCheckRecover(hbp.data(), sb);
    if (!header_ok.ok()) {
      fail_record(rblk);
      return header_ok;
    }

    LogRecordInfo info;
    info.lsn = header.lsn;
    info.block = rblk;
    info.cycle = header.cycle;
    info.header_blocks = hblks;  // the pass-wide header block count
    info.data_blocks = header.data_blocks();
    info.num_ops = header.num_ops;
    info.prev_block = header.prev_block;
    info.tail_lsn = header.tail_lsn;
    info.computed_crc = computed;
    info.crc_stored = GetLE32(hbp.data(), lf::kRecCrc) != 0;

    UnpackCycleData(hbp, raw);
    if (handler) {
      Status status = handler(info, *raw);
      if (!status.ok()) {
        fail_record(rblk);
        return status;
      }
    }
    return OkStatus();
  };

  uint32_t blk_no = tail_blk;
  Status error = OkStatus();
  if (tail_blk > head_blk) {
    // Records wrap around the physical end of the log.
    while (blk_no < log_bbnum) {
      uint32_t split_hblks = 0;
      uint32_t wrapped_hblks = 0;
      if (blk_no + hblks <= log_bbnum) {
        if (!img.ReadPhysical(blk_no, hblks, hbp.data())) {
          fail_record(blk_no);
          return XFS_ERR(XfsError::kIoError, "failed to read the log")
              .Metadata("log");
        }
      } else {
        if (blk_no != log_bbnum) {
          split_hblks = log_bbnum - blk_no;
          if (!img.ReadPhysical(blk_no, split_hblks, hbp.data())) {
            fail_record(blk_no);
            return XFS_ERR(XfsError::kIoError, "failed to read the log")
                .Metadata("log");
          }
        }
        wrapped_hblks = hblks - split_hblks;
        if (!img.ReadPhysical(0, wrapped_hblks,
                              hbp.data() + static_cast<size_t>(split_hblks) * 512)) {
          fail_record(blk_no);
          return XFS_ERR(XfsError::kIoError, "failed to read the log")
              .Metadata("log");
        }
      }
      const uint32_t rblk = blk_no;
      const uint32_t header_block = blk_no;
      RecordHeader header = ParseHeader(hbp.data());
      auto valid = ValidateRecordAt(header, header_block, logv2,
                                    log_bbnum, h_size, "log record");
      if (!valid.ok()) {
        fail_record(rblk);
        return valid.error();
      }
      const uint32_t bblks = header.data_blocks();
      blk_no += hblks;

      data.assign(static_cast<size_t>(bblks) * 512, 0);
      if (blk_no + bblks <= log_bbnum || blk_no >= log_bbnum) {
        const uint32_t rblk_no = img.geometry().Wrap(blk_no);
        if (!img.ReadRange(rblk_no, bblks, data.data())) {
          fail_record(rblk);
          return XFS_ERR(XfsError::kIoError, "failed to read the log")
              .Metadata("log");
        }
      } else {
        const uint32_t split_bblks = log_bbnum - blk_no;
        if (split_bblks) {
          if (!img.ReadPhysical(blk_no, split_bblks, data.data())) {
            fail_record(rblk);
            return XFS_ERR(XfsError::kIoError, "failed to read the log")
                .Metadata("log");
          }
        }
        if (!img.ReadPhysical(0, bblks - split_bblks,
                              data.data() + static_cast<size_t>(split_bblks) * 512)) {
          fail_record(rblk);
          return XFS_ERR(XfsError::kIoError, "failed to read the log")
              .Metadata("log");
        }
      }
      error = process(rblk, header_block, header, &data);
      if (!error.ok()) return error.error();
      blk_no += bblks;
    }
    blk_no -= log_bbnum;
  }

  while (blk_no < head_blk) {
    if (!img.ReadPhysical(blk_no, hblks, hbp.data())) {
      fail_record(blk_no);
      return XFS_ERR(XfsError::kIoError, "failed to read the log")
          .Metadata("log");
    }
    const uint32_t rblk = blk_no;
    RecordHeader header = ParseHeader(hbp.data());
    auto valid = ValidateRecordAt(header, blk_no, logv2, log_bbnum, h_size,
                                  "log record");
    if (!valid.ok()) {
      fail_record(rblk);
      return valid.error();
    }
    const uint32_t bblks = header.data_blocks();
    data.assign(static_cast<size_t>(bblks) * 512, 0);
    if (blk_no + hblks + bblks > log_bbnum) {
      // Split across the physical end (the head is before the end, so this is
      // data-only wrap).
      const uint32_t dblk = blk_no + hblks;
      const uint32_t split = log_bbnum - dblk;
      if (!img.ReadPhysical(dblk, split, data.data())) {
        fail_record(rblk);
        return XFS_ERR(XfsError::kIoError, "failed to read the log")
            .Metadata("log");
      }
      if (!img.ReadPhysical(0, bblks - split,
                            data.data() + static_cast<size_t>(split) * 512)) {
        fail_record(rblk);
        return XFS_ERR(XfsError::kIoError, "failed to read the log")
            .Metadata("log");
      }
    } else if (!img.ReadPhysical(blk_no + hblks, bblks, data.data())) {
      fail_record(rblk);
      return XFS_ERR(XfsError::kIoError, "failed to read the log")
          .Metadata("log");
    }
    error = process(rblk, blk_no, header, &data);
    if (!error.ok()) return error.error();
    blk_no += bblks + hblks;
  }
  (void)dbp;
  return 0;
}

// ---------------------------------------------------------------------------
// Transaction reconstruction (xlog_recover_add_to_trans and friends)
// ---------------------------------------------------------------------------
struct ItemBuilder {
  std::vector<std::vector<uint8_t>> regions;
  uint32_t total = 0;  // ri_total: regions the format header promises
  bool complete() const { return total != 0 && regions.size() == total; }
};

struct TransBuilder {
  uint32_t tid = 0;
  uint64_t lsn = 0;
  bool has_item = false;      // an (possibly empty) item is open
  ItemBuilder item;           // the open item
  std::vector<RecoveredItem> items;  // closed items of this transaction
  uint8_t theader[lf::kTransHeaderSize] = {};
  uint32_t theader_bytes = 0;
};

class ReconstructionState {
 public:
  ReconstructionState() = default;

  // Feeds one record's worth of operations. Committed transactions are
  // appended to transactions().
  Status ProcessRecord(const LogRecordInfo& info, const std::vector<uint8_t>& data);

  const std::vector<RecoveredTransaction>& transactions() const {
    return transactions_;
  }
  const std::vector<std::string>& unsupported() const { return unsupported_; }

 private:
  TransBuilder* Find(uint32_t tid) {
    auto it = open_.find(tid);
    return it == open_.end() ? nullptr : &it->second;
  }

  Status CloseItem(TransBuilder* trans);
  Status AddToTrans(TransBuilder* trans, const uint8_t* dp, uint32_t len);
  Status AddToContTrans(TransBuilder* trans, const uint8_t* dp, uint32_t len);
  Status CommitTrans(TransBuilder* trans);
  Status ProcessOphdr(uint32_t tid, uint8_t flags, const uint8_t* dp,
                      uint32_t len, const LogRecordInfo& info);

  std::unordered_map<uint32_t, TransBuilder> open_;
  std::vector<RecoveredTransaction> transactions_;
  std::vector<std::string> unsupported_;
};

Status ReconstructionState::CloseItem(TransBuilder* trans) {
  if (!trans->has_item) return OkStatus();
  if (trans->item.regions.empty()) {
    // The placeholder item of a transaction header that never received a
    // region is not a log item.
    trans->has_item = false;
    trans->item = ItemBuilder();
    return OkStatus();
  }
  if (trans->item.regions[0].size() < 2) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "log item format header is too small")
        .Metadata("log item");
  }
  RecoveredItem item;
  item.tid = trans->tid;
  item.type = LoadLE16(trans->item.regions[0].data());
  item.regions = std::move(trans->item.regions);
  trans->items.push_back(std::move(item));
  trans->has_item = false;
  trans->item = ItemBuilder();
  return OkStatus();
}

// xlog_recover_add_to_trans().
Status ReconstructionState::AddToTrans(TransBuilder* trans, const uint8_t* dp,
                                       uint32_t len) {
  if (len == 0) return OkStatus();
  if (!trans->has_item) {
    // Expect the transaction header. It can be split across operations; in
    // that case the remainder arrives through AddToContTrans.
    if (len < 4 || GetLE32(dp) != lf::kTransHeaderMagic) {
      return XFS_ERR(XfsError::kCorruptBtree, "bad transaction header magic")
          .Metadata("log item");
    }
    if (len > lf::kTransHeaderSize) {
      return XFS_ERR(XfsError::kCorruptBtree, "bad transaction header length")
          .Actual(len)
          .Metadata("log item");
    }
    std::memcpy(trans->theader, dp, len);
    trans->theader_bytes = len;
    if (len == lf::kTransHeaderSize) {
      trans->has_item = true;  // empty placeholder item for the first region
    }
    return OkStatus();
  }

  if (trans->item.total != 0 && trans->item.regions.size() == trans->item.total) {
    // The tail item is complete: keep it and start a new one.
    Status closed = CloseItem(trans);
    if (!closed.ok()) return closed;
  }
  if (!trans->has_item) {
    trans->has_item = true;
    trans->item = ItemBuilder();
  }
  if (trans->item.total == 0) {
    // First region: its first 32 bits hold the type and size of the item.
    if (len < 4) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "log item region smaller than its format header")
          .Actual(len)
          .Metadata("log item");
    }
    const uint32_t ilf_size = LoadLE16(static_cast<const uint8_t*>(dp) + 2);
    if (ilf_size == 0 || ilf_size > kMaxRegionsInItem) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "bad number of regions in a log item")
          .Actual(ilf_size)
          .Metadata("log item");
    }
    trans->item.total = ilf_size;
  }
  if (trans->item.regions.size() >= trans->item.total) {
    return XFS_ERR(XfsError::kCorruptBtree, "log item region count overflow")
        .Actual(trans->item.regions.size())
        .Metadata("log item");
  }
  trans->item.regions.emplace_back(dp, dp + len);
  return OkStatus();
}

// xlog_recover_add_to_cont_trans(): continues the previous op's data.
Status ReconstructionState::AddToContTrans(TransBuilder* trans,
                                           const uint8_t* dp, uint32_t len) {
  if (!trans->has_item) {
    // The transaction header was split across records.
    if (len > lf::kTransHeaderSize) {
      return XFS_ERR(XfsError::kCorruptBtree, "bad transaction header length")
          .Actual(len)
          .Metadata("log item");
    }
    trans->has_item = true;
    const uint32_t at = lf::kTransHeaderSize - len;
    std::memcpy(trans->theader + at, dp, len);
    trans->theader_bytes = lf::kTransHeaderSize;
    return OkStatus();
  }
  if (trans->item.regions.empty()) {
    // A continuation without a region to extend: only legal for the split
    // transaction header, handled above.
    return XFS_ERR(XfsError::kCorruptBtree,
                   "log continuation without a region to extend")
        .Metadata("log item");
  }
  std::vector<uint8_t>& tail = trans->item.regions.back();
  tail.insert(tail.end(), dp, dp + len);
  return OkStatus();
}

// xlog_recover_commit_trans(): reorder and close the transaction.
Status ReconstructionState::CommitTrans(TransBuilder* trans) {
  Status closed = CloseItem(trans);
  if (!closed.ok()) return closed;
  if (trans->items.empty()) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "committed transaction with no items")
        .Metadata("log item");
  }

  // xlog_recover_reorder_trans(): buffer items first, then non-buffer items,
  // then inode buffers, then cancelled buffers (which are dropped in pass 2).
  std::vector<RecoveredItem> buffers;
  std::vector<RecoveredItem> items;
  std::vector<RecoveredItem> inode_buffers;
  std::vector<RecoveredItem> cancelled;
  for (RecoveredItem& item : trans->items) {
    const RecoveryItemClass cls = ClassifyLogItem(item.type);
    if (cls == RecoveryItemClass::kBuffer) {
      const uint16_t blf_flags =
          item.regions[0].size() >= 6
              ? LoadLE16(item.regions[0].data() + 4)
              : 0;
      const uint16_t blft = static_cast<uint16_t>((blf_flags >> lf::kBlftShift) &
                                                  lf::kBlftMask);
      if (blf_flags & lf::kBlfCancel) {
        cancelled.push_back(std::move(item));
      } else if (blft == lf::kBlftDino) {
        inode_buffers.push_back(std::move(item));
      } else {
        buffers.push_back(std::move(item));
      }
      continue;
    }
    if (cls == RecoveryItemClass::kIcreate) {
      buffers.push_back(std::move(item));  // XLOG_REORDER_BUFFER_LIST
      continue;
    }
    if (cls == RecoveryItemClass::kRefused || cls == RecoveryItemClass::kUnknown) {
      unsupported_.push_back(
          "unsupported recovery item " +
          std::string(LogItemTypeName(item.type) ? LogItemTypeName(item.type)
                                                 : "unknown") +
          " (0x" + [&] {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%04x", item.type);
            return std::string(buf);
          }() +
          ") transaction tid=" + std::to_string(trans->tid) + " lsn=" +
          LsnText(trans->lsn));
    }
    items.push_back(std::move(item));
  }

  RecoveredTransaction out;
  out.tid = trans->tid;
  out.lsn = trans->lsn;
  out.items.reserve(trans->items.size() + 1);
  for (auto& item : buffers) out.items.push_back(std::move(item));
  for (auto& item : items) out.items.push_back(std::move(item));
  for (auto& item : inode_buffers) out.items.push_back(std::move(item));
  for (auto& item : cancelled) out.items.push_back(std::move(item));
  transactions_.push_back(std::move(out));
  return OkStatus();
}

// xlog_recovery_process_trans(): the flag state machine.
Status ReconstructionState::ProcessOphdr(uint32_t tid, uint8_t flags,
                                         const uint8_t* dp, uint32_t len,
                                         const LogRecordInfo& info) {
  TransBuilder* trans = Find(tid);
  bool created = false;
  if (trans == nullptr) {
    if ((flags & lf::kFlagStartTrans) == 0) {
      return OkStatus();  // slack space before the next START
    }
    if (len != 0) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "START_TRANS operation carries data")
          .Actual(len)
          .Metadata("log op");
    }
    TransBuilder fresh;
    fresh.tid = tid;
    fresh.lsn = info.lsn;
    auto inserted = open_.emplace(tid, std::move(fresh));
    trans = &inserted.first->second;
    created = true;
  }
  if (created) return OkStatus();  // nothing more to do for the START op

  uint8_t effective = flags & ~lf::kFlagEndTrans;
  if (effective & lf::kFlagWasContTrans) effective &= ~lf::kFlagContinueTrans;
  switch (effective) {
    case 0:
    case lf::kFlagContinueTrans:
      return AddToTrans(trans, dp, len);
    case lf::kFlagWasContTrans:
      return AddToContTrans(trans, dp, len);
    case lf::kFlagCommitTrans: {
      Status s = CommitTrans(trans);
      open_.erase(tid);
      return s;
    }
    case lf::kFlagUnmountTrans:
      // The unmount record is not replayed; it only marks a clean log.
      open_.erase(tid);
      return OkStatus();
    default:
      return XFS_ERR(XfsError::kCorruptBtree, "bad log operation flags")
          .Actual(effective)
          .Metadata("log op");
  }
}

Status ReconstructionState::ProcessRecord(const LogRecordInfo& info,
                                          const std::vector<uint8_t>& data) {
  const uint8_t* dp = data.data();
  const uint8_t* end = data.data() + data.size();
  uint32_t num_ops = info.num_ops;
  while (dp < end && num_ops > 0) {
    if (dp + lf::kOpHeaderSize > end) {
      return XFS_ERR(XfsError::kCorruptBtree, "log op header overrun")
          .Metadata("log op");
    }
    const uint32_t tid = GetBE32(dp, lf::kOpTid);
    const uint32_t len = GetBE32(dp, lf::kOpLen);
    const uint8_t client = dp[lf::kOpClientId];
    const uint8_t flags = dp[lf::kOpFlags];
    dp += lf::kOpHeaderSize;
    if (client != lf::kClientTransaction && client != lf::kClientLog) {
      return XFS_ERR(XfsError::kCorruptBtree, "bad log operation client id")
          .Actual(client)
          .Metadata("log op");
    }
    if (dp + len > end) {
      return XFS_ERR(XfsError::kCorruptBtree, "log operation overrun")
          .Actual(len)
          .Metadata("log op");
    }
    Status s = ProcessOphdr(tid, flags, dp, len, info);
    if (!s.ok()) return s;
    dp += len;
    --num_ops;
  }
  if (num_ops != 0) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "log record data shorter than h_num_logops")
        .Actual(num_ops)
        .Metadata("log record");
  }
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Item naming / classification
// ---------------------------------------------------------------------------
struct ItemTypeEntry {
  uint16_t type;
  const char* name;
  RecoveryItemClass cls;
};

constexpr ItemTypeEntry kItemTypes[] = {
    {lf::kItemBuf, "XFS_LI_BUF", RecoveryItemClass::kBuffer},
    {lf::kItemInode, "XFS_LI_INODE", RecoveryItemClass::kInode},
    {lf::kItemIcreate, "XFS_LI_ICREATE", RecoveryItemClass::kIcreate},
    {lf::kItemEfi, "XFS_LI_EFI", RecoveryItemClass::kIntent},
    {lf::kItemEfd, "XFS_LI_EFD", RecoveryItemClass::kIntentDone},
    {lf::kItemBui, "XFS_LI_BUI", RecoveryItemClass::kIntent},
    {lf::kItemBud, "XFS_LI_BUD", RecoveryItemClass::kIntentDone},
    {lf::kItemIunlink, "XFS_LI_IUNLINK", RecoveryItemClass::kIntent},
    {lf::kItemDquot, "XFS_LI_DQUOT", RecoveryItemClass::kRefused},
    {lf::kItemQuotaoff, "XFS_LI_QUOTAOFF", RecoveryItemClass::kRefused},
    {lf::kItemRui, "XFS_LI_RUI", RecoveryItemClass::kIntent},
    {lf::kItemRud, "XFS_LI_RUD", RecoveryItemClass::kIntentDone},
    {lf::kItemCui, "XFS_LI_CUI", RecoveryItemClass::kIntent},
    {lf::kItemCud, "XFS_LI_CUD", RecoveryItemClass::kIntentDone},
    {lf::kItemAttri, "XFS_LI_ATTRI", RecoveryItemClass::kIntent},
    {lf::kItemAttrd, "XFS_LI_ATTRD", RecoveryItemClass::kIntentDone},
    // Exchange-range and realtime intents are intentionally refused before
    // the first write.  The exchange-range feature bit is accepted because
    // ordinary namespace/attr/reflink operations do not create XMI/XMD, but a
    // log that actually contains one must never be replayed halfway.
    {lf::kItemXmi, "XFS_LI_XMI", RecoveryItemClass::kRefused},
    {lf::kItemXmd, "XFS_LI_XMD", RecoveryItemClass::kRefused},
    {lf::kItemEfiRt, "XFS_LI_EFI_RT", RecoveryItemClass::kRefused},
    {lf::kItemEfdRt, "XFS_LI_EFD_RT", RecoveryItemClass::kRefused},
    {lf::kItemRuiRt, "XFS_LI_RUI_RT", RecoveryItemClass::kRefused},
    {lf::kItemRudRt, "XFS_LI_RUD_RT", RecoveryItemClass::kRefused},
    {lf::kItemCuiRt, "XFS_LI_CUI_RT", RecoveryItemClass::kRefused},
    {lf::kItemCudRt, "XFS_LI_CUD_RT", RecoveryItemClass::kRefused},
};

}  // namespace

const char* ToString(RecoveryItemClass value) {
  switch (value) {
    case RecoveryItemClass::kBuffer: return "buffer";
    case RecoveryItemClass::kInode: return "inode";
    case RecoveryItemClass::kIcreate: return "icreate";
    case RecoveryItemClass::kIntent: return "intent";
    case RecoveryItemClass::kIntentDone: return "intent-done";
    case RecoveryItemClass::kRefused: return "refused";
    case RecoveryItemClass::kUnknown: return "unknown";
  }
  return "?";
}

const char* LogItemTypeName(uint16_t type) {
  for (const ItemTypeEntry& entry : kItemTypes) {
    if (entry.type == type) return entry.name;
  }
  return nullptr;
}

RecoveryItemClass ClassifyLogItem(uint16_t type) {
  for (const ItemTypeEntry& entry : kItemTypes) {
    if (entry.type == type) return entry.cls;
  }
  return RecoveryItemClass::kUnknown;
}

uint64_t LogScan::ItemCount() const {
  uint64_t count = 0;
  for (const RecoveredTransaction& trans : transactions) {
    count += trans.items.size();
  }
  return count;
}

Result<RecoveryGeometry> MakeRecoveryGeometry(const XfsSuperblock& sb,
                                              const XfsGeometry& geo) {
  RecoveryGeometry out;
  if (sb.logstart == 0 || sb.logblocks == 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "no internal log to recover")
        .Metadata("log");
  }
  const uint64_t log_bytes = static_cast<uint64_t>(sb.logblocks) << geo.block_log;
  if (log_bytes % 512 != 0 || log_bytes / 512 > 0x7fffffffu) {
    return XFS_ERR(XfsError::kUnsupportedGeometry, "invalid log size")
        .Actual(log_bytes)
        .Metadata("log");
  }
  out.log_start = geo.FsbToByte(sb.logstart);
  out.log_blocks = static_cast<uint32_t>(log_bytes / 512);
  out.sector_blocks = sb.logsectsize ? sb.logsectsize / 512 : 1;
  if (out.sector_blocks == 0) out.sector_blocks = 1;
  if (out.log_start + log_bytes > 0 && out.log_blocks == 0) {
    return XFS_ERR(XfsError::kUnsupportedGeometry, "empty log")
        .Metadata("log");
  }
  return out;
}

Result<LogScan> ScanLog(BlockDevice& dev, const XfsSuperblock& sb,
                        const XfsGeometry& geo) {
  auto geometry = MakeRecoveryGeometry(sb, geo);
  if (!geometry.ok()) return geometry.error();
  LogImage img(dev, geometry.value());

  LogScan scan;
  scan.geometry = geometry.value();
  const bool logv2 = HasLogV2(sb);
  // m_logbsize: sb_logsunit when it is larger than one big record, else 32 KiB
  // (xfs_super.c). Only used for the xfsprogs iclog-size quirk.
  const uint32_t logbsize =
      sb.logsunit > kHeaderCycleBytes ? sb.logsunit : kHeaderCycleBytes;

  auto discovered = FindTail(img, logv2, logbsize, sb, &scan.warnings);
  if (!discovered.ok()) {
    if (IsLogConsistencyError(discovered.error().code)) {
      scan.state = LogState::kUnknown;
      scan.detail = "log is inconsistent: " + discovered.error().message;
      scan.warnings.push_back(scan.detail);
      return scan;
    }
    return discovered.error();
  }
  const DiscoveredLog& d = discovered.value();
  scan.state = d.state;
  scan.head_block = d.head_block;
  scan.head_cycle = d.head_cycle;
  scan.tail_block = d.tail_block;
  scan.tail_cycle = d.tail_cycle;
  scan.prev_block = d.prev_block;
  scan.newest_lsn = d.newest_lsn;
  scan.tail_lsn = d.newest_tail_lsn;
  scan.wrapped = d.wrapped;

  switch (d.state) {
    case LogState::kEmpty:
      scan.detail = "log is zeroed (freshly formatted)";
      return scan;
    case LogState::kClean:
      scan.detail = "newest log record is an unmount record";
      scan.ends_with_unmount_record = true;
      return scan;
    case LogState::kUnknown:
      scan.detail = "log contains data but no valid record headers";
      return scan;
    default:
      break;
  }
  scan.detail = "log needs replay";

  if (d.tail_block == d.head_block) {
    // The tail already points at the head: nothing is outstanding.
    scan.state = LogState::kClean;
    scan.detail = "tail is at the head; nothing to replay";
    return scan;
  }

  // Validate the whole window, then reconstruct the committed transactions.
  ReconstructionState reconstruction;
  auto handler = [&](const LogRecordInfo& info,
                     const std::vector<uint8_t>& data) -> Status {
    scan.records.push_back(info);
    return reconstruction.ProcessRecord(info, data);
  };
  uint32_t first_bad = 0;
  bool first_bad_set = false;
  auto iter = IterateRecords(img, d.head_block, d.tail_block,
                             RecordPass::kReconstruct, logv2, logbsize, sb,
                             handler, &first_bad, &first_bad_set, &scan.warnings);
  if (!iter.ok()) {
    if (IsLogConsistencyError(iter.error().code)) {
      scan.state = LogState::kUnknown;
      scan.detail = "log is inconsistent: " + iter.error().message;
      scan.warnings.push_back(scan.detail);
      return scan;
    }
    return iter.error();
  }

  scan.transactions = reconstruction.transactions();
  scan.unsupported_items = reconstruction.unsupported();

  // Inventory and validate every RUI/RUD before replay.  This includes
  // operations that are already paired (a matching RUD later in the log):
  // shared/reflink intent must refuse even if it would not need completion.
  for (const RecoveredTransaction& trans : scan.transactions) {
    for (const RecoveredItem& item : trans.items) {
      if (item.type == lf::kItemRui) {
        if (!geo.features.rmapbt) {
          scan.unsupported_items.push_back(
              "RUI item present on a filesystem without rmapbt");
          continue;
        }
        if (item.regions.empty()) {
          scan.unsupported_items.push_back("RUI item has no format region");
          continue;
        }
        auto parsed = ParseRui(item.regions[0], geo);
        if (!parsed.ok()) {
          scan.unsupported_items.push_back(
              std::string("RUI preflight: ") + parsed.error().Format());
        }
      } else if (item.type == lf::kItemRud) {
        if (item.regions.empty()) {
          scan.unsupported_items.push_back("RUD item has no format region");
          continue;
        }
        auto parsed = ParseRud(item.regions[0]);
        if (!parsed.ok()) {
          scan.unsupported_items.push_back(
              std::string("RUD preflight: ") + parsed.error().Format());
        }
      } else if (item.type == lf::kItemBui) {
        if (item.regions.empty()) {
          scan.unsupported_items.push_back("BUI item has no format region");
          continue;
        }
        auto parsed = ParseBmapIntent(item.regions[0], geo);
        if (!parsed.ok()) {
          scan.unsupported_items.push_back(
              std::string("BUI preflight: ") + parsed.error().Format());
        }
      } else if (item.type == lf::kItemBud) {
        if (item.regions.empty()) {
          scan.unsupported_items.push_back("BUD item has no format region");
          continue;
        }
        auto parsed = ParseBmapIntentDone(item.regions[0]);
        if (!parsed.ok()) {
          scan.unsupported_items.push_back(
              std::string("BUD preflight: ") + parsed.error().Format());
        }
      } else if (item.type == lf::kItemCui) {
        if (!geo.features.reflink) {
          scan.unsupported_items.push_back(
              "CUI item present on a filesystem without reflink");
          continue;
        }
        if (item.regions.empty()) {
          scan.unsupported_items.push_back("CUI item has no format region");
          continue;
        }
        auto parsed = ParseRefcountIntent(item.regions[0], geo);
        if (!parsed.ok()) {
          scan.unsupported_items.push_back(
              std::string("CUI preflight: ") + parsed.error().Format());
        }
      } else if (item.type == lf::kItemCud) {
        if (item.regions.empty()) {
          scan.unsupported_items.push_back("CUD item has no format region");
          continue;
        }
        auto parsed = ParseRefcountIntentDone(item.regions[0]);
        if (!parsed.ok()) {
          scan.unsupported_items.push_back(
              std::string("CUD preflight: ") + parsed.error().Format());
        }
      } else if (item.type == lf::kItemAttri) {
        auto parsed = ParseXattrIntent(item.regions, geo);
        if (!parsed.ok()) {
          scan.unsupported_items.push_back(
              std::string("ATTRI preflight: ") + parsed.error().Format());
        }
      } else if (item.type == lf::kItemAttrd) {
        if (item.regions.empty()) {
          scan.unsupported_items.push_back("ATTRD item has no format region");
          continue;
        }
        auto parsed = ParseXattrIntentDone(item.regions[0]);
        if (!parsed.ok()) {
          scan.unsupported_items.push_back(
              std::string("ATTRD preflight: ") + parsed.error().Format());
        }
      }
    }
  }

  // A done item can outlive its intent in the replay window once the tail
  // advances. xlog_recover_release_intent cancels only earlier active matches;
  // an unmatched RUD is legal, not a reason to refuse an otherwise valid log.
  // CollectPendingIntents preserves this ordering (including reused IDs).

  // An unresolved intent this build cannot finish is a refusal too: replaying
  // its metadata would be safe, but the deferred operation could not be
  // completed (xlog_recover_process_intents). EFI and RUI have completion
  // passes; every other intent type refuses before the first write.
  for (const PendingIntent& intent : CollectPendingIntents(scan)) {
    if (intent.type == lf::kItemEfi || intent.type == lf::kItemRui ||
        intent.type == lf::kItemCui || intent.type == lf::kItemBui ||
        intent.type == lf::kItemAttri) {
      continue;
    }
    scan.unsupported_items.push_back(
        "unresolved intent " +
        std::string(LogItemTypeName(intent.type) ? LogItemTypeName(intent.type)
                                                 : "unknown") +
        " transaction tid=" + std::to_string(intent.tid) + " lsn=" +
        LsnText(intent.lsn) +
        " requires deferred completion this build does not implement");
  }

  XFS_INFO("recovery scan: head=" << d.head_block << " cycle=" << d.head_cycle
                                  << " tail=" << d.tail_block
                                  << " records=" << scan.records.size()
                                  << " transactions=" << scan.transactions.size()
                                  << " items=" << scan.ItemCount());
  for (const std::string& warning : scan.warnings) XFS_WARN("log: " + warning);
  return scan;
}

std::vector<PendingIntent> CollectPendingIntents(const LogScan& scan) {
  std::vector<PendingIntent> pending;
  for (const RecoveredTransaction& trans : scan.transactions) {
    for (const RecoveredItem& item : trans.items) {
      const RecoveryItemClass cls = ClassifyLogItem(item.type);
      if (cls == RecoveryItemClass::kIntent) {
        PendingIntent intent;
        intent.type = item.type;
        intent.tid = trans.tid;
        intent.lsn = trans.lsn;
        intent.regions = item.regions;
        pending.push_back(std::move(intent));
      } else if (cls == RecoveryItemClass::kIntentDone) {
        // xlog_recover_release_intent(): a done item cancels the matching
        // intent (by id) if it was collected from an earlier record. The item
        // formats are host byte order.
        if (item.regions.empty() || item.regions[0].size() < lf::kIntentLogFormatSize) {
          continue;
        }
        const uint64_t id =
            LoadLE64(item.regions[0].data() + lf::kIntentIdOffset);
        uint16_t want_type = lf::kItemBui;
        if (item.type == lf::kItemEfd) {
          want_type = lf::kItemEfi;
        } else if (item.type == lf::kItemRud) {
          want_type = lf::kItemRui;
        } else if (item.type == lf::kItemCud) {
          want_type = lf::kItemCui;
        } else if (item.type == lf::kItemAttrd) {
          want_type = lf::kItemAttri;
        }
        for (auto it = pending.begin(); it != pending.end(); ++it) {
          if (it->type != want_type || it->regions.empty() ||
              it->regions[0].size() < lf::kIntentLogFormatSize) {
            continue;
          }
          if (LoadLE64(it->regions[0].data() + lf::kIntentIdOffset) == id) {
            pending.erase(it);
            break;
          }
        }
      }
    }
  }
  return pending;
}

}  // namespace xfs
