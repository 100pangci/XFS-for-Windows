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
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/inode.h"
#include "xfs/log_format.h"
#include "xfs/log_record.h"
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

 private:
  uint32_t log_blocks_;
  xfstest::MemoryBlockDevice device_;
  xfs::XfsSuperblock sb_;
  xfs::XfsGeometry geo_;
};

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
  CHECK_EQ(log.WriteRecord(1, 0, 0xffffffffu, xfs::MakeLsn(1, 0), 7, ops), 2u);

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
  CHECK_EQ(log.WriteRecord(2, 62, 60, xfs::MakeLsn(2, 2), before, ops), 2u);

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
  CHECK_EQ(log.WriteRecord(2, 62, 60, xfs::MakeLsn(2, 2), tid, ops), 2u);

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
