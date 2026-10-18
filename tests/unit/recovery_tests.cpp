// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - log recovery scanner tests (RW-5).
//
// These tests drive the scanner with synthetic logs written block by block
// (head/tail/cycle/wrap/torn-tail/continuation/uncommitted/unsupported), plus
// the real fixture image for the writer-produced cases. They are deliberately
// independent of the writer's positioning logic: the synthetic logs are built
// from the on-disk format constants, so a bug shared with the writer cannot
// hide behind them.
#include <cstring>
#include <string>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/ag.h"
#include "xfs/alloc.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/error.h"
#include "xfs/filesystem.h"
#include "xfs/inode.h"
#include "xfs/log_checker.h"
#include "xfs/log_format.h"
#include "xfs/log_record.h"
#include "xfs/ondisk.h"
#include "xfs/recovery.h"
#include "xfs/superblock.h"

namespace {

namespace lf = xfs::logfmt;
using xfs::GetBE32;
using xfs::GetBE64;
using xfs::LoadLE16;
using xfs::LogOp;
using xfs::LogRecordParams;
using xfs::PutBE32;
using xfs::PutBE64;
using xfs::StoreLE16;
using xfs::StoreLE32;

void PutLE16At(uint8_t* p, size_t off, uint16_t value) {
  StoreLE16(p + off, value);
}
void PutLE32At(uint8_t* p, size_t off, uint32_t value) {
  StoreLE32(p + off, value);
}
void PutLE64At(uint8_t* p, size_t off, uint64_t value) {
  StoreLE32(p + off, static_cast<uint32_t>(value & 0xffffffffu));
  StoreLE32(p + off + 4, static_cast<uint32_t>(value >> 32));
}

constexpr char kUuid[16] = {0x77, 0x66, 0x55, 0x44, 0x33, 0x22,
                            0x41, 0x99, 0x88, 0x77, 0x66, 0x55,
                            0x44, 0x33, 0x22, 0x11};

// A synthetic internal log on a 4 MiB device: 1 MiB into the image, a small
// ring (64 blocks by default), everything else zeroed - the mkfs state.
class SyntheticLog {
 public:
  explicit SyntheticLog(uint32_t log_blocks = 64)
      : log_blocks_(log_blocks), device_(4 * 1024 * 1024) {
    std::memcpy(sb_.uuid.data(), kUuid, 16);
    sb_.meta_uuid = sb_.uuid;
    sb_.versionnum = 0xb4a5;  // v5 + XFS_SB_VERSION_LOGV2BIT
    sb_.blocksize = 4096;
    sb_.sectsize = 512;
    sb_.inodesize = 512;
    sb_.logstart = kLogStartFsb;
    sb_.logblocks = (log_blocks * 512) / 4096;
    sb_.logsectsize = 512;
    sb_.logsunit = 0;

    geo_.block_size = 4096;
    geo_.block_log = 12;
    geo_.agblocks = 1024;
    geo_.agblklog = 10;
    geo_.agcount = 16;
    geo_.dblocks = 1024;
    geo_.logstart = sb_.logstart;
    geo_.logblocks = sb_.logblocks;
    geo_.uuid = sb_.uuid;
    geo_.meta_uuid = sb_.meta_uuid;
  }

  static constexpr uint64_t kLogStartFsb = 256;  // byte 1 MiB

  uint32_t log_blocks() const { return log_blocks_; }
  uint64_t log_byte() const { return 1024 * 1024; }
  xfstest::MemoryBlockDevice& device() { return device_; }
  const xfs::XfsSuperblock& sb() const { return sb_; }
  const xfs::XfsGeometry& geo() const { return geo_; }

  xfs::Result<xfs::LogScan> Scan() {
    return xfs::ScanLog(device_, sb_, geo_);
  }

  // Writes one complete record at a log-relative block. Returns the number of
  // blocks it occupies.
  uint32_t WriteRecord(uint32_t cycle, uint32_t block, uint32_t prev,
                       uint64_t tail_lsn, uint32_t tid,
                       const std::vector<LogOp>& ops) {
    LogRecordParams p;
    p.cycle = cycle;
    p.block = block;
    p.tid = tid;
    p.prev_block = prev;
    p.tail_lsn = tail_lsn;
    p.log_blocks = log_blocks_;
    p.record_size = 32768;
    p.uuid = sb_.uuid;
    auto record = xfs::BuildLogRecord(p, ops);
    CHECK(record.ok());
    CHECK(device_.Write(log_byte() + static_cast<uint64_t>(block) * 512,
                        record.value().data(), record.value().size()));
    return static_cast<uint32_t>(record.value().size() / 512);
  }

  void WriteRaw(uint64_t byte_offset, const void* data, size_t size) {
    CHECK(device_.Write(byte_offset, data, size));
  }
  bool ReadRaw(uint64_t byte_offset, void* data, size_t size) {
    return device_.Read(byte_offset, data, size);
  }

  // Replays the current log content (the caller scanned it first).
  xfs::Result<xfs::ReplayStats> Replay(const xfs::LogScan& scan,
                                       xfs::RecoveredLogState* state) {
    return xfs::ReplayLog(device_, sb_, geo_, scan, state);
  }

 private:
  uint32_t log_blocks_;
  xfstest::MemoryBlockDevice device_;
  xfs::XfsSuperblock sb_;
  xfs::XfsGeometry geo_;
};

// A buffer log item whose bitmap covers `logged_chunks` full chunks: the
// payload is the complete logged region, run-length one.
std::vector<std::vector<uint8_t>> BufItemRegions(
    uint64_t daddr, uint32_t len_blocks, uint16_t blft, uint16_t extra_flags,
    const std::vector<uint8_t>& payload) {
  CHECK(payload.size() % lf::kBufChunkSize == 0);
  const uint32_t chunks = static_cast<uint32_t>(payload.size() / lf::kBufChunkSize);
  const uint32_t map_words = (chunks + 31) / 32;
  std::vector<uint8_t> format(lf::kBufLogFormatSize + map_words * 4, 0);
  PutLE16At(format.data(), 0, lf::kItemBuf);
  PutLE16At(format.data(), 2, 2);  // format + one region
  PutLE16At(format.data(), 4,
            static_cast<uint16_t>((blft << lf::kBlftShift) | extra_flags));
  PutLE16At(format.data(), 6, static_cast<uint16_t>(len_blocks));
  PutLE32At(format.data(), 8, static_cast<uint32_t>(daddr));
  PutLE32At(format.data(), 12, static_cast<uint32_t>(daddr >> 32));
  PutLE32At(format.data(), 16, map_words);
  for (uint32_t i = 0; i < chunks; ++i) {
    format[lf::kBufMapOffset + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
  }
  return {format, payload};
}

// Builds a valid 512-byte AGF image (magic, uuid, lsn, CRC).
std::vector<uint8_t> MakeAgf(uint32_t seqno) {
  std::vector<uint8_t> agf(512, 0);
  xfs::PutBE32(agf.data(), 0, 0x58414746);  // 'XAGF'
  xfs::PutBE32(agf.data(), 4, 1);
  xfs::PutBE32(agf.data(), 8, seqno);
  xfs::PutBE32(agf.data(), 12, 1024);
  std::memcpy(agf.data() + 64, kUuid, 16);
  xfs::UpdateXfsChecksum(agf.data(), agf.size(), 216);
  return agf;
}

// One inode log item (format header + log dinode) as the item regions.
std::vector<std::vector<uint8_t>> InodeItemRegions(uint64_t ino) {
  std::vector<uint8_t> format(lf::kInodeLogFormatSize, 0);
  PutLE16At(format.data(), lf::kIlfType, lf::kItemInode);
  PutLE16At(format.data(), lf::kIlfSize, 2);  // format + log dinode
  PutLE32At(format.data(), lf::kIlfFields, lf::kILogCore);
  PutLE64At(format.data(), lf::kIlfIno, ino);
  std::vector<uint8_t> dinode(lf::kLogDinodeSize, 0);
  StoreLE16(dinode.data(), 0x494e);  // di_magic, host order in the log dinode
  return {format, dinode};
}

// START + transaction header + item regions + COMMIT.
std::vector<LogOp> CommitOps(uint32_t tid,
                             std::vector<std::vector<uint8_t>> regions) {
  std::vector<LogOp> ops;
  ops.push_back(LogOp(lf::kFlagStartTrans, {}));
  std::vector<uint8_t> trans_header;
  xfs::AppendTransHeader(&trans_header, tid,
                         static_cast<uint32_t>(regions.size()));
  ops.push_back(LogOp(0, std::move(trans_header)));
  for (auto& region : regions) ops.push_back(LogOp(0, std::move(region)));
  ops.push_back(LogOp(lf::kFlagCommitTrans, {}));
  return ops;
}

// Fills the whole ring once with 2-block transaction records; returns the
// block after the last record (the head).
uint32_t FillCycle(SyntheticLog* log, uint32_t cycle, uint64_t tail_lsn,
                   uint32_t* next_tid) {
  uint32_t block = 0;
  uint32_t prev = 0xffffffffu;
  while (block + 2 <= log->log_blocks()) {
    std::vector<LogOp> ops = CommitOps((*next_tid)++, InodeItemRegions(128));
    uint32_t used = log->WriteRecord(cycle, block, prev, tail_lsn, 0, ops);
    CHECK_EQ(used, 2u);
    prev = block;
    block += used;
  }
  return block;
}

}  // namespace

TEST(recovery_scan_empty_log_is_empty) {
  SyntheticLog log;
  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().state == xfs::LogState::kEmpty);
  CHECK(scan.value().transactions.empty());
}

TEST(recovery_scan_clean_unmount_record) {
  SyntheticLog log;
  uint32_t used = log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 0,
                                  {xfs::MakeUnmountOp()});
  CHECK_EQ(used, 2u);
  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().state == xfs::LogState::kClean);
  CHECK(scan.value().transactions.empty());
  // The next write goes right after the unmount record, same cycle.
  CHECK_EQ(scan.value().head_block, 2u);
  CHECK_EQ(scan.value().head_cycle, 1u);
  CHECK_EQ(scan.value().prev_block, 0u);
}

TEST(recovery_scan_dirty_single_transaction) {
  SyntheticLog log;
  auto ops = CommitOps(7, InodeItemRegions(128));
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 7, ops);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().state == xfs::LogState::kDirty);
  CHECK(scan.value().Recoverable());
  CHECK_EQ(scan.value().records.size(), 1u);
  CHECK_EQ(scan.value().transactions.size(), 1u);
  if (scan.value().transactions.empty()) return;
  const xfs::RecoveredTransaction& trans = scan.value().transactions[0];
  CHECK_EQ(trans.tid, 7u);
  CHECK_EQ(trans.lsn, xfs::MakeLsn(1, 0));
  CHECK_EQ(trans.items.size(), 1u);
  if (!trans.items.empty()) {
    CHECK_EQ(trans.items[0].type, lf::kItemInode);
    CHECK_EQ(trans.items[0].regions.size(), 2u);
    CHECK(xfs::ClassifyLogItem(trans.items[0].type) ==
          xfs::RecoveryItemClass::kInode);
  }
  // Head/tail as the writer expects them: the next record goes after this one.
  CHECK_EQ(scan.value().tail_block, 0u);
  CHECK_EQ(scan.value().head_block, 2u);
  CHECK_EQ(scan.value().head_cycle, 1u);
}

TEST(recovery_scan_wrap_bumps_the_cycle) {
  // Fill the ring with cycle 1 records, then a full pass of cycle 2 records;
  // the newest record ends exactly at the physical end, so the head must be
  // block 0 with cycle 3 (xlog_set_state's bump_cycle).
  SyntheticLog log;
  uint32_t next_tid = 1;
  uint32_t end = FillCycle(&log, 1, xfs::MakeLsn(1, 0), &next_tid);
  CHECK_EQ(end, log.log_blocks());
  end = FillCycle(&log, 2, xfs::MakeLsn(2, 2), &next_tid);
  CHECK_EQ(end, log.log_blocks());

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().state == xfs::LogState::kDirty);
  CHECK(scan.value().Recoverable());
  CHECK_EQ(scan.value().head_block, 0u);
  CHECK_EQ(scan.value().head_cycle, 3u);
  CHECK_EQ(scan.value().prev_block, 62u);
  CHECK_EQ(scan.value().tail_block, 2u);
  CHECK(scan.value().wrapped);
  // Everything from the tail (cycle 2, block 2) to the head is replayed:
  // 31 records of 2 blocks each.
  CHECK_EQ(scan.value().records.size(), 31u);
  CHECK_EQ(scan.value().transactions.size(), 31u);
}

TEST(recovery_scan_torn_tail_is_trimmed) {
  SyntheticLog log;
  uint32_t next_tid = 1;
  FillCycle(&log, 1, xfs::MakeLsn(1, 0), &next_tid);
  FillCycle(&log, 2, xfs::MakeLsn(2, 2), &next_tid);

  // Corrupt one byte inside the newest record's data area (block 62 is the
  // header, 63 the data block; the cycle stamp occupies the first word, so
  // flip a byte deeper in).
  const uint64_t data_block_byte =
      log.log_byte() + static_cast<uint64_t>(63) * 512;
  uint8_t byte = 0;
  CHECK(log.ReadRaw(data_block_byte + 300, &byte, 1));
  byte ^= 0xA5;
  log.WriteRaw(data_block_byte + 300, &byte, 1);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().state == xfs::LogState::kDirty);
  // The torn record is trimmed; the head walks back to it and the previous
  // record becomes the newest good one.
  CHECK_EQ(scan.value().head_block, 62u);
  CHECK_EQ(scan.value().prev_block, 60u);
  CHECK_EQ(scan.value().records.size(), 30u);
  CHECK_EQ(scan.value().transactions.size(), 30u);
}

TEST(recovery_scan_uncommitted_transaction_is_dropped) {
  SyntheticLog log;
  uint32_t next_tid = 1;
  FillCycle(&log, 1, xfs::MakeLsn(1, 0), &next_tid);
  const uint32_t before = next_tid;
  FillCycle(&log, 2, xfs::MakeLsn(2, 2), &next_tid);

  // Rewrite the newest record (block 62) without its COMMIT op.
  std::vector<LogOp> ops = CommitOps(before, InodeItemRegions(128));
  ops.pop_back();  // drop COMMIT_TRANS
  log.WriteRecord(2, 62, 60, xfs::MakeLsn(2, 2), before, ops);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().state == xfs::LogState::kDirty);
  CHECK_EQ(scan.value().records.size(), 31u);
  // 30 committed transactions: the newest record has no commit, so it is
  // never replayed ("no commit = the transaction never happened").
  CHECK_EQ(scan.value().transactions.size(), 30u);
}

TEST(recovery_scan_unsupported_item_refuses) {
  SyntheticLog log;
  uint32_t next_tid = 1;
  FillCycle(&log, 1, xfs::MakeLsn(1, 0), &next_tid);
  FillCycle(&log, 2, xfs::MakeLsn(2, 2), &next_tid);

  // A committed RUI item (rmap update intent): known type, refused by this
  // build's writable profile.
  std::vector<uint8_t> rui(lf::kIntentLogFormatSize, 0);
  PutLE16At(rui.data(), 0, lf::kItemRui);
  PutLE16At(rui.data(), 2, 1);  // one region
  const uint32_t tid = next_tid;
  auto ops = CommitOps(tid, {rui});
  log.WriteRecord(2, 62, 60, xfs::MakeLsn(2, 2), tid, ops);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(!scan.value().Recoverable());
  CHECK_EQ(scan.value().unsupported_items.size(), 1u);
  if (!scan.value().unsupported_items.empty()) {
    CHECK(scan.value().unsupported_items[0].find("XFS_LI_RUI") !=
          std::string::npos);
  }
}

TEST(recovery_scan_keeps_every_item_of_a_transaction) {
  // Regression: a transaction with several items must keep all of them (the
  // kernel keeps a list of items on the recover transaction; closing one item
  // must not drop the ones before it).
  SyntheticLog log;
  auto inode = InodeItemRegions(128);
  auto buf = BufItemRegions(4096, 1, lf::kBlftAgf, 0,
                            std::vector<uint8_t>(lf::kBufChunkSize, 0));
  std::vector<uint8_t> icreate(lf::kIcreateLogSize, 0);
  PutLE16At(icreate.data(), 0, lf::kItemIcreate);
  PutLE16At(icreate.data(), 2, 1);
  std::vector<std::vector<uint8_t>> regions;
  regions.push_back(inode[0]);
  regions.push_back(inode[1]);
  regions.push_back(buf[0]);
  regions.push_back(buf[1]);
  regions.push_back(icreate);
  auto ops = CommitOps(5, std::move(regions));
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 5, ops);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK_EQ(scan.value().transactions.size(), 1u);
  if (scan.value().transactions.empty()) return;
  const auto& items = scan.value().transactions[0].items;
  CHECK_EQ(items.size(), 3u);
  if (items.size() != 3) return;
  // xlog_recover_reorder_trans: buffers (and icreate) first, then other items.
  CHECK_EQ(items[0].type, lf::kItemBuf);
  CHECK_EQ(items[0].regions.size(), 2u);
  CHECK_EQ(items[1].type, lf::kItemIcreate);
  CHECK_EQ(items[1].regions.size(), 1u);
  CHECK_EQ(items[2].type, lf::kItemInode);
  CHECK_EQ(items[2].regions.size(), 2u);
}

// Builds a record larger than 32 KiB by hand (the writer refuses to produce
// one): two header blocks, the second an xlog_rec_ext_header carrying the
// cycle data of data blocks 64.., then the data blocks. Returns the record
// image and the block/op counts.
std::vector<uint8_t> BuildExtendedRecord(uint32_t cycle, uint32_t block,
                                         uint32_t prev, uint64_t tail_lsn,
                                         uint32_t tid,
                                         const std::vector<LogOp>& ops,
                                         uint32_t* data_blocks,
                                         uint32_t* num_ops) {
  std::vector<uint8_t> data;
  uint32_t count = 0;
  for (const LogOp& op : ops) {
    xfs::AppendLogOp(&data, tid, op);
    ++count;
  }
  CHECK(data.size() > lf::kHeaderCycleBytes);
  const size_t rounded =
      (data.size() + lf::kCycleBlockSize - 1) & ~(lf::kCycleBlockSize - 1);
  data.resize(rounded, 0);
  const size_t blocks = rounded / lf::kCycleBlockSize;
  CHECK(blocks > lf::kRecCycleDataCount);

  std::vector<uint32_t> saved(blocks, 0);
  for (size_t i = 0; i < blocks; ++i) {
    uint8_t* b = data.data() + i * lf::kCycleBlockSize;
    saved[i] = xfs::GetBE32(b);
    xfs::StoreBE32(b, cycle);
  }

  constexpr uint32_t kHSize = 65536;  // > 32 KiB: one extension header
  std::vector<uint8_t> record(2 * lf::kRecordHeaderSize + rounded, 0);
  uint8_t* h = record.data();
  xfs::PutBE32(h, lf::kRecMagic, lf::kRecordMagic);
  xfs::PutBE32(h, lf::kRecCycle, cycle);
  xfs::PutBE32(h, lf::kRecVersion, lf::kVersion2);
  xfs::PutBE32(h, lf::kRecLen, static_cast<uint32_t>(rounded));
  xfs::PutBE64(h, lf::kRecLsn, xfs::MakeLsn(cycle, block));
  xfs::PutBE64(h, lf::kRecTailLsn, tail_lsn);
  xfs::PutBE32(h, lf::kRecPrevBlock, prev);
  xfs::PutBE32(h, lf::kRecNumLogOps, count);
  for (size_t i = 0; i < lf::kRecCycleDataCount; ++i) {
    xfs::PutBE32(h, lf::kRecCycleData + i * 4, saved[i]);
  }
  xfs::PutBE32(h, lf::kRecFmt, lf::kFmtLinuxLe);
  std::memcpy(h + lf::kRecUuid, kUuid, 16);
  xfs::PutBE32(h, lf::kRecSize, kHSize);
  uint8_t* ext = h + lf::kRecordHeaderSize;
  xfs::PutBE32(ext, 0, cycle);  // xh_cycle
  for (size_t i = lf::kRecCycleDataCount; i < blocks; ++i) {
    xfs::PutBE32(ext, 4 + (i - lf::kRecCycleDataCount) * 4, saved[i]);
  }
  std::memcpy(record.data() + 2 * lf::kRecordHeaderSize, data.data(),
              data.size());
  std::vector<uint8_t> header(record.begin(), record.begin() + lf::kRecCrcCovered);
  xfs::PutLE32(header.data(), lf::kRecCrc, 0);
  uint32_t crc = xfs::Crc32cUpdate(0xFFFFFFFFu, header.data(), header.size());
  crc = xfs::Crc32cUpdate(crc, record.data() + 2 * lf::kRecordHeaderSize,
                          data.size());
  xfs::PutLE32(record.data(), lf::kRecCrc, ~crc);
  if (data_blocks != nullptr) *data_blocks = static_cast<uint32_t>(blocks);
  if (num_ops != nullptr) *num_ops = count;
  return record;
}

TEST(recovery_scan_large_record_with_extension_header) {
  // A record above 32 KiB needs extension header blocks for the cycle data of
  // data blocks 64.. (xlog_rec_ext_header). The writer cannot emit one, so the
  // scanner is exercised independently of the writer's 32 KiB limit. The
  // synthetic ring has to be larger than the record (2 + 65 blocks).
  SyntheticLog log(96);
  std::vector<uint8_t> payload(32 * 1024, 0);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
  }
  auto regions = BufItemRegions(8192, 64, lf::kBlftAgf, 0, payload);
  auto ops = CommitOps(9, regions);
  uint32_t data_blocks = 0;
  uint32_t num_ops = 0;
  auto record = BuildExtendedRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 9,
                                    ops, &data_blocks, &num_ops);
  CHECK(data_blocks > 64u);
  log.WriteRaw(log.log_byte(), record.data(), record.size());

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().state == xfs::LogState::kDirty);
  CHECK_EQ(scan.value().records.size(), 1u);
  if (!scan.value().records.empty()) {
    CHECK_EQ(scan.value().records[0].header_blocks, 2u);
    CHECK_EQ(scan.value().records[0].data_blocks, data_blocks);
    CHECK_EQ(scan.value().records[0].num_ops, num_ops);
  }
  CHECK_EQ(scan.value().transactions.size(), 1u);
  if (scan.value().transactions.empty()) return;
  const auto& items = scan.value().transactions[0].items;
  CHECK_EQ(items.size(), 1u);
  if (items.empty()) return;
  CHECK_EQ(items[0].type, lf::kItemBuf);
  CHECK_EQ(items[0].regions.size(), 2u);
  if (items[0].regions.size() == 2) {
    // Every data block's cycle stamp was restored from the header or the
    // extension header: the logged region must be byte-identical.
    CHECK(items[0].regions[1] == payload);
  }
}

TEST(recovery_scan_split_continuation_across_records) {
  SyntheticLog log;
  // Record A: START + transaction header + the first 8 bytes of the item
  // format header, flagged CONTINUE. Record B: the rest with WAS_CONT, then
  // the payload region and the COMMIT op.
  auto regions = InodeItemRegions(256);
  std::vector<LogOp> first;
  first.push_back(LogOp(lf::kFlagStartTrans, {}));
  std::vector<uint8_t> trans_header;
  xfs::AppendTransHeader(&trans_header, 42, regions.size());
  first.push_back(LogOp(0, std::move(trans_header)));
  std::vector<uint8_t> part1(regions[0].begin(), regions[0].begin() + 8);
  first.push_back(LogOp(lf::kFlagContinueTrans, std::move(part1)));
  CHECK_EQ(log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 42, first), 2u);

  std::vector<LogOp> second;
  std::vector<uint8_t> part2(regions[0].begin() + 8, regions[0].end());
  second.push_back(LogOp(lf::kFlagWasContTrans, std::move(part2)));
  second.push_back(LogOp(0, regions[1]));
  second.push_back(LogOp(lf::kFlagCommitTrans, {}));
  CHECK_EQ(log.WriteRecord(1, 2, 0, xfs::MakeLsn(1, 0), 42, second), 2u);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().state == xfs::LogState::kDirty);
  CHECK_EQ(scan.value().transactions.size(), 1u);
  if (scan.value().transactions.empty()) return;
  CHECK_EQ(scan.value().transactions[0].items.size(), 1u);
  if (!scan.value().transactions[0].items.empty()) {
    const xfs::RecoveredItem& item = scan.value().transactions[0].items[0];
    CHECK_EQ(item.regions.size(), 2u);
    CHECK_EQ(item.type, lf::kItemInode);
    if (item.regions.size() == 2) {
      CHECK_EQ(item.regions[0].size(), lf::kInodeLogFormatSize);
      CHECK_EQ(item.regions[1].size(), lf::kLogDinodeSize);
    }
  }
}

TEST(recovery_scan_real_fixture_dirty_transaction) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  const uint64_t ino = fixture.info().file_ino;
  const xfs::XfsTimestamp stamp{1700000000, 0};
  auto updated = fs.value()->SetFileTimes(ino, stamp, stamp, stamp);
  CHECK(updated.ok());
  if (!updated.ok()) {
    fixture.Reset();
    return;
  }
  // Read the state with the filesystem still mounted (its writer holds the
  // position); scanning the same bytes with a second device view must agree.
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) {
    fixture.Reset();
    return;
  }
  auto scan = xfs::ScanLog(*memory, fs.value()->superblock(),
                           fs.value()->geometry());
  CHECK(scan.ok());
  if (scan.ok()) {
    CHECK(scan.value().state == xfs::LogState::kDirty);
    CHECK(scan.value().Recoverable());
    CHECK_EQ(scan.value().transactions.size(), 1u);
    if (!scan.value().transactions.empty()) {
      CHECK_EQ(scan.value().transactions[0].lsn, scan.value().newest_lsn);
      CHECK_EQ(scan.value().transactions[0].items.size(), 1u);
      if (!scan.value().transactions[0].items.empty()) {
        CHECK_EQ(scan.value().transactions[0].items[0].type, lf::kItemInode);
      }
    }
  }
  // Drop the filesystem without writing an unmount record: the fixture keeps
  // the dirty log for the next test.
  fs.value().reset();
}

// ---------------------------------------------------------------------------
// Replay (pass1/pass2)
// ---------------------------------------------------------------------------

namespace {

constexpr uint64_t kAgfDaddr = 4096;  // byte 2 MiB inside the test device

// A buffer log item for a cancellation (the bitmap logs one chunk).
std::vector<std::vector<uint8_t>> CancelItemRegions(uint64_t daddr,
                                                    uint32_t len_blocks,
                                                    uint16_t blft) {
  std::vector<uint8_t> chunk(lf::kBufChunkSize, 0);
  return BufItemRegions(daddr, len_blocks, blft, lf::kBlfCancel, chunk);
}

}  // namespace

TEST(recovery_replay_buffer_item_stamps_lsn_and_crc) {
  SyntheticLog log;
  std::vector<uint8_t> on_disk = MakeAgf(7);
  log.WriteRaw(kAgfDaddr * 512, on_disk.data(), on_disk.size());

  std::vector<uint8_t> updated = MakeAgf(8);
  auto ops = CommitOps(11, BufItemRegions(kAgfDaddr, 1, lf::kBlftAgf, 0, updated));
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 11, ops);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().Recoverable());
  xfs::RecoveredLogState state;
  auto replay = log.Replay(scan.value(), &state);
  CHECK(replay.ok());
  if (!replay.ok()) return;
  CHECK_EQ(replay.value().buffers_replayed, 1u);
  CHECK_EQ(replay.value().transactions_replayed, 1u);

  std::vector<uint8_t> after(512);
  CHECK(log.ReadRaw(kAgfDaddr * 512, after.data(), after.size()));
  CHECK_EQ(xfs::GetBE32(after.data(), 8), 8u);  // the logged change
  CHECK_EQ(xfs::GetBE64(after.data(), xfs::ondisk::agf::kLsn),
           xfs::MakeLsn(1, 0));
  CHECK(xfs::VerifyXfsChecksum(after.data(), after.size(),
                               xfs::ondisk::agf::kCrc));
  // The writer continues after the replayed record, with the replayed window
  // pinned as the tail (intent completion still needs it).
  CHECK_EQ(state.head_block, scan.value().head_block);
  CHECK_EQ(state.head_cycle, 1u);
  CHECK_EQ(state.tail_lsn, xfs::MakeLsn(1, 0));
}

TEST(recovery_replay_skips_newer_metadata) {
  SyntheticLog log;
  std::vector<uint8_t> on_disk = MakeAgf(7);
  xfs::PutBE64(on_disk.data(), xfs::ondisk::agf::kLsn, xfs::MakeLsn(2, 0));
  xfs::UpdateXfsChecksum(on_disk.data(), on_disk.size(), xfs::ondisk::agf::kCrc);
  log.WriteRaw(kAgfDaddr * 512, on_disk.data(), on_disk.size());

  std::vector<uint8_t> older = MakeAgf(9);
  auto ops = CommitOps(12, BufItemRegions(kAgfDaddr, 1, lf::kBlftAgf, 0, older));
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 12, ops);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(replay.ok());
  if (!replay.ok()) return;
  CHECK_EQ(replay.value().buffers_replayed, 0u);
  CHECK_EQ(replay.value().buffers_skipped_newer, 1u);

  std::vector<uint8_t> after(512);
  CHECK(log.ReadRaw(kAgfDaddr * 512, after.data(), after.size()));
  CHECK_EQ(xfs::GetBE32(after.data(), 8), 7u);  // still the newer on-disk value
  CHECK_EQ(xfs::GetBE64(after.data(), xfs::ondisk::agf::kLsn),
           xfs::MakeLsn(2, 0));
}

TEST(recovery_replay_cancel_table) {
  SyntheticLog log;
  std::vector<uint8_t> on_disk = MakeAgf(7);
  log.WriteRaw(kAgfDaddr * 512, on_disk.data(), on_disk.size());

  // Record 1: a logged update that a later cancellation invalidates.
  auto first = CommitOps(21, BufItemRegions(kAgfDaddr, 1, lf::kBlftAgf, 0,
                                            MakeAgf(9)));
  const uint32_t block1 = 0;
  const uint32_t block2 =
      block1 + log.WriteRecord(1, block1, 0xffffffffu, xfs::MakeLsn(1, 0), 21,
                               first);
  // Record 2: the block is freed (cancelled).
  auto cancel = CommitOps(22, CancelItemRegions(kAgfDaddr, 1, lf::kBlftAgf));
  const uint32_t block3 =
      block2 + log.WriteRecord(1, block2, block1, xfs::MakeLsn(1, 0), 22,
                               cancel);
  // Record 3: the block is reused and logged again; this one must replay.
  auto reuse = CommitOps(23, BufItemRegions(kAgfDaddr, 1, lf::kBlftAgf, 0,
                                            MakeAgf(10)));
  log.WriteRecord(1, block3, block2, xfs::MakeLsn(1, 0), 23, reuse);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK_EQ(scan.value().transactions.size(), 3u);
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(replay.ok());
  if (!replay.ok()) return;
  CHECK_EQ(replay.value().buffers_replayed, 1u);  // only the post-cancel one

  std::vector<uint8_t> after(512);
  CHECK(log.ReadRaw(kAgfDaddr * 512, after.data(), after.size()));
  CHECK_EQ(xfs::GetBE32(after.data(), 8), 10u);
}

TEST(recovery_replay_refuses_unsupported_items) {
  SyntheticLog log;
  std::vector<uint8_t> rui(lf::kIntentLogFormatSize, 0);
  PutLE16At(rui.data(), 0, lf::kItemRui);
  PutLE16At(rui.data(), 2, 1);
  auto ops = CommitOps(31, {rui});
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 31, ops);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(!replay.ok());
  if (!replay.ok()) {
    CHECK(replay.error().message.find("XFS_LI_RUI") != std::string::npos);
  }
}

TEST(recovery_replay_collects_pending_intents) {
  SyntheticLog log;
  std::vector<uint8_t> efi(lf::kIntentLogFormatSize + lf::kExtentSize, 0);
  PutLE16At(efi.data(), 0, lf::kItemEfi);
  PutLE16At(efi.data(), 2, 1);
  PutLE32At(efi.data(), lf::kIntentNextentsOffset, 1);
  PutLE64At(efi.data(), lf::kIntentIdOffset, 0x1234);
  auto first = CommitOps(41, {efi});
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 41, first);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().Recoverable());
  auto pending = xfs::CollectPendingIntents(scan.value());
  CHECK_EQ(pending.size(), 1u);
  if (!pending.empty()) CHECK_EQ(pending[0].type, lf::kItemEfi);

  // A done item cancels the matching intent.
  std::vector<uint8_t> efd(lf::kIntentLogFormatSize + lf::kExtentSize, 0);
  PutLE16At(efd.data(), 0, lf::kItemEfd);
  PutLE16At(efd.data(), 2, 1);
  PutLE32At(efd.data(), lf::kIntentNextentsOffset, 1);
  PutLE64At(efd.data(), lf::kIntentIdOffset, 0x1234);
  auto second = CommitOps(42, {efd});
  log.WriteRecord(1, 2, 0, xfs::MakeLsn(1, 0), 42, second);

  auto scan2 = log.Scan();
  CHECK(scan2.ok());
  if (!scan2.ok()) return;
  CHECK_EQ(xfs::CollectPendingIntents(scan2.value()).size(), 0u);
  auto replay = log.Replay(scan2.value(), nullptr);
  CHECK(replay.ok());
  if (replay.ok()) CHECK_EQ(replay.value().intents_pending, 0u);
}

TEST(recovery_replay_real_transaction_is_idempotent) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  const uint64_t ino = fixture.info().file_ino;
  const xfs::XfsTimestamp stamp{1700000123, 456789};
  auto updated = fs.value()->SetFileTimes(ino, stamp, stamp, stamp);
  CHECK(updated.ok());
  if (!updated.ok()) {
    fixture.Reset();
    return;
  }
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) {
    fixture.Reset();
    return;
  }
  const xfs::XfsGeometry geo = fs.value()->geometry();
  const xfs::XfsSuperblock sb = fs.value()->superblock();

  auto scan = xfs::ScanLog(*memory, sb, geo);
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().Recoverable());
  xfs::RecoveredLogState state;
  auto replay = xfs::ReplayLog(*memory, sb, geo, scan.value(), &state);
  if (!replay.ok()) {
    std::fprintf(stderr, "replay failed: %s\n", replay.error().Format().c_str());
  }
  CHECK(replay.ok());
  if (!replay.ok()) return;
  CHECK_EQ(replay.value().inodes_replayed, 1u);

  std::vector<uint8_t> inode_after(geo.inode_size);
  CHECK(memory->Read(geo.InoToByteOffset(ino), inode_after.data(),
                     inode_after.size()));
  // The replayed inode carries the transaction's LSN.
  CHECK_EQ(xfs::GetBE64(inode_after.data(), xfs::ondisk::inode::kLsn),
           scan.value().newest_lsn);
  CHECK(xfs::VerifyXfsChecksum(inode_after.data(), inode_after.size(),
                               xfs::ondisk::inode::kCrc));

  // A second recovery of the same image is a no-op: same bytes, same state.
  auto scan2 = xfs::ScanLog(*memory, sb, geo);
  CHECK(scan2.ok());
  if (!scan2.ok()) return;
  auto replay2 = xfs::ReplayLog(*memory, sb, geo, scan2.value(), nullptr);
  CHECK(replay2.ok());
  std::vector<uint8_t> inode_twice(geo.inode_size);
  CHECK(memory->Read(geo.InoToByteOffset(ino), inode_twice.data(),
                     inode_twice.size()));
  CHECK(inode_after == inode_twice);

  fs.value().reset();
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Filesystem-level recovery (mount, intents, unlinked inodes, clean log)
// ---------------------------------------------------------------------------

namespace {

// Leaves the fixture image with a dirty log: one committed transaction that
// updated the fixture file's timestamps, then the filesystem is dropped
// without writing the unmount record.
void MakeWriterDirtyLog(xfstest::FixtureImage& fixture,
                        const xfs::XfsTimestamp& stamp) {
  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  auto updated = fs.value()->SetFileTimes(fixture.info().file_ino, stamp, stamp,
                                          stamp);
  CHECK(updated.ok());
}

}  // namespace

TEST(recovery_mount_replays_writer_dirty_log) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const xfs::XfsTimestamp stamp{1700000555, 123};
  MakeWriterDirtyLog(fixture, stamp);

  // A plain read-only mount must refuse (no writes on a read-only mount).
  auto ro = fixture.Mount(/*read_write=*/false);
  CHECK(!ro.ok());
  if (!ro.ok()) CHECK_EQ(ro.error().code, xfs::XfsError::kDirtyLog);

  // A read-write mount replays the journal as part of mounting.
  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  CHECK(fs.value()->recovery_report().has_value());
  if (fs.value()->recovery_report().has_value()) {
    const auto& report = *fs.value()->recovery_report();
    CHECK(report.recovered);
    CHECK_EQ(report.replay.transactions_replayed, 1u);
    CHECK_EQ(report.replay.inodes_replayed, 1u);
    CHECK_EQ(report.replay.buffers_skipped_newer, 0u);
  }
  auto info = fs.value()->GetFileInfo(fixture.info().file_ino);
  CHECK(info.ok());
  if (info.ok()) CHECK_EQ(info.value().mtime.sec, stamp.sec);

  // Unmount cleanly: the log ends with an unmount record for everyone.
  CHECK(fs.value()->WriteCleanUnmountRecord().ok());
  fs.value().reset();

  auto clean = fixture.Mount(/*read_write=*/false);
  CHECK(clean.ok());
  if (clean.ok()) {
    auto check = xfs::CheckLogClean(clean.value()->device(),
                                    clean.value()->geometry(),
                                    clean.value()->superblock());
    CHECK(check.ok());
    if (check.ok()) CHECK(check.value().state == xfs::LogState::kClean);
  }
  fixture.Reset();
}

TEST(recovery_recover_command_cleans_the_log) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const xfs::XfsTimestamp stamp{1700000777, 0};
  MakeWriterDirtyLog(fixture, stamp);

  // The standalone command shape: open the image without touching the log,
  // then run recovery explicitly and finish with a clean unmount record.
  xfs::MountOptions options;
  options.check_log = false;
  auto borrowed =
      std::make_unique<xfstest::BorrowedBlockDevice>(&fixture.storage());
  auto fs = xfs::XfsFilesystem::Mount(std::move(borrowed), options);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  auto report =
      fs.value()->RecoverJournal(xfs::XfsFilesystem::RecoveryFinish::kCleanLog);
  CHECK(report.ok());
  if (!report.ok()) {
    fixture.Reset();
    return;
  }
  CHECK(report.value().recovered);
  CHECK_EQ(report.value().replay.transactions_replayed, 1u);
  fs.value().reset();

  // The log is clean now: even a strict read-only mount is happy.
  auto clean = fixture.Mount(/*read_write=*/false);
  CHECK(clean.ok());
  if (clean.ok()) {
    auto check = xfs::CheckLogClean(clean.value()->device(),
                                    clean.value()->geometry(),
                                    clean.value()->superblock());
    CHECK(check.ok());
    if (check.ok()) CHECK(check.value().state == xfs::LogState::kClean);
    auto info = clean.value()->GetFileInfo(fixture.info().file_ino);
    CHECK(info.ok());
    if (info.ok()) CHECK_EQ(info.value().mtime.sec, stamp.sec);
  }
  fixture.Reset();
}

TEST(recovery_of_a_clean_log_is_a_noop) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  xfs::MountOptions options;
  options.check_log = false;
  auto borrowed =
      std::make_unique<xfstest::BorrowedBlockDevice>(&fixture.storage());
  auto fs = xfs::XfsFilesystem::Mount(std::move(borrowed), options);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  auto report =
      fs.value()->RecoverJournal(xfs::XfsFilesystem::RecoveryFinish::kMount);
  CHECK(report.ok());
  if (report.ok()) {
    CHECK(!report.value().recovered);
    CHECK_EQ(report.value().replay.transactions_replayed, 0u);
  }
  fs.value().reset();
  fixture.Reset();
}

TEST(recovery_mount_finishes_extent_free_intents) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) return;

  // Allocate a run of blocks so an EFI can describe a genuinely allocated
  // extent, then unmount cleanly.
  xfs::Allocation alloc;
  uint64_t freeblks_before = 0;
  uint64_t btreeblks_before = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) {
      fixture.Reset();
      return;
    }
    auto agf = xfs::ReadAgf(*memory, fs.value()->geometry(), 0);
    CHECK(agf.ok());
    if (agf.ok()) {
      freeblks_before = agf.value().freeblks;
      btreeblks_before = agf.value().btreeblks;
    }
    xfs::Status status = fs.value()->RunTransaction(
        [&](xfs::XfsTransaction& txn) -> xfs::Status {
          xfs::BlockAllocator blocks(&txn, &fs.value()->device(),
                                     &fs.value()->geometry());
          xfs::Status s = blocks.Allocate(0, 4, &alloc);
          if (!s.ok()) return s;
          return blocks.Commit();
        });
    CHECK(status.ok());
    if (!status.ok()) {
      fixture.Reset();
      return;
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }

  // Append a hand-built EFI record (extent free intent) at the next log write
  // position: this is what a Linux truncate/unlink leaves behind after a
  // crash.
  {
    const xfs::XfsGeometry& geo = xfs::MakeGeometry(
        xfs::ReadSuperblock(*memory, 0, true).value());
    auto sb = xfs::ReadSuperblock(*memory, 0, true);
    CHECK(sb.ok());
    if (!sb.ok()) return;
    auto position = xfs::ReadLogPosition(*memory, geo, sb.value());
    CHECK(position.ok());
    if (!position.ok()) return;
    std::vector<uint8_t> efi(lf::kIntentLogFormatSize + lf::kExtentSize, 0);
    PutLE16At(efi.data(), 0, lf::kItemEfi);
    PutLE16At(efi.data(), 2, 1);
    PutLE32At(efi.data(), lf::kIntentNextentsOffset, 1);
    PutLE64At(efi.data(), lf::kIntentIdOffset, 0xeF1u);
    PutLE64At(efi.data() + lf::kIntentLogFormatSize,
              lf::kExtentStartOffset, alloc.fsb);
    PutLE32At(efi.data() + lf::kIntentLogFormatSize, lf::kExtentLenOffset,
              alloc.len);
    xfs::LogRecordParams params;
    params.cycle = position.value().cycle;
    params.block = position.value().next_block;
    params.tid = 0x77;
    params.prev_block = position.value().prev_block;
    params.tail_lsn = xfs::MakeLsn(position.value().cycle,
                                   position.value().next_block);
    params.log_blocks = position.value().log_blocks;
    params.record_size = position.value().record_size;
    params.uuid = geo.meta_uuid;
    auto record = xfs::BuildLogRecord(
        params, CommitOps(0x77, {efi}));
    CHECK(record.ok());
    if (!record.ok()) return;
    CHECK(memory->Write(geo.FsbToByte(sb.value().logstart) +
                            static_cast<uint64_t>(params.block) * 512,
                        record.value().data(), record.value().size()));
    CHECK(memory->Flush());
  }

  // Mount read-write: the log is replayed, the EFI is found, the extents are
  // freed in a transaction that also logs the EFD.
  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  CHECK(fs.value()->recovery_report().has_value());
  if (fs.value()->recovery_report().has_value()) {
    const auto& report = *fs.value()->recovery_report();
    CHECK_EQ(report.replay.intents_pending, 1u);
    CHECK_EQ(report.intents_completed, 1u);
  }
  auto agf = xfs::ReadAgf(*memory, fs.value()->geometry(), 0);
  CHECK(agf.ok());
  if (agf.ok()) {
    CHECK_EQ(agf.value().freeblks, freeblks_before);
    CHECK_EQ(agf.value().btreeblks, btreeblks_before);
  }
  // The completion transaction logged the matching EFD: a second scan finds
  // no pending intent.
  auto scan = xfs::ScanLog(*memory, fs.value()->superblock(),
                           fs.value()->geometry());
  CHECK(scan.ok());
  if (scan.ok()) {
    CHECK(scan.value().Recoverable());
    CHECK_EQ(xfs::CollectPendingIntents(scan.value()).size(), 0u);
  }
  fs.value().reset();

  // Crash right after the completion (no unmount record): the next recovery
  // re-replays the window, the EFD releases the intent, and the extents are
  // not freed twice.
  auto again = fixture.Mount(/*read_write=*/true);
  CHECK(again.ok());
  if (again.ok()) {
    if (again.value()->recovery_report().has_value()) {
      CHECK_EQ(again.value()->recovery_report()->intents_completed, 0u);
    }
    auto agf2 = xfs::ReadAgf(*memory, again.value()->geometry(), 0);
    CHECK(agf2.ok());
    if (agf2.ok()) {
      CHECK_EQ(agf2.value().freeblks, freeblks_before);
      CHECK_EQ(agf2.value().btreeblks, btreeblks_before);
    }
  }
  fixture.Reset();
}

// A device that accepts writes up to a limit and then fails every further
// write/flush, emulating a power loss in the middle of recovery: everything
// before the cut is durable, nothing after it happens.
class FailAfterWrites final : public xfs::BlockDevice {
 public:
  explicit FailAfterWrites(xfs::BlockDevice* backing) : backing_(backing) {}
  void FailAfter(uint64_t writes) { fail_after_ = writes; }
  void Disable() { fail_after_ = kNoLimit; }
  uint64_t writes() const { return writes_; }

  bool Read(uint64_t offset, void* buffer, size_t size) override {
    return backing_->Read(offset, buffer, size);
  }
  bool Write(uint64_t offset, const void* buffer, size_t size) override {
    if (writes_ >= fail_after_) {
      last_error_ = "injected crash";
      return false;
    }
    ++writes_;
    return backing_->Write(offset, buffer, size);
  }
  bool Flush() override {
    if (writes_ >= fail_after_) {
      last_error_ = "injected crash";
      return false;
    }
    return backing_->Flush();
  }
  uint64_t Size() const override { return backing_->Size(); }
  uint32_t SectorSize() const override { return backing_->SectorSize(); }
  const xfs::DeviceInfo& Info() const override { return backing_->Info(); }
  void Close() override {}

 private:
  static constexpr uint64_t kNoLimit = ~0ull;
  xfs::BlockDevice* backing_;
  uint64_t fail_after_ = kNoLimit;
  uint64_t writes_ = 0;
};

// Recovery must be restartable: crash at any write, restart it, and the final
// image must equal a recovery that ran without a crash.
TEST(recovery_crash_and_rerun_converges) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) return;
  const uint64_t ino = fixture.info().file_ino;

  // A dirty log with several committed transactions.
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) {
      fixture.Reset();
      return;
    }
    for (int i = 0; i < 3; ++i) {
      const xfs::XfsTimestamp stamp{1700010000 + i, static_cast<uint32_t>(i)};
      CHECK(fs.value()->SetFileTimes(ino, stamp, stamp, stamp).ok());
    }
  }
  const std::vector<uint8_t> dirty = memory->bytes();

  xfs::MountOptions options;
  options.read_write = true;

  // Reference: a complete recovery.
  {
    auto fs = xfs::XfsFilesystem::Mount(
        std::make_unique<xfstest::BorrowedBlockDevice>(&fixture.storage()),
        options);
    CHECK(fs.ok());
    if (!fs.ok()) {
      fixture.Reset();
      return;
    }
  }
  const std::vector<uint8_t> expected = memory->bytes();
  uint64_t verified_cuts = 0;
  for (uint64_t cut = 0; cut < 16; ++cut) {
    memory->bytes() = dirty;
    bool crashed_early = false;
    {
      auto device = std::make_unique<FailAfterWrites>(&fixture.storage());
      device->FailAfter(cut);
      auto crashed = xfs::XfsFilesystem::Mount(std::move(device), options);
      crashed_early = !crashed.ok();
    }
    if (!crashed_early) break;  // the cut is past the last recovery write
    // Power is back: recovery runs again, without a write limit.
    auto retry = xfs::XfsFilesystem::Mount(
        std::make_unique<xfstest::BorrowedBlockDevice>(&fixture.storage()),
        options);
    if (!retry.ok()) {
      std::fprintf(stderr, "restart after cut %llu failed: %s\n",
                   static_cast<unsigned long long>(cut),
                   retry.error().Format().c_str());
      CHECK(retry.ok());
      fixture.Reset();
      return;
    }
    if (memory->bytes() != expected) {
      size_t diff = 0;
      while (diff < expected.size() && memory->bytes()[diff] == expected[diff]) {
        ++diff;
      }
      std::fprintf(stderr, "cut %llu: image diverges at byte 0x%zx\n",
                   static_cast<unsigned long long>(cut), diff);
      CHECK(memory->bytes() == expected);
      fixture.Reset();
      return;
    }
    ++verified_cuts;
  }
  CHECK(verified_cuts >= 3);  // several replay writes were cut and restarted
  fixture.Reset();
}

TEST(recovery_mount_frees_unlinked_inodes) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t ino = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) {
      fixture.Reset();
      return;
    }
    auto created = fs.value()->CreateRegularFile(
        fs.value()->RootInode(), "unlinked-recovery-test", 0644, 0, 0);
    CHECK(created.ok());
    if (!created.ok()) {
      fixture.Reset();
      return;
    }
    ino = created.value();
    // Defer the free: the inode becomes zero-link and goes on the AGI
    // unlinked list, exactly the open-but-unlinked crash state.
    CHECK(fs.value()->Unlink(fs.value()->RootInode(), "unlinked-recovery-test",
                             /*defer_inode_free=*/true)
              .ok());
  }
  const xfs::XfsGeometry geo = xfs::MakeGeometry(
      xfs::ReadSuperblock(fixture.storage(), 0, true).value());
  const uint32_t agno = geo.InoAgno(ino);
  const uint32_t bucket = geo.InoAgino(ino) % 64;
  {
    auto agi = xfs::ReadAgi(fixture.storage(), geo, agno);
    CHECK(agi.ok());
    if (agi.ok()) CHECK(agi.value().unlinked[bucket] != 0xffffffffu);
  }

  // Remount read-write: recovery replays the unlink transaction (a no-op for
  // the metadata, which is already on disk) and then processes the AGI
  // unlinked lists before the volume is usable.
  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  CHECK(fs.value()->recovery_report().has_value());
  if (fs.value()->recovery_report().has_value()) {
    CHECK(fs.value()->recovery_report()->unlinked_inodes_processed >= 1u);
  }
  auto agi = xfs::ReadAgi(fs.value()->device(), fs.value()->geometry(), agno);
  CHECK(agi.ok());
  if (agi.ok()) CHECK_EQ(agi.value().unlinked[bucket], 0xffffffffu);
  auto lookup = fs.value()->LookupName(fs.value()->RootInode(),
                                       "unlinked-recovery-test");
  CHECK(!lookup.ok());
  auto freed = fs.value()->OpenInode(ino);
  if (freed.ok()) CHECK_EQ(freed.value().nlink, 0u);
  fs.value().reset();
  fixture.Reset();
}

TEST(recovery_replays_a_freed_inode) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();

  // A writer-side unlink leaves the transaction that frees the inode in the
  // log: the core is zeroed (mode 0, xfs_difree), which is exactly what the
  // kernel's xfs_dinode_verify accepts for an inode that is not in use.
  // Recovery used to refuse it with "recovered inode did not verify".
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) {
      fixture.Reset();
      return;
    }
    auto removed = fs.value()->Unlink(fs.value()->RootInode(), "hello.bin");
    CHECK(removed.ok());
    // The filesystem object is dropped without writing an unmount record, so
    // the freeing transaction stays in the log.
  }

  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  CHECK(fs.value()->recovery_report().has_value());
  if (fs.value()->recovery_report().has_value()) {
    CHECK(fs.value()->recovery_report()->recovered);
    CHECK(fs.value()->recovery_report()->replay.inodes_replayed >= 1u);
  }
  // The namespace entry is gone and the freed inode stays unreadable: the
  // tolerance above is for replay, readers keep refusing a mode-0 inode.
  auto lookup = fs.value()->LookupName(fs.value()->RootInode(), "hello.bin");
  CHECK(!lookup.ok());
  auto gone = fs.value()->OpenInode(fixture.info().file_ino);
  CHECK(!gone.ok());
  CHECK(fs.value()->WriteCleanUnmountRecord().ok());
  fs.value().reset();
  fixture.Reset();
}
