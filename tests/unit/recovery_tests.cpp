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
#include <cstdio>
#include <cstring>
#include <memory>
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
#include "xfs/formatter.h"
#include "xfs/inode.h"
#include "xfs/log_checker.h"
#include "xfs/log_format.h"
#include "xfs/log_record.h"
#include "xfs/ondisk.h"
#include "xfs/recovery.h"
#include "xfs/rmap.h"
#include "xfs/superblock.h"
#include "xfs/transaction.h"

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

constexpr uint8_t kUuid[16] = {0x77, 0x66, 0x55, 0x44, 0x33, 0x22,
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
    sb_.versionnum = 0xb4b5;  // canonical mkfs.xfs v5 versionnum
    sb_.blocksize = 4096;
    sb_.sectsize = 512;
    sb_.inodesize = 512;
    sb_.logstart = kLogStartFsb;
    sb_.logblocks = (log_blocks * 512) / 4096;
    sb_.logsectsize = 512;
    sb_.logsunit = 0;

    geo_.block_size = 4096;
    geo_.block_log = 12;
    geo_.inode_size = 512;
    geo_.inopblog = 3;
    geo_.agblocks = 1024;
    geo_.agblklog = 10;
    geo_.agcount = 16;
    geo_.agno_log = 4;
    geo_.agino_log = 13;
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

TEST(recovery_scan_attri_and_attrd_are_supported) {
  SyntheticLog log;
  uint32_t next_tid = 1;
  // An ATTRI with format/name/value regions must be parsed and kept as a
  // pending attribute intent, not classified as unsupported.
  std::vector<uint8_t> attri(40, 0);
  PutLE16At(attri.data(), 0, lf::kItemAttri);
  PutLE16At(attri.data(), 2, 3);  // format + name + value
  PutLE64At(attri.data(), 8, 0xa11ce);
  PutLE64At(attri.data(), 16, 1);  // synthetic geometry inode range
  PutLE32At(attri.data(), 24, 1);    // SET
  PutLE32At(attri.data(), 28, 6);
  PutLE32At(attri.data(), 32, 3);
  PutLE32At(attri.data(), 36, 0);
  std::vector<uint8_t> name(8, 0);
  std::memcpy(name.data(), "user.x", 6);
  std::vector<uint8_t> value(4, 0);
  std::memcpy(value.data(), "abc", 3);
  auto first = CommitOps(next_tid, {attri, name, value});
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), next_tid, first);
  ++next_tid;

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().Recoverable());
  CHECK(scan.value().unsupported_items.empty());
  auto pending = xfs::CollectPendingIntents(scan.value());
  CHECK_EQ(pending.size(), 1u);
  if (!pending.empty()) {
    CHECK_EQ(pending[0].type, lf::kItemAttri);
  }

  // The matching ATTRD in a later transaction resolves it.
  std::vector<uint8_t> attrd(16, 0);
  PutLE16At(attrd.data(), 0, lf::kItemAttrd);
  PutLE16At(attrd.data(), 2, 1);
  PutLE64At(attrd.data(), 8, 0xa11ce);
  auto second = CommitOps(next_tid, {attrd});
  uint32_t used = log.WriteRecord(1, 2, 0, xfs::MakeLsn(1, 0), next_tid,
                                  second);
  ++next_tid;
  CHECK_EQ(used, 2u);
  auto scan2 = log.Scan();
  CHECK(scan2.ok());
  if (!scan2.ok()) return;
  CHECK(scan2.value().Recoverable());
  CHECK(xfs::CollectPendingIntents(scan2.value()).empty());
}

TEST(recovery_scan_unsupported_item_refuses) {
  SyntheticLog log;
  uint32_t next_tid = 1;
  FillCycle(&log, 1, xfs::MakeLsn(1, 0), &next_tid);
  FillCycle(&log, 2, xfs::MakeLsn(2, 2), &next_tid);

  // A committed DQUOT item (quota update) is an unsupported semantic in this
  // build.  ATTRI/ATTRD are supported by RW-6D.
  std::vector<uint8_t> dquot(lf::kIntentLogFormatSize, 0);
  PutLE16At(dquot.data(), 0, lf::kItemDquot);
  PutLE16At(dquot.data(), 2, 1);  // one region
  const uint32_t tid = next_tid;
  auto ops = CommitOps(tid, {dquot});
  log.WriteRecord(2, 62, 60, xfs::MakeLsn(2, 2), tid, ops);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(!scan.value().Recoverable());
  CHECK_EQ(scan.value().unsupported_items.size(), 1u);
  if (!scan.value().unsupported_items.empty()) {
    CHECK(scan.value().unsupported_items[0].find("XFS_LI_DQUOT") !=
          std::string::npos);
  }
}

TEST(recovery_scan_rud_without_intent_in_replay_window_is_legal) {
  SyntheticLog log;
  uint32_t next_tid = 1;
  FillCycle(&log, 1, xfs::MakeLsn(1, 0), &next_tid);
  std::vector<uint8_t> rud(lf::kRudFormatSize, 0);
  PutLE16At(rud.data(), 0, lf::kItemRud);
  PutLE16At(rud.data(), 2, 1);
  PutLE64At(rud.data(), lf::kIntentIdOffset, 0xabc);
  auto ops = CommitOps(next_tid, {rud});
  log.WriteRecord(1, 62, 60, xfs::MakeLsn(1, 2), next_tid, ops);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().Recoverable());
  CHECK(scan.value().unsupported_items.empty());
  CHECK(xfs::CollectPendingIntents(scan.value()).empty());
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
  // xlog_cksum() also covers each extension header's 260 bytes.
  crc = xfs::Crc32cUpdate(crc, ext, lf::kRecExtSize);
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

TEST(recovery_scan_writer_large_record_with_extension_header) {
  // Same shape as the synthetic scanner test, but produced by the production
  // log-record writer: the writer must choose h_size > 32 KiB, fill the
  // extension header, and the scanner must verify its CRC and unpack every
  // cycle word back into the 32 KiB logged buffer region.
  SyntheticLog log(96);
  std::vector<uint8_t> payload(32 * 1024, 0);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>((i * 17 + 3) & 0xFF);
  }
  auto regions = BufItemRegions(8192, 64, lf::kBlftAgf, 0, payload);
  auto ops = CommitOps(9, regions);
  xfs::LogRecordParams params;
  params.cycle = 1;
  params.block = 0;
  params.tid = 9;
  params.prev_block = 0xffffffffu;
  params.tail_lsn = xfs::MakeLsn(1, 0);
  params.log_blocks = log.log_blocks();
  params.uuid = log.sb().uuid;
  auto record = xfs::BuildLogRecord(params, ops);
  CHECK(record.ok());
  if (!record.ok()) return;
  CHECK(GetBE32(record.value().data(), lf::kRecSize) == 64 * 1024);
  CHECK(GetBE32(record.value().data(), lf::kRecLen) > lf::kHeaderCycleBytes);
  log.WriteRaw(log.log_byte(), record.value().data(), record.value().size());

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().state == xfs::LogState::kDirty);
  CHECK_EQ(scan.value().records.size(), 1u);
  if (!scan.value().records.empty()) {
    CHECK_EQ(scan.value().records[0].header_blocks, 2u);
  }
  CHECK_EQ(scan.value().transactions.size(), 1u);
  if (scan.value().transactions.empty()) return;
  const auto& items = scan.value().transactions[0].items;
  CHECK_EQ(items.size(), 1u);
  if (items.size() == 1 && items[0].regions.size() == 2) {
    CHECK(items[0].type == lf::kItemBuf);
    CHECK(items[0].regions[1] == payload);
  } else {
    CHECK(false);
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

bool ScanHasCancelItem(const xfs::LogScan& scan, uint64_t daddr,
                       uint32_t len_blocks) {
  for (const xfs::RecoveredTransaction& trans : scan.transactions) {
    for (const xfs::RecoveredItem& item : trans.items) {
      if (item.type != lf::kItemBuf || item.regions.empty() ||
          item.regions[0].size() < lf::kBufLogFormatSize) {
        continue;
      }
      const std::vector<uint8_t>& region = item.regions[0];
      if ((LoadLE16(region.data() + 4) & lf::kBlfCancel) == 0) continue;
      const uint32_t len = LoadLE16(region.data() + 6);
      const uint64_t blkno =
          static_cast<uint64_t>(xfs::GetLE32(region.data() + 8)) |
          (static_cast<uint64_t>(xfs::GetLE32(region.data() + 12)) << 32);
      if (blkno == daddr && len == len_blocks) return true;
    }
  }
  return false;
}

// Replaces the payload with an AGF-shaped buffer so recovery can classify a
// replayed buffer (xlog_recover_validate_buf_type). The buffer is always the
// size of the original remote symlink allocation.
std::vector<uint8_t> AgfBuffer(uint32_t seqno, size_t bytes) {
  std::vector<uint8_t> data = MakeAgf(seqno);
  data.resize(bytes, 0xA5);
  return data;
}

// A synthetic cancel item with the Linux stale-buffer encoding.
// `len_blocks` is in 512-byte daddr units.
std::vector<std::vector<uint8_t>> CancelItemRegions(uint64_t daddr,
                                                    uint32_t len_blocks) {
  const uint32_t chunks = len_blocks * (512 / lf::kBufChunkSize);
  const uint32_t map_words = (chunks + 31) / 32;
  std::vector<uint8_t> format(lf::kBufLogFormatSize + map_words * 4, 0);
  PutLE16At(format.data(), 0, lf::kItemBuf);
  PutLE16At(format.data(), 2, 1);
  PutLE16At(format.data(), 4, lf::kBlfCancel);
  PutLE16At(format.data(), 6, static_cast<uint16_t>(len_blocks));
  PutLE32At(format.data(), 8, static_cast<uint32_t>(daddr));
  PutLE32At(format.data(), 12, static_cast<uint32_t>(daddr >> 32));
  PutLE32At(format.data(), 16, map_words);
  return {format};
}

std::vector<std::vector<uint8_t>> CancelItemRegionsWithoutMap(
    uint64_t daddr, uint32_t len_blocks) {
  std::vector<uint8_t> format(lf::kBufLogFormatSize, 0);
  PutLE16At(format.data(), 0, lf::kItemBuf);
  PutLE16At(format.data(), 2, 1);
  PutLE16At(format.data(), 4, lf::kBlfCancel);
  PutLE16At(format.data(), 6, static_cast<uint16_t>(len_blocks));
  PutLE32At(format.data(), 8, static_cast<uint32_t>(daddr));
  PutLE32At(format.data(), 12, static_cast<uint32_t>(daddr >> 32));
  return {format};
}

std::vector<std::vector<uint8_t>> CancelItemRegionsWithNonzeroMap(
    uint64_t daddr, uint32_t len_blocks) {
  std::vector<std::vector<uint8_t>> regions =
      CancelItemRegions(daddr, len_blocks);
  regions[0][lf::kBufMapOffset] = 1;
  return regions;
}

struct FreedRemoteSymlink {
  uint64_t fsb = 0;
  uint32_t blocks = 0;
};

// Creates a remote symlink (target above the literal-area limit), records its
// data extent, then unlinks it without deferral. The data blocks are free and
// their cancel item is in the dirty log.
bool CreateAndFreeRemoteSymlink(xfs::XfsFilesystem& fs,
                                const std::string& name,
                                FreedRemoteSymlink* out) {
  std::string target(400, 'x');
  for (size_t i = 0; i < target.size(); ++i) {
    target[i] = static_cast<char>('a' + (i % 26));
  }
  auto ino = fs.CreateSymlink(fs.RootInode(), name, target, 0777, 0, 0);
  CHECK(ino.ok());
  if (!ino.ok()) return false;
  auto extents = fs.GetDataExtents(ino.value());
  CHECK(extents.ok());
  if (!extents.ok()) return false;
  CHECK_EQ(extents.value().size(), 1u);
  CHECK(!extents.value().empty());
  if (extents.value().size() != 1 || extents.value()[0].blockcount == 0) {
    return false;
  }
  out->fsb = extents.value()[0].startblock;
  out->blocks = static_cast<uint32_t>(extents.value()[0].blockcount);
  auto unlinked = fs.Unlink(fs.RootInode(), name);
  CHECK(unlinked.ok());
  return unlinked.ok();
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
  auto cancel = CommitOps(22, CancelItemRegions(kAgfDaddr, 1));
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
  CHECK_EQ(replay.value().cancels_seen, 1u);
  CHECK_EQ(replay.value().cancels_consumed, 1u);
  CHECK_EQ(replay.value().cancel_refs_unconsumed, 0u);
  CHECK_EQ(replay.value().buffers_cancelled, 1u);
  CHECK_EQ(replay.value().buffers_after_cancel, 1u);

  std::vector<uint8_t> after(512);
  CHECK(log.ReadRaw(kAgfDaddr * 512, after.data(), after.size()));
  CHECK_EQ(xfs::GetBE32(after.data(), 8), 10u);
}

TEST(recovery_replay_cancel_reuse_cancel_reuse) {
  SyntheticLog log;
  std::vector<uint8_t> on_disk = MakeAgf(7);
  log.WriteRaw(kAgfDaddr * 512, on_disk.data(), on_disk.size());

  // The full reuse chain: an old owner, the free that cancels it, a reuse
  // that the next free cancels again, and the final new owner.  Every cancel
  // reference is added in pass 1, so a cancel protects against every older
  // item even when that item's position precedes the cancel's own position;
  // only the write after the last cancellation may reach the device.
  auto first = CommitOps(32, BufItemRegions(kAgfDaddr, 1, lf::kBlftAgf, 0,
                                            MakeAgf(9)));
  uint32_t block = log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 32,
                                   first);
  auto cancel1 = CommitOps(33, CancelItemRegions(kAgfDaddr, 1));
  const uint32_t cancel1_block = block;
  block += log.WriteRecord(1, block, 0, xfs::MakeLsn(1, 0), 33, cancel1);
  auto reuse1 = CommitOps(34, BufItemRegions(kAgfDaddr, 1, lf::kBlftAgf, 0,
                                             MakeAgf(10)));
  const uint32_t reuse1_block = block;
  block += log.WriteRecord(1, block, cancel1_block, xfs::MakeLsn(1, 0), 34,
                           reuse1);
  auto cancel2 = CommitOps(35, CancelItemRegions(kAgfDaddr, 1));
  const uint32_t cancel2_block = block;
  block += log.WriteRecord(1, block, reuse1_block, xfs::MakeLsn(1, 0), 35,
                           cancel2);
  auto reuse2 = CommitOps(36, BufItemRegions(kAgfDaddr, 1, lf::kBlftAgf, 0,
                                             MakeAgf(11)));
  log.WriteRecord(1, block, cancel2_block, xfs::MakeLsn(1, 0), 36, reuse2);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK_EQ(scan.value().transactions.size(), 5u);
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(replay.ok());
  if (!replay.ok()) return;
  CHECK_EQ(replay.value().buffers_replayed, 1u);
  CHECK_EQ(replay.value().buffers_cancelled, 2u);
  CHECK_EQ(replay.value().buffers_after_cancel, 1u);
  CHECK_EQ(replay.value().cancels_seen, 2u);
  CHECK_EQ(replay.value().cancels_consumed, 2u);
  CHECK_EQ(replay.value().cancel_refs_unconsumed, 0u);

  std::vector<uint8_t> after(512);
  CHECK(log.ReadRaw(kAgfDaddr * 512, after.data(), after.size()));
  CHECK_EQ(xfs::GetBE32(after.data(), 8), 11u);
}

TEST(recovery_refuses_cancel_without_linux_map) {
  SyntheticLog log;
  std::vector<uint8_t> on_disk = MakeAgf(7);
  log.WriteRaw(kAgfDaddr * 512, on_disk.data(), on_disk.size());

  auto first = CommitOps(24, BufItemRegions(kAgfDaddr, 1, lf::kBlftAgf, 0,
                                            MakeAgf(9)));
  const uint32_t block1 = 0;
  const uint32_t block2 =
      block1 + log.WriteRecord(1, block1, 0xffffffffu, xfs::MakeLsn(1, 0), 24,
                               first);
  // Linux xfs_buf_item_format_segment() always writes blf_map_size words
  // (24 bytes for a one-block buffer).  A 20-byte format cannot come from the
  // Linux writer, so recovery must refuse it before replaying anything.
  auto cancel = CommitOps(25, CancelItemRegionsWithoutMap(kAgfDaddr, 1));
  log.WriteRecord(1, block2, block1, xfs::MakeLsn(1, 0), 25, cancel);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(!replay.ok());
  if (replay.ok()) return;
  CHECK(replay.error().Format().find("bitmap size") != std::string::npos);
  std::vector<uint8_t> after(512);
  CHECK(log.ReadRaw(kAgfDaddr * 512, after.data(), after.size()));
  CHECK_EQ(xfs::GetBE32(after.data(), 8), 7u);  // nothing replayed
}

TEST(recovery_refuses_cancel_with_nonzero_map) {
  SyntheticLog log;
  std::vector<uint8_t> on_disk = MakeAgf(7);
  log.WriteRaw(kAgfDaddr * 512, on_disk.data(), on_disk.size());
  // xfs_trans_binval() memsets every data-map word; a set bit is not a Linux
  // stale buffer and must not be interpreted as one.
  auto cancel = CommitOps(
      26, CancelItemRegionsWithNonzeroMap(kAgfDaddr, 1));
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 26, cancel);
  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(!replay.ok());
  if (replay.ok()) return;
  CHECK(replay.error().Format().find("non-zero data map") != std::string::npos);
}

TEST(recovery_refuses_cancel_with_buffer_type) {
  SyntheticLog log;
  // xfs_trans_binval() clears XFS_BLFT_MASK; a stale item that still names a
  // buffer type is not the encoding the Linux writer produces.
  auto regions = CancelItemRegions(kAgfDaddr, 1);
  PutLE16At(regions[0].data(), 4,
            static_cast<uint16_t>(lf::kBlfCancel |
                                  (lf::kBlftBtree << lf::kBlftShift)));
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 32,
                  CommitOps(32, std::move(regions)));
  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(!replay.ok());
  if (replay.ok()) return;
  CHECK(replay.error().Format().find("buffer type") != std::string::npos);
}

TEST(recovery_refuses_cancel_with_undefined_flags) {
  SyntheticLog log;
  // Bits 5..10 are not defined on disk.
  auto regions = CancelItemRegions(kAgfDaddr, 1);
  PutLE16At(regions[0].data(), 4,
            static_cast<uint16_t>(lf::kBlfCancel | (1u << 5)));
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 33,
                  CommitOps(33, std::move(regions)));
  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(!replay.ok());
  if (replay.ok()) return;
  CHECK(replay.error().Format().find("undefined buffer flags") !=
        std::string::npos);
}

TEST(recovery_refuses_cancel_with_inode_bit) {
  SyntheticLog log;
  // xfs_trans_binval() clears XFS_BLF_INODE_BUF.
  auto regions = CancelItemRegions(kAgfDaddr, 1);
  PutLE16At(regions[0].data(), 4,
            static_cast<uint16_t>(lf::kBlfCancel | lf::kBlfInodeBuf));
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 34,
                  CommitOps(34, std::move(regions)));
  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(!replay.ok());
  if (replay.ok()) return;
  CHECK(replay.error().Format().find("unexpected low buffer flags") !=
        std::string::npos);
}

TEST(recovery_refuses_cancel_with_payload_region) {
  SyntheticLog log;
  // A stale item has blf_size 1 and one BFORMAT region; a second region is
  // not a Linux stale encoding.
  auto regions = CancelItemRegions(kAgfDaddr, 1);
  PutLE16At(regions[0].data(), 2, 2);
  regions.push_back(std::vector<uint8_t>(128, 0xAA));
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 35,
                  CommitOps(35, std::move(regions)));
  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(!replay.ok());
  if (replay.ok()) return;
  CHECK(replay.error().Format().find("payload region") != std::string::npos);
}

TEST(recovery_refuses_cancel_outside_data_device) {
  SyntheticLog log;
  // The synthetic device is 4 MiB; this stale item claims a range far
  // outside it.  Nothing can match that geometry, and accepting it would
  // leave an unexplainable cancellation in the table.
  auto cancel = CommitOps(31, CancelItemRegions(1u << 20, 8));
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 31, cancel);
  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(!replay.ok());
  if (replay.ok()) return;
  CHECK(replay.error().Format().find("outside the data device") !=
        std::string::npos);
}

TEST(recovery_refuses_partial_cancel_overlap_in_transaction) {
  SyntheticLog log;
  std::vector<uint8_t> on_disk = MakeAgf(7);
  log.WriteRaw(kAgfDaddr * 512, on_disk.data(), on_disk.size());

  // One transaction cannot log a one-block buffer and cancel a two-block
  // buffer that overlaps it: the Linux buffer cache never owns two partially
  // overlapping buffers at once.  Recovery must refuse instead of applying
  // half of the exact-key cancellation.
  auto item = BufItemRegions(kAgfDaddr, 1, lf::kBlftAgf, 0, MakeAgf(9));
  auto cancel = CancelItemRegions(kAgfDaddr, 2);
  std::vector<std::vector<uint8_t>> regions = std::move(item);
  regions.insert(regions.end(), std::make_move_iterator(cancel.begin()),
                 std::make_move_iterator(cancel.end()));
  auto ops = CommitOps(27, std::move(regions));
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 27, ops);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(!replay.ok());
  if (replay.ok()) return;
  CHECK(replay.error().Format().find("partially overlaps") !=
        std::string::npos);
}

TEST(recovery_allows_cross_transaction_geometry_reuse) {
  SyntheticLog log;
  std::vector<uint8_t> on_disk = MakeAgf(7);
  log.WriteRaw(kAgfDaddr * 512, on_disk.data(), on_disk.size());

  // Linux cancels only the exact (blkno, len) key.  A later cancellation of
  // a different, overlapping geometry therefore does not suppress the older
  // item; the kernel replays it.  Windows must do the same instead of
  // rejecting a legal metadata-reuse sequence.
  auto first = CommitOps(28, BufItemRegions(kAgfDaddr, 1, lf::kBlftAgf, 0,
                                            MakeAgf(9)));
  const uint32_t block1 = 0;
  const uint32_t block2 =
      block1 + log.WriteRecord(1, block1, 0xffffffffu, xfs::MakeLsn(1, 0), 28,
                               first);
  auto cancel = CommitOps(29, CancelItemRegions(kAgfDaddr, 2));
  log.WriteRecord(1, block2, block1, xfs::MakeLsn(1, 0), 29, cancel);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(replay.ok());
  if (!replay.ok()) return;
  CHECK_EQ(replay.value().buffers_replayed, 1u);
  CHECK_EQ(replay.value().buffers_cancelled, 0u);
  CHECK_EQ(replay.value().buffers_after_cancel, 0u);
  CHECK_EQ(replay.value().cancels_consumed, 1u);
  std::vector<uint8_t> after(512);
  CHECK(log.ReadRaw(kAgfDaddr * 512, after.data(), after.size()));
  CHECK_EQ(xfs::GetBE32(after.data(), 8), 9u);
}

TEST(recovery_cancels_follow_buffers_in_reverse_log_order) {
  SyntheticLog log;
  const uint64_t buf_a = kAgfDaddr;
  const uint64_t buf_b = kAgfDaddr + 64;
  const uint64_t cancel_a = kAgfDaddr + 1;
  const uint64_t cancel_b = kAgfDaddr + 65;

  // xlog_recover_reorder_trans() moves buffer items to the front and every
  // cancel to the head of cancel_list with list_move(), then splices that
  // list onto the tail.  With two cancels logged interleaved with buffers,
  // recovery must see: buffers in log order, then cancels in reverse log
  // order.  The scanner is the only component that can establish this order
  // for the replay pass, so lock it here even though the kernel's cancel
  // refcounts are order-insensitive for distinct keys.
  std::vector<std::vector<uint8_t>> regions;
  auto add = [&regions](std::vector<std::vector<uint8_t>> item) {
    regions.insert(regions.end(), std::make_move_iterator(item.begin()),
                   std::make_move_iterator(item.end()));
  };
  add(CancelItemRegions(cancel_a, 1));
  add(BufItemRegions(buf_a, 1, lf::kBlftAgf, 0, MakeAgf(9)));
  add(CancelItemRegions(cancel_b, 1));
  add(BufItemRegions(buf_b, 1, lf::kBlftAgf, 0, MakeAgf(10)));
  auto ops = CommitOps(30, std::move(regions));
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 30, ops);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK_EQ(scan.value().transactions.size(), 1u);
  if (scan.value().transactions.size() != 1u) return;
  const auto& items = scan.value().transactions[0].items;
  CHECK_EQ(items.size(), 4u);
  if (items.size() != 4u) return;
  for (const auto& item : items) CHECK_EQ(item.type, lf::kItemBuf);
  CHECK_EQ(xfs::LoadLE32(items[0].regions[0].data() + 8), buf_a);
  CHECK_EQ(xfs::LoadLE32(items[1].regions[0].data() + 8), buf_b);
  CHECK_EQ(xfs::LoadLE32(items[2].regions[0].data() + 8), cancel_b);
  CHECK_EQ(xfs::LoadLE32(items[3].regions[0].data() + 8), cancel_a);
  for (size_t i = 2; i < 4; ++i) {
    CHECK((LoadLE16(items[i].regions[0].data() + 4) & lf::kBlfCancel) != 0);
  }
  for (size_t i = 0; i < 2; ++i) {
    CHECK((LoadLE16(items[i].regions[0].data() + 4) & lf::kBlfCancel) == 0);
  }
}

TEST(transaction_emits_linux_cancel_and_revives) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  const xfs::XfsGeometry& geo = fs.value()->geometry();
  const uint32_t last_ag = geo.agcount - 1;
  const uint64_t scratch_fsb =
      geo.AgFsb(last_ag, geo.AgBlocksFor(last_ag) - 1);
  CHECK(geo.FsbValid(scratch_fsb));
  if (!geo.FsbValid(scratch_fsb)) {
    fixture.Reset();
    return;
  }
  const uint64_t offset = geo.FsbToByte(scratch_fsb);
  const size_t bytes = geo.block_size;

  auto txn = fs.value()->BeginTransaction();
  CHECK(txn.ok());
  if (!txn.ok()) {
    fixture.Reset();
    return;
  }
  auto first = AgfBuffer(0x1111, bytes);
  CHECK(txn.value()
            .AddMetadataBuffer(offset, std::move(first), lf::kBlftAgf,
                               xfs::ondisk::agf::kLsn,
                               xfs::ondisk::agf::kCrc, "scratch")
            .ok());
  CHECK_EQ(txn.value().writes().size(), 1u);
  CHECK(txn.value()
            .InvalidateMetadataBuffer(offset, bytes, "scratch")
            .ok());

  // The cancel encoding is exactly xfs_trans_binval()/xfs_buf_item_format_
  // segment(): one BFORMAT region with no payload, blf_size 1, a zeroed map
  // sized for the whole buffer, and no inode/type flags.
  const auto& cancelled_ops = txn.value().ops();
  CHECK_EQ(cancelled_ops.size(), 1u);
  if (cancelled_ops.size() == 1) {
    const std::vector<uint8_t>& format = cancelled_ops[0].payload;
    const uint32_t chunks = static_cast<uint32_t>(bytes / lf::kBufChunkSize);
    const uint32_t map_words = (chunks + 31) / 32;
    CHECK_EQ(format.size(), lf::kBufLogFormatSize + map_words * 4);
    CHECK_EQ(LoadLE16(format.data() + 0), lf::kItemBuf);
    CHECK_EQ(LoadLE16(format.data() + 2), 1u);
    CHECK_EQ(LoadLE16(format.data() + 4), lf::kBlfCancel);
    CHECK_EQ(LoadLE16(format.data() + 6), bytes / 512);
    CHECK_EQ(xfs::GetLE32(format.data() + 8), offset / 512);
    CHECK_EQ(xfs::GetLE32(format.data() + 12), (offset / 512) >> 32);
    CHECK_EQ(xfs::GetLE32(format.data() + 16), map_words);
    for (uint32_t i = 0; i < map_words * 4; ++i) {
      CHECK_EQ(format[lf::kBufMapOffset + i], 0u);
    }
  }
  CHECK_EQ(txn.value().writes().size(), 1u);
  if (txn.value().writes().size() == 1) {
    CHECK(txn.value().writes()[0].suppressed);
  }

  // Re-logging the exact buffer supersedes the cancel: the stale item is
  // removed so recovery cannot skip the new item, and the write-back becomes
  // live again.
  auto second = AgfBuffer(0x2222, bytes);
  CHECK(txn.value()
            .AddMetadataBuffer(offset, std::move(second), lf::kBlftAgf,
                               xfs::ondisk::agf::kLsn,
                               xfs::ondisk::agf::kCrc, "scratch")
            .ok());
  CHECK_EQ(txn.value().writes().size(), 1u);
  if (txn.value().writes().size() == 1) {
    CHECK(!txn.value().writes()[0].suppressed);
  }
  for (const xfs::LogOp& op : txn.value().ops()) {
    CHECK(op.payload.size() < 6 ||
          (LoadLE16(op.payload.data() + 4) & lf::kBlfCancel) == 0);
  }

  fixture.Reset();
}

TEST(transaction_ordered_inode_buffers_require_icreate_intent) {
  xfs::XfsGeometry geo;
  geo.block_size = 4096;
  geo.block_log = 12;
  geo.inode_size = 512;
  geo.inopblock = 8;
  geo.agcount = 1;
  geo.agblocks = 128;
  geo.dblocks = 128;
  geo.agblklog = 7;
  constexpr uint32_t agbno = 8;
  constexpr uint32_t cluster_blocks = 2;
  const uint64_t offset = geo.FsbToByte(geo.AgFsb(0, agbno));
  const size_t cluster_bytes = cluster_blocks * geo.block_size;

  xfs::XfsTransaction txn;
  CHECK(!txn.AddOrderedBuffer(offset, std::vector<uint8_t>(cluster_bytes, 0x55),
                              64, "inode chunk")
             .ok());
  CHECK(txn.InvalidateMetadataBuffer(offset, cluster_bytes, "inode chunk")
            .ok());

  auto make_icreate = [&] {
    std::vector<uint8_t> payload(lf::kIcreateLogSize, 0);
    PutLE16At(payload.data(), lf::kIclType, lf::kItemIcreate);
    PutLE16At(payload.data(), lf::kIclSize, 1);
    PutBE32(payload.data(), lf::kIclAg, 0);
    PutBE32(payload.data(), lf::kIclAgbno, agbno);
    PutBE32(payload.data(), lf::kIclCount, 16);
    PutBE32(payload.data(), lf::kIclIsize, 512);
    PutBE32(payload.data(), lf::kIclLength, cluster_blocks);
    return payload;
  };
  CHECK(txn.AddIcreateWithOrderedBuffers(make_icreate(), geo, cluster_blocks)
            .ok());
  CHECK(!txn.AddOrderedBuffer(offset + 512,
                              std::vector<uint8_t>(cluster_bytes - 512, 0x66),
                              64, "partial inode chunk")
             .ok());
  CHECK(txn.AddOrderedBuffer(offset, std::vector<uint8_t>(cluster_bytes, 0x77),
                             64, "inode chunk")
            .ok());
  CHECK_EQ(txn.ops().size(), 1u);
  if (txn.ops().size() == 1) {
    CHECK_EQ(LoadLE16(txn.ops()[0].payload.data()), lf::kItemIcreate);
  }
  CHECK_EQ(txn.writes().size(), 1u);
  if (txn.writes().size() == 1) CHECK(!txn.writes()[0].suppressed);
}

TEST(transaction_cancel_suppresses_writes_inside_an_inode_cluster) {
  xfs::XfsTransaction txn;
  const uint64_t cluster_offset = 8192;
  const size_t cluster_bytes = 8192;

  // The ifree transaction queues the freed inode's 512-byte write before the
  // inode allocator discovers that the chunk is empty and invalidates the
  // whole cluster buffer.
  xfs::TransactionWrite inode;
  inode.offset = cluster_offset + 512;
  inode.data.assign(512, 0x44);
  inode.ino = 64;
  inode.inode_metadata = true;
  inode.what = "inode";
  CHECK(txn.AddWrite(std::move(inode)).ok());

  xfs::TransactionWrite cluster;
  cluster.offset = cluster_offset;
  cluster.data.assign(cluster_bytes, 0x55);
  cluster.what = "inode cluster";
  CHECK(txn.AddWrite(std::move(cluster)).ok());

  CHECK(txn.InvalidateMetadataBuffer(cluster_offset, cluster_bytes, "inode chunk")
            .ok());
  CHECK_EQ(txn.writes().size(), 2u);
  for (const xfs::TransactionWrite& write : txn.writes()) {
    CHECK(write.suppressed);  // neither stale buffer is dispatched
  }
  CHECK_EQ(txn.ops().size(), 1u);
  if (txn.ops().size() == 1) {
    const std::vector<uint8_t>& format = txn.ops()[0].payload;
    CHECK_EQ(LoadLE16(format.data() + 4), lf::kBlfCancel);
    CHECK_EQ(LoadLE16(format.data() + 6), cluster_bytes / 512);
    CHECK_EQ(xfs::GetLE32(format.data() + 16), 2u);  // 64 chunks, 2 words
    CHECK_EQ(format.size(), lf::kBufLogFormatSize + 2 * 4);
  }

  // Re-logging the whole cluster revives only the cluster buffer; the older
  // inode write into the same cluster stays stale.
  auto revived = AgfBuffer(0x3333, cluster_bytes);
  CHECK(txn.AddMetadataBuffer(cluster_offset, std::move(revived),
                              lf::kBlftAgf, xfs::ondisk::agf::kLsn,
                              xfs::ondisk::agf::kCrc, "inode cluster")
            .ok());
  CHECK_EQ(txn.writes().size(), 2u);
  for (const xfs::TransactionWrite& write : txn.writes()) {
    if (write.offset == cluster_offset) {
      CHECK(!write.suppressed);
    } else {
      CHECK(write.suppressed);
    }
  }
  for (const xfs::LogOp& op : txn.ops()) {
    CHECK(op.payload.size() < 6 ||
          (LoadLE16(op.payload.data() + 4) & lf::kBlfCancel) == 0);
  }
}

TEST(recovery_writer_cancel_protects_direct_symlink_reuse) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  FreedRemoteSymlink freed;
  CHECK(CreateAndFreeRemoteSymlink(*fs.value(), "reuse-link", &freed));
  if (freed.blocks == 0) {
    fixture.Reset();
    return;
  }
  const xfs::XfsGeometry geo = fs.value()->geometry();
  const xfs::XfsSuperblock sb = fs.value()->superblock();
  const uint64_t offset = geo.FsbToByte(freed.fsb);
  const size_t bytes = static_cast<size_t>(freed.blocks) * geo.block_size;

  // The old remote-symlink buffer item is still in the dirty log. Publish new
  // bytes into the freed address with a plain (not logged) write, as a data
  // reuse would.
  std::vector<uint8_t> reuse(bytes, 0x5B);
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) {
    fixture.Reset();
    return;
  }
  CHECK(memory->Write(offset, reuse.data(), reuse.size()));
  CHECK(memory->Flush());

  fs.value().reset();  // crash before a clean unmount
  auto scan = xfs::ScanLog(*memory, sb, geo);
  CHECK(scan.ok());
  if (!scan.ok()) {
    fixture.Reset();
    return;
  }
  CHECK(scan.value().Recoverable());
  CHECK(ScanHasCancelItem(scan.value(), offset / 512,
                          static_cast<uint32_t>(bytes / 512)));

  auto recovered = fixture.Mount(/*read_write=*/true);
  CHECK(recovered.ok());
  if (!recovered.ok()) {
    fixture.Reset();
    return;
  }
  std::vector<uint8_t> after(bytes, 0);
  CHECK(memory->Read(offset, after.data(), after.size()));
  CHECK(after == reuse);  // the cancelled older item did not overwrite it
  recovered.value().reset();
  fixture.Reset();
}

TEST(recovery_writer_cancel_allows_later_journaled_reuse) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  FreedRemoteSymlink freed;
  CHECK(CreateAndFreeRemoteSymlink(*fs.value(), "relog-link", &freed));
  if (freed.blocks == 0) {
    fixture.Reset();
    return;
  }
  const xfs::XfsGeometry geo = fs.value()->geometry();
  const xfs::XfsSuperblock sb = fs.value()->superblock();
  const uint64_t offset = geo.FsbToByte(freed.fsb);
  const size_t bytes = static_cast<size_t>(freed.blocks) * geo.block_size;

  // Reuse the address for new metadata in a later transaction. The cancel was
  // logged earlier, so recovery consumes it before this record and must replay
  // the new item.
  auto txn = fs.value()->BeginTransaction();
  CHECK(txn.ok());
  if (!txn.ok()) {
    fixture.Reset();
    return;
  }
  auto image = AgfBuffer(0x5678, bytes);
  CHECK(txn.value()
            .AddMetadataBuffer(offset, std::move(image), lf::kBlftAgf,
                               xfs::ondisk::agf::kLsn,
                               xfs::ondisk::agf::kCrc, "reuse")
            .ok());
  const std::vector<uint8_t>* queued = txn.value().QueuedBuffer(offset);
  CHECK(queued != nullptr);
  if (!queued) {
    fixture.Reset();
    return;
  }
  const std::vector<uint8_t> expected = *queued;
  CHECK(fs.value()->CommitTransaction(txn.value()).ok());

  // Model a crash that loses this metadata write-back: the log record is
  // durable, the buffer contents are not.
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) {
    fixture.Reset();
    return;
  }
  std::vector<uint8_t> lost(bytes, 0);
  CHECK(memory->Write(offset, lost.data(), lost.size()));
  CHECK(memory->Flush());
  fs.value().reset();

  auto scan = xfs::ScanLog(*memory, sb, geo);
  CHECK(scan.ok());
  if (scan.ok()) {
    CHECK(ScanHasCancelItem(scan.value(), offset / 512,
                            static_cast<uint32_t>(bytes / 512)));
  }
  auto recovered = fixture.Mount(/*read_write=*/true);
  CHECK(recovered.ok());
  if (!recovered.ok()) {
    fixture.Reset();
    return;
  }
  std::vector<uint8_t> after(bytes, 0);
  CHECK(memory->Read(offset, after.data(), after.size()));
  CHECK(after == expected);  // the post-cancel item was replayed
  recovered.value().reset();
  fixture.Reset();
}

TEST(recovery_replay_refuses_unsupported_items) {
  SyntheticLog log;
  std::vector<uint8_t> dquot(lf::kIntentLogFormatSize, 0);
  PutLE16At(dquot.data(), 0, lf::kItemDquot);
  PutLE16At(dquot.data(), 2, 1);
  auto ops = CommitOps(31, {dquot});
  log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 31, ops);

  auto scan = log.Scan();
  CHECK(scan.ok());
  if (!scan.ok()) return;
  auto replay = log.Replay(scan.value(), nullptr);
  CHECK(!replay.ok());
  if (!replay.ok()) {
    CHECK(replay.error().message.find("XFS_LI_DQUOT") != std::string::npos);
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

TEST(recovery_refuses_shared_or_malformed_rui_before_writes) {
  for (int variant = 0; variant < 2; ++variant) {
    xfstest::MemoryBlockDevice device(300ull << 20);
    xfs::mkfs::FormatOptions options;
    options.block_size = 4096;
    options.inode_size = 512;
    options.rmapbt = true;
    options.quiet = true;
    CHECK(xfs::mkfs::Format(device, options).ok());
    auto sb = xfs::ReadSuperblock(device, 0, true);
    CHECK(sb.ok());
    if (!sb.ok()) return;
    const xfs::XfsGeometry geo = xfs::MakeGeometry(sb.value());

    auto position = xfs::ReadLogPosition(device, geo, sb.value());
    CHECK(position.ok());
    if (!position.ok()) return;
    std::vector<uint8_t> rui(lf::kRuiFormatSize + lf::kRuiExtentSize, 0);
    PutLE16At(rui.data(), 0, lf::kItemRui);
    PutLE16At(rui.data(), 2, 1);
    PutLE32At(rui.data(), lf::kIntentNextentsOffset, 1);
    PutLE64At(rui.data(), lf::kIntentIdOffset, 0x5b6c + variant);
    PutLE64At(rui.data() + lf::kRuiFormatSize + lf::kRuiOwnerOffset, 0,
              sb.value().rootino);
    PutLE64At(rui.data() + lf::kRuiFormatSize + lf::kRuiStartblockOffset, 0,
              geo.AgFsb(0, 100));
    PutLE64At(rui.data() + lf::kRuiFormatSize + lf::kRuiStartoffOffset, 0, 0);
    PutLE32At(rui.data() + lf::kRuiFormatSize + lf::kRuiLenOffset, 0, 1);
    uint32_t flags = lf::kRmapExtentMapShared;
    if (variant == 1) {
      flags = lf::kRmapExtentMap | (1u << 8);  // reserved flag bit
    }
    PutLE32At(rui.data() + lf::kRuiFormatSize + lf::kRuiFlagsOffset, 0, flags);

    xfs::LogRecordParams params;
    params.cycle = position.value().cycle;
    params.block = position.value().next_block;
    params.tid = 0x5b6c + variant;
    params.prev_block = position.value().prev_block;
    params.tail_lsn = xfs::MakeLsn(position.value().cycle,
                                   position.value().next_block);
    params.log_blocks = position.value().log_blocks;
    params.record_size = position.value().record_size;
    params.uuid = geo.meta_uuid;
    auto record = xfs::BuildLogRecord(
        params, CommitOps(0x5b6c + variant, {rui}));
    CHECK(record.ok());
    if (!record.ok()) return;
    CHECK(device.Write(geo.FsbToByte(sb.value().logstart) +
                           static_cast<uint64_t>(params.block) * 512,
                       record.value().data(), record.value().size()));
    CHECK(device.Flush());

    const std::vector<uint8_t> pristine = device.bytes();
    xfs::MountOptions mount_options;
    mount_options.read_write = true;
    mount_options.recovery_only = true;
    mount_options.clean_log_after_recovery = true;
    auto fs = xfs::XfsFilesystem::Mount(
        std::make_unique<xfstest::BorrowedBlockDevice>(&device),
        mount_options);
    CHECK(!fs.ok());
    if (!fs.ok()) {
      CHECK(fs.error().message.find("RUI") != std::string::npos ||
            fs.error().message.find("reflink") != std::string::npos ||
            fs.error().message.find("reserved") != std::string::npos);
    }
    CHECK(device.bytes() == pristine);
  }
}

TEST(recovery_mount_completes_surviving_rui_convert) {
  // A Linux RUI that survives without a matching RUD must be completed by a
  // normal transaction that logs the rmap buffers and the RUD together.
  xfstest::MemoryBlockDevice device(300ull << 20);
  xfs::mkfs::FormatOptions options;
  options.block_size = 4096;
  options.inode_size = 512;
  options.rmapbt = true;
  options.quiet = true;
  CHECK(xfs::mkfs::Format(device, options).ok());

  auto sb0 = xfs::ReadSuperblock(device, 0, true);
  CHECK(sb0.ok());
  if (!sb0.ok()) return;
  const xfs::XfsGeometry geo = xfs::MakeGeometry(sb0.value());
  xfs::Allocation alloc;
  {
    // Allocate an unwritten extent and map it exactly as the writer does.
    xfs::XfsTransaction txn;
    xfs::BlockAllocator blocks(&txn, &device, &geo);
    CHECK(blocks
              .AllocateOwned(0, 4,
                             xfs::RmapOwner::Data(sb0.value().rootino, 0x123,
                                                  true),
                             &alloc)
              .ok());
    CHECK(blocks.Commit().ok());
    for (const xfs::TransactionWrite& write : txn.writes()) {
      CHECK(device.Write(write.offset, write.data.data(),
                         write.data.size()));
    }
    CHECK(device.Flush());
  }

  // Append a committed RUI/CONVERT (new state written) at the next log block.
  {
    auto position = xfs::ReadLogPosition(device, geo, sb0.value());
    CHECK(position.ok());
    if (!position.ok()) return;
    std::vector<uint8_t> rui(lf::kRuiFormatSize + lf::kRuiExtentSize, 0);
    PutLE16At(rui.data(), 0, lf::kItemRui);
    PutLE16At(rui.data(), 2, 1);
    PutLE32At(rui.data(), lf::kIntentNextentsOffset, 1);
    PutLE64At(rui.data(), lf::kIntentIdOffset, 0x5b6b);
    PutLE64At(rui.data() + lf::kRuiFormatSize + lf::kRuiOwnerOffset, 0,
              sb0.value().rootino);
    PutLE64At(rui.data() + lf::kRuiFormatSize + lf::kRuiStartblockOffset, 0,
              alloc.fsb);
    PutLE64At(rui.data() + lf::kRuiFormatSize + lf::kRuiStartoffOffset, 0,
              0x123);
    PutLE32At(rui.data() + lf::kRuiFormatSize + lf::kRuiLenOffset, 0,
              alloc.len);
    // The intent describes the *new* state: converting the unwritten extent
    // to written, so the UNWRITTEN bit is clear.
    PutLE32At(rui.data() + lf::kRuiFormatSize + lf::kRuiFlagsOffset, 0,
              lf::kRmapExtentConvert);

    xfs::LogRecordParams params;
    params.cycle = position.value().cycle;
    params.block = position.value().next_block;
    params.tid = 0x5b6b;
    params.prev_block = position.value().prev_block;
    params.tail_lsn = xfs::MakeLsn(position.value().cycle,
                                   position.value().next_block);
    params.log_blocks = position.value().log_blocks;
    params.record_size = position.value().record_size;
    params.uuid = geo.meta_uuid;
    auto record = xfs::BuildLogRecord(params, CommitOps(0x5b6b, {rui}));
    CHECK(record.ok());
    if (!record.ok()) return;
    CHECK(device.Write(geo.FsbToByte(sb0.value().logstart) +
                           static_cast<uint64_t>(params.block) * 512,
                       record.value().data(), record.value().size()));
    CHECK(device.Flush());
  }

  const std::vector<uint8_t> dirty = device.bytes();
  xfs::MountOptions mount_options;
  mount_options.read_write = true;
  mount_options.recovery_only = true;
  // Keep the completion transaction in the log for the structural assertion
  // below; the crash loop uses a copy that closes with an unmount record.
  mount_options.clean_log_after_recovery = false;
  auto fs = xfs::XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(&device), mount_options);
  if (!fs.ok()) {
    std::fprintf(stderr, "rui mount failed: %s\n", fs.error().Format().c_str());
  }
  CHECK(fs.ok());
  if (!fs.ok()) return;
  CHECK(fs.value()->recovery_report().has_value());
  if (fs.value()->recovery_report().has_value()) {
    CHECK_EQ(fs.value()->recovery_report()->replay.intents_pending, 1u);
    CHECK_EQ(fs.value()->recovery_report()->intents_completed, 1u);
  }
  fs.value().reset();

  // Atomicity assertion independent of the final filesystem state: scan the
  // still-dirty log and require one committed transaction carrying both the
  // RUD and at least one metadata buffer item.
  {
    auto after_sb = xfs::ReadSuperblock(device, 0, true);
    CHECK(after_sb.ok());
    if (!after_sb.ok()) return;
    auto after_scan =
        xfs::ScanLog(device, after_sb.value(),
                     xfs::MakeGeometry(after_sb.value()));
    CHECK(after_scan.ok());
    if (!after_scan.ok()) return;
    bool saw = false;
    for (const auto& trans : after_scan.value().transactions) {
      bool rud = false;
      size_t buffers = 0;
      for (const auto& item : trans.items) {
        if (item.type == lf::kItemRud) rud = true;
        if (item.type == lf::kItemBuf) ++buffers;
      }
      if (rud) {
        CHECK(buffers > 0);
        saw = true;
      }
    }
    CHECK(saw);
    CHECK_EQ(xfs::CollectPendingIntents(after_scan.value()).size(), 0u);
  }

  // Crash at every recovery write and restart: the existing RUI must not run
  // twice, and the committed RUD must be replayed instead.
  class LimitedDevice final : public xfs::BlockDevice {
   public:
    explicit LimitedDevice(xfs::BlockDevice* backing) : backing_(backing) {}
    void FailAfter(uint64_t writes) { fail_after_ = writes; }
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
    xfs::BlockDevice* backing_;
    uint64_t fail_after_ = ~0ull;
    uint64_t writes_ = 0;
  };

  xfs::MountOptions crash_mount_options = mount_options;
  crash_mount_options.clean_log_after_recovery = true;
  uint64_t verified_cuts = 0;
  for (uint64_t cut = 0; cut < 64; ++cut) {
    device.bytes() = dirty;
    bool crashed_early = false;
    {
      auto limited = std::make_unique<LimitedDevice>(&device);
      limited->FailAfter(cut);
      auto crashed = xfs::XfsFilesystem::Mount(std::move(limited),
                                               crash_mount_options);
      crashed_early = !crashed.ok();
    }
    if (!crashed_early) break;
    auto retry = xfs::XfsFilesystem::Mount(
        std::make_unique<xfstest::BorrowedBlockDevice>(&device),
        crash_mount_options);
    if (!retry.ok()) {
      std::fprintf(stderr, "RUI restart after cut %llu failed: %s\n",
                   static_cast<unsigned long long>(cut),
                   retry.error().Format().c_str());
      CHECK(retry.ok());
      return;
    }
    // The final log position after a torn completion record can legitimately
    // differ from the clean reference (stale blocks were trimmed first), so
    // the crash oracle compares semantics: no pending RUI, the converted rmap
    // record is present exactly once, and the log ends clean.
    auto final_sb = xfs::ReadSuperblock(device, 0, true);
    CHECK(final_sb.ok());
    if (!final_sb.ok()) return;
    auto final_scan =
        xfs::ScanLog(device, final_sb.value(),
                     xfs::MakeGeometry(final_sb.value()));
    CHECK(final_scan.ok());
    if (!final_scan.ok()) return;
    CHECK(final_scan.value().Recoverable());
    CHECK_EQ(xfs::CollectPendingIntents(final_scan.value()).size(), 0u);
    CHECK(final_scan.value().state == xfs::LogState::kClean ||
          final_scan.value().state == xfs::LogState::kEmpty);
    auto final_geo = xfs::MakeGeometry(final_sb.value());
    auto final_tree =
        xfs::LoadRmapTree(device, final_geo,
                          static_cast<uint32_t>(alloc.fsb >> final_geo.agblklog),
                          final_sb.value().lsn);
    CHECK(final_tree.ok());
    if (!final_tree.ok()) return;
    auto converted = final_tree.value().LookupEq(xfs::RmapKey{
        static_cast<uint32_t>(alloc.fsb &
                              ((1ULL << final_geo.agblklog) - 1)),
        final_sb.value().rootino, 0x123, 0});
    CHECK(converted.ok());
    if (converted.ok()) {
      CHECK_EQ(converted.value().blockcount, alloc.len);
      CHECK(!converted.value().unwritten());
    }
    ++verified_cuts;
  }
  CHECK(verified_cuts >= 3);

  auto sb = xfs::ReadSuperblock(device, 0, true);
  CHECK(sb.ok());
  if (!sb.ok()) return;
  const xfs::XfsGeometry after = xfs::MakeGeometry(sb.value());
  const uint32_t agno = static_cast<uint32_t>(alloc.fsb >> after.agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(alloc.fsb & ((1ULL << after.agblklog) - 1));
  auto tree = xfs::LoadRmapTree(device, after, agno, sb.value().lsn);
  CHECK(tree.ok());
  if (!tree.ok()) return;
  auto rec = tree.value().LookupEq(
      xfs::RmapKey{agbno, sb.value().rootino, 0x123, 0});
  CHECK(rec.ok());
  if (rec.ok()) {
    CHECK_EQ(rec.value().blockcount, alloc.len);
    CHECK(!rec.value().unwritten());
  }
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
