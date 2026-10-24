// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - journal (log) writer tests.
//
// These tests pin down the on-disk log format (record header, op chain, cycle
// stamping, CRC), the clean-log write position, the first metadata mutation
// (inode timestamps through a transaction) and the write-ahead ordering of a
// commit. tests/tools/log_probe.py is the independent oracle that reimplements
// the same format from the kernel sources and runs over real images in CI.
#include <cstring>
#include <string>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/formatter.h"
#include "xfs/inode.h"
#include "xfs/log_checker.h"
#include "xfs/log_format.h"
#include "xfs/log_record.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"

namespace {

namespace lf = xfs::logfmt;
namespace di = xfs::ondisk::inode;
using xfs::GetBE32;
using xfs::GetBE64;
using xfs::GetLE32;
using xfs::LogOp;
using xfs::LogRecordParams;

constexpr char kUuid[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                            0x47, 0x88, 0x99, 0xaa, 0xbb, 0xcc,
                            0xdd, 0xee, 0xff, 0x00};

// Recomputes the record CRC the way xlog_cksum does (header with the CRC field
// zeroed, then the data area as it lies on disk), independently of the code
// under test.
uint32_t IndependentRecordCrc(const std::vector<uint8_t>& record) {
  std::vector<uint8_t> header(record.begin(),
                              record.begin() + lf::kRecCrcCovered);
  xfs::PutLE32(header.data(), lf::kRecCrc, 0);
  const uint32_t len = GetBE32(record.data(), lf::kRecLen);
  uint32_t crc = xfs::Crc32cUpdate(0xFFFFFFFFu, header.data(), header.size());
  crc = xfs::Crc32cUpdate(crc, record.data() + lf::kRecordHeaderSize, len);
  return ~crc;
}

std::vector<LogOp> FrameOps(uint32_t tid,
                            std::vector<std::vector<uint8_t>> items) {
  std::vector<LogOp> ops;
  ops.push_back(LogOp(lf::kFlagStartTrans, {}));
  std::vector<uint8_t> trans_header;
  trans_header.reserve(lf::kTransHeaderSize);
  xfs::AppendTransHeader(&trans_header, tid,
                         static_cast<uint32_t>(items.size()));
  ops.push_back(LogOp(0, std::move(trans_header)));
  for (auto& item : items) {
    ops.push_back(LogOp(0, std::move(item)));
  }
  ops.push_back(LogOp(lf::kFlagCommitTrans, {}));
  return ops;
}

LogRecordParams BaseParams() {
  LogRecordParams params;
  params.cycle = 3;
  params.block = 42;
  params.tid = 0x11223344;
  params.prev_block = 41;
  params.tail_lsn = xfs::MakeLsn(3, 42);
  params.log_blocks = 2048;
  std::memcpy(params.uuid.data(), kUuid, 16);
  return params;
}

// Records every device operation so the write-ahead ordering can be checked.
class RecordingDevice final : public xfs::BlockDevice {
 public:
  explicit RecordingDevice(xfs::BlockDevice* backing) : backing_(backing) {}

  struct Op {
    bool flush = false;
    uint64_t offset = 0;
    size_t size = 0;
  };

  bool Read(uint64_t offset, void* buffer, size_t size) override {
    return backing_->Read(offset, buffer, size);
  }
  bool Write(uint64_t offset, const void* buffer, size_t size) override {
    ops.push_back({false, offset, size});
    return backing_->Write(offset, buffer, size);
  }
  bool Flush() override {
    ops.push_back({true, 0, 0});
    return backing_->Flush();
  }
  uint64_t Size() const override { return backing_->Size(); }
  uint32_t SectorSize() const override { return backing_->SectorSize(); }
  const xfs::DeviceInfo& Info() const override { return backing_->Info(); }
  void Close() override {}

  std::vector<Op> ops;

 private:
  xfs::BlockDevice* backing_;
};

}  // namespace

TEST(log_record_layout_matches_the_kernel_format) {
  LogRecordParams params = BaseParams();
  std::vector<LogOp> ops = FrameOps(0x11223344, {std::vector<uint8_t>(16, 0x11),
                                                 std::vector<uint8_t>(236, 0x22)});
  auto built = xfs::BuildLogRecord(params, ops);
  CHECK(built.ok());
  if (!built.ok()) return;
  const std::vector<uint8_t>& record = built.value();

  // 12 (start) + 28 (trans header + op) + 28 + 248 (items + op headers) + 12
  // (commit) = 328 data bytes, padded to the log sector size.
  CHECK_EQ(record.size(), lf::kRecordHeaderSize + 512u);

  CHECK_EQ(GetBE32(record.data(), lf::kRecMagic), lf::kRecordMagic);
  CHECK_EQ(GetBE32(record.data(), lf::kRecCycle), 3u);
  CHECK_EQ(GetBE32(record.data(), lf::kRecVersion), lf::kVersion2);
  CHECK_EQ(GetBE32(record.data(), lf::kRecLen), 512u);
  CHECK_EQ(GetBE64(record.data(), lf::kRecLsn), xfs::MakeLsn(3, 42));
  CHECK_EQ(GetBE64(record.data(), lf::kRecTailLsn), xfs::MakeLsn(3, 42));
  CHECK_EQ(GetBE32(record.data(), lf::kRecPrevBlock), 41u);
  CHECK_EQ(GetBE32(record.data(), lf::kRecNumLogOps), 5u);
  CHECK_EQ(GetBE32(record.data(), lf::kRecFmt), lf::kFmtLinuxLe);
  CHECK_EQ(GetBE32(record.data(), lf::kRecSize), lf::kDefaultRecordSize);
  CHECK(std::memcmp(record.data() + lf::kRecUuid, kUuid, 16) == 0);

  // The first word of every data block is the record cycle; the original word
  // is preserved in the header (here: the first op's tid).
  CHECK_EQ(GetBE32(record.data() + lf::kRecordHeaderSize, 0), 3u);
  CHECK_EQ(GetBE32(record.data(), lf::kRecCycleData), 0x11223344u);

  // The record CRC covers header (CRC zero) + data as written.
  CHECK_EQ(GetLE32(record.data(), lf::kRecCrc), IndependentRecordCrc(record));

  // Parse the op chain back out of the record.
  std::vector<uint8_t> data(record.begin() + lf::kRecordHeaderSize,
                            record.end());
  xfs::StoreBE32(data.data(), GetBE32(record.data(), lf::kRecCycleData));
  size_t pos = 0;
  auto read_op = [&](uint8_t* flags, uint32_t* len,
                     const uint8_t** payload) -> bool {
    if (pos + lf::kOpHeaderSize > 512) return false;
    *flags = data[pos + lf::kOpFlags];
    *len = GetBE32(data.data(), pos + lf::kOpLen);
    CHECK_EQ(GetBE32(data.data(), pos + lf::kOpTid), 0x11223344u);
    CHECK_EQ(static_cast<uint8_t>(data[pos + lf::kOpClientId]),
             lf::kClientTransaction);
    *payload = data.data() + pos + lf::kOpHeaderSize;
    pos += lf::kOpHeaderSize + *len;
    pos = (pos + 3) & ~size_t{3};
    return true;
  };
  uint8_t flags = 0;
  uint32_t len = 0;
  const uint8_t* payload = nullptr;
  CHECK(read_op(&flags, &len, &payload));
  CHECK_EQ(static_cast<int>(flags), lf::kFlagStartTrans);
  CHECK_EQ(len, 0u);
  CHECK(read_op(&flags, &len, &payload));
  CHECK_EQ(static_cast<int>(flags), 0);
  CHECK_EQ(len, lf::kTransHeaderSize);
  CHECK_EQ(xfs::LoadLE32(payload + lf::kThMagic), lf::kTransHeaderMagic);
  CHECK_EQ(xfs::LoadLE32(payload + lf::kThType), lf::kTransTypeCheckpoint);
  CHECK_EQ(xfs::LoadLE32(payload + lf::kThTid), 0x11223344u);
  CHECK_EQ(xfs::LoadLE32(payload + lf::kThNumItems), 2u);
  CHECK(read_op(&flags, &len, &payload));
  CHECK_EQ(len, 16u);
  CHECK(read_op(&flags, &len, &payload));
  CHECK_EQ(len, 236u);
  CHECK(read_op(&flags, &len, &payload));
  CHECK_EQ(static_cast<int>(flags), lf::kFlagCommitTrans);
  CHECK_EQ(len, 0u);
}

TEST(log_record_larger_than_32k_uses_extension_headers) {
  // A legal Linux record whose data area needs an iclog larger than 32 KiB.
  // The writer must choose h_size (64 KiB here), place the extra cycle words
  // in xlog_rec_ext_header blocks and produce a CRC that covers them.
  std::vector<std::vector<uint8_t>> items;
  for (uint32_t i = 0; i < 24; ++i) {
    items.emplace_back(2500, static_cast<uint8_t>(i * 7 + 1));
  }
  LogRecordParams params = BaseParams();
  params.log_blocks = 2048;  // 1 MiB log
  auto record = xfs::BuildLogRecord(params, FrameOps(7, std::move(items)));
  CHECK(record.ok());
  const std::vector<uint8_t>& r = record.value();
  CHECK(GetBE32(r.data(), lf::kRecSize) == 64 * 1024);
  CHECK(GetBE32(r.data(), lf::kRecLen) > lf::kHeaderCycleBytes);
  CHECK(GetBE32(r.data(), lf::kRecLen) <= 64 * 1024);
  CHECK(r.size() > lf::kHeaderCycleBytes);  // header blocks + data blocks
  CHECK(r.size() <= lf::kMaxRecordBytes);

  // Extension header: its cycle word and the first saved data cycle word.
  const uint8_t* ext = r.data() + lf::kRecordHeaderSize;
  CHECK(GetBE32(ext, lf::kRecExtCycle) == 3);
  const uint32_t data_blocks = GetBE32(r.data(), lf::kRecLen) / 512;
  CHECK(data_blocks > lf::kRecCycleDataCount);
  // Data block 0 starts with the START op's transaction id, which is saved in
  // the header while the on-disk word is the record cycle.
  CHECK(GetBE32(r.data(), lf::kRecCycleData) == params.tid);
  CHECK(GetBE32(r.data() + 2 * lf::kRecordHeaderSize, 0) == 3);

  // Independent CRC over fixed header + extension header + data.
  std::vector<uint8_t> fixed(r.begin(), r.begin() + lf::kRecCrcCovered);
  xfs::PutLE32(fixed.data(), lf::kRecCrc, 0);
  uint32_t crc = xfs::Crc32cUpdate(0xFFFFFFFFu, fixed.data(), fixed.size());
  crc = xfs::Crc32cUpdate(crc, ext, lf::kRecExtSize);
  crc = xfs::Crc32cUpdate(crc, r.data() + 2 * lf::kRecordHeaderSize,
                          GetBE32(r.data(), lf::kRecLen));
  CHECK(~crc == GetLE32(r.data(), lf::kRecCrc));
}

TEST(transaction_larger_than_one_record_is_split_and_recovers) {
  // A transaction whose item stream exceeds one 32 KiB record must be split
  // into multiple records with a single COMMIT. Reading it back through the
  // independent scanner proves the CONTINUE/WAS_CONT framing and the one
  // transaction id survive the split.
  xfstest::MemoryBlockDevice device(300ull << 20);
  xfs::mkfs::FormatOptions options;
  options.block_size = 4096;
  options.inode_size = 512;
  options.quiet = true;
  CHECK(xfs::mkfs::Format(device, options).ok());
  auto mounted = xfs::XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(&device),
      [] {
        xfs::MountOptions m;
        m.read_write = true;
        return m;
      }());
  CHECK(mounted.ok());
  if (!mounted.ok()) return;
  auto txn = mounted.value()->BeginTransaction();
  CHECK(txn.ok());
  if (!txn.ok()) return;
  constexpr uint32_t kItems = 24;
  std::vector<uint8_t> item(2000, 0);
  xfs::StoreLE16(item.data(), lf::kItemBuf);
  xfs::StoreLE16(item.data() + 2, 1);
  for (uint32_t i = 0; i < kItems; ++i) {
    xfs::StoreLE32(item.data() + 8, i);
    txn.value().AddLogOp(item);
  }
  // One region larger than a whole record forces a payload split with
  // CONTINUE/WAS_CONT, not just an op-boundary split.
  std::vector<uint8_t> huge(70000, 0x5a);
  xfs::StoreLE16(huge.data(), lf::kItemBuf);
  xfs::StoreLE16(huge.data() + 2, 1);
  txn.value().AddLogOp(std::move(huge));
  CHECK(txn.value().serialized_log_bytes() > lf::kHeaderCycleBytes);
  CHECK(txn.value().record_fits());
  CHECK(mounted.value()->CommitTransaction(txn.value()).ok());
  mounted.value().reset();

  auto sb = xfs::ReadSuperblock(device, 0, true);
  CHECK(sb.ok());
  if (!sb.ok()) return;
  auto scan =
      xfs::ScanLog(device, sb.value(), xfs::MakeGeometry(sb.value()));
  CHECK(scan.ok());
  if (!scan.ok()) return;
  CHECK(scan.value().state == xfs::LogState::kDirty);
  CHECK(scan.value().records.size() >= 2);
  CHECK_EQ(scan.value().transactions.size(), 1u);
  if (!scan.value().transactions.empty()) {
    CHECK_EQ(scan.value().transactions[0].items.size(), kItems + 1);
    const auto& items = scan.value().transactions[0].items;
    if (items.size() == kItems + 1 && !items.back().regions.empty()) {
      CHECK_EQ(items.back().regions[0].size(), 70000u);
    }
  }
}

TEST(log_record_rejects_bad_parameters) {
  // Larger than XLOG_MAX_RECORD_BSIZE (256 KiB): the caller must split the
  // metadata across transactions instead of emitting an illegal record.
  LogRecordParams params = BaseParams();
  params.log_blocks = 4096;
  std::vector<LogOp> huge =
      FrameOps(0x11223344, {std::vector<uint8_t>(300 * 1024, 0x33)});
  auto built = xfs::BuildLogRecord(params, huge);
  CHECK(!built.ok());
  if (!built.ok()) CHECK(built.error().code == xfs::XfsError::kUnsupportedFeature);

  // Block outside the log, and the cycles that must never be written.
  params = BaseParams();
  params.block = params.log_blocks;
  CHECK(!xfs::BuildLogRecord(params, FrameOps(0x11223344, {})).ok());
  params = BaseParams();
  params.cycle = 0;
  CHECK(!xfs::BuildLogRecord(params, FrameOps(0x11223344, {})).ok());
  params = BaseParams();
  params.log_blocks = 0;
  CHECK(!xfs::BuildLogRecord(params, FrameOps(0x11223344, {})).ok());
  CHECK(!xfs::BuildLogRecord(BaseParams(), {}).ok());
}

TEST(log_position_of_a_fresh_fixture_is_the_kernel_state) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  const xfs::XfsGeometry& geo = fs.value()->geometry();
  auto position =
      xfs::ReadLogPosition(*dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage()),
                           geo, fs.value()->superblock());
  CHECK(position.ok());
  if (!position.ok()) return;
  // The formatter writes one unmount record (header + one data block) at block
  // 0, so the next record goes to block 2 in the same cycle, linked back to 0,
  // and the tail of the log is that next record.
  CHECK(position.value().clean);
  CHECK_EQ(position.value().cycle, 1u);
  CHECK_EQ(position.value().next_block, 2u);
  CHECK_EQ(position.value().prev_block, 0u);
  CHECK_EQ(position.value().tail_lsn, xfs::MakeLsn(1, 2));
  CHECK_EQ(position.value().log_blocks,
           static_cast<uint32_t>((geo.logblocks << geo.block_log) / 512));
}

TEST(clean_unmount_record_closes_the_log) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) return;

  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  const xfs::XfsGeometry& geo = fs.value()->geometry();
  const uint64_t ino = fixture.info().file_ino;

  std::vector<uint8_t> overlay(64, 0x7E);
  auto written = fs.value()->OverwriteFileData(ino, 4096, overlay.data(),
                                               overlay.size());
  CHECK(written.ok());
  // After a transaction the log is dirty: the newest record is a transaction
  // record, and a read-write mount must not start from it.
  auto check = xfs::CheckLogClean(*memory, geo, fs.value()->superblock());
  CHECK(check.ok());
  if (check.ok()) CHECK(check.value().state == xfs::LogState::kDirty);
  auto position = xfs::ReadLogPosition(*memory, geo, fs.value()->superblock());
  CHECK(!position.ok());
  if (!position.ok()) CHECK(position.error().code == xfs::XfsError::kDirtyLog);

  // Writing the unmount record closes the log again.
  CHECK(fs.value()->WriteCleanUnmountRecord().ok());
  check = xfs::CheckLogClean(*memory, geo, fs.value()->superblock());
  CHECK(check.ok());
  if (check.ok()) CHECK(check.value().state == xfs::LogState::kClean);
  // ... and the next writer starts after it.
  position = xfs::ReadLogPosition(*memory, geo, fs.value()->superblock());
  CHECK(position.ok());
  if (position.ok()) {
    CHECK(position.value().clean);
    CHECK(position.value().cycle == 1u);
    CHECK(position.value().prev_block == 4u);  // tx record blocks 2-3, then unmount
    CHECK(position.value().next_block == 6u);
  }
  fixture.Reset();
}

TEST(log_position_crosses_filesystem_block_count_boundary) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  const auto& geo = fs.value()->geometry();
  const auto& sb = fs.value()->superblock();
  const uint64_t log_start = geo.FsbToByte(sb.logstart);

  // sb_logblocks counts filesystem blocks, but record LSNs and the log head
  // count 512-byte basic blocks. Seed a clean close just before the old
  // scanner's erroneous boundary and let the next transaction cross it.
  LogRecordParams params = BaseParams();
  params.uuid = sb.uuid;
  params.log_blocks = static_cast<uint32_t>(
      (static_cast<uint64_t>(sb.logblocks) << geo.block_log) / 512);
  CHECK(params.log_blocks > sb.logblocks + 4);
  params.cycle = 7;
  params.block = sb.logblocks - 2;
  params.prev_block = 0xffffffffu;
  params.tail_lsn = xfs::MakeLsn(params.cycle, params.block);
  auto record = xfs::BuildLogRecord(params, {xfs::MakeUnmountOp()});
  CHECK(record.ok());
  CHECK_EQ(record.value().size(), 1024u);
  CHECK(memory->Write(log_start + uint64_t{params.block} * 512,
                      record.value().data(), record.value().size()));

  auto position = xfs::ReadLogPosition(*memory, geo, sb);
  CHECK(position.ok());
  CHECK_EQ(position.value().next_block, sb.logblocks);
  const uint8_t byte = 0x7e;
  CHECK(fs.value()->OverwriteFileData(fixture.info().file_ino, 0, &byte, 1)
            .ok());
  auto check = xfs::CheckLogClean(*memory, geo, sb);
  CHECK(check.ok());
  CHECK(check.value().state == xfs::LogState::kDirty);
  CHECK_EQ(check.value().block, sb.logblocks);
  CHECK(!xfs::ReadLogPosition(*memory, geo, sb).ok());

  CHECK(fs.value()->WriteCleanUnmountRecord().ok());
  check = xfs::CheckLogClean(*memory, geo, sb);
  CHECK(check.ok());
  CHECK(check.value().state == xfs::LogState::kClean);
  CHECK(check.value().block > sb.logblocks);
  position = xfs::ReadLogPosition(*memory, geo, sb);
  CHECK(position.ok());
  CHECK(position.value().next_block > sb.logblocks);
  fixture.Reset();
}

TEST(set_file_times_writes_inode_and_log) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) return;
  const auto& pristine = fixture.pristine();
  const uint64_t ino = fixture.info().file_ino;

  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  const xfs::XfsGeometry& geo = fs.value()->geometry();
  const uint64_t offset = geo.InoToByteOffset(ino);

  const xfs::XfsTimestamp atime{1000, 1};
  const xfs::XfsTimestamp mtime{2000, 2};
  const xfs::XfsTimestamp ctime{3000, 3};
  auto info = fs.value()->SetFileTimes(ino, atime, mtime, ctime);
  CHECK(info.ok());
  if (!info.ok()) return;
  CHECK_EQ(info.value().atime.sec, 1000);
  CHECK_EQ(info.value().atime.nsec, 1u);
  CHECK_EQ(info.value().mtime.sec, 2000);
  CHECK_EQ(info.value().ctime.sec, 3000);
  CHECK_EQ(info.value().size, fixture.info().file_size);

  const uint8_t* inode = memory->bytes().data() + offset;
  CHECK_EQ(GetBE32(inode, di::kAtime), 1000u);
  CHECK_EQ(GetBE32(inode, di::kAtime + 4), 1u);
  CHECK_EQ(GetBE32(inode, di::kMtime), 2000u);
  CHECK_EQ(GetBE32(inode, di::kMtime + 4), 2u);
  CHECK_EQ(GetBE32(inode, di::kCtime), 3000u);
  CHECK_EQ(GetBE32(inode, di::kCtime + 4), 3u);
  const uint64_t inode_lsn = GetBE64(inode, di::kLsn);
  CHECK(inode_lsn != 0);
  CHECK(xfs::VerifyXfsChecksum(inode, geo.inode_size, di::kCrc));

  // Only the core fields the transaction owns changed: the forks and every
  // other byte of the inode are still the pristine ones.
  CHECK(std::memcmp(inode, pristine.data() + offset,
                    static_cast<size_t>(geo.inode_size)) != 0);
  for (size_t i = 0; i < geo.inode_size; ++i) {
    if (i >= di::kAtime && i < di::kAtime + 8) continue;
    if (i >= di::kMtime && i < di::kMtime + 8) continue;
    if (i >= di::kCtime && i < di::kCtime + 8) continue;
    if (i >= di::kLsn && i < di::kLsn + 8) continue;
    if (i >= di::kCrc && i < di::kCrc + 4) continue;
    if (inode[i] != pristine[offset + i]) {
      ::xfstest::Fail(__FILE__, __LINE__,
                      "unexpected inode byte change at offset " +
                          std::to_string(i));
      break;
    }
  }

  // The inode's LSN is the LSN of the newest log record: that is what makes
  // replay idempotent (an older record for the same inode is skipped).
  auto check = xfs::CheckLogClean(*memory, geo, fs.value()->superblock());
  CHECK(check.ok());
  if (check.ok()) {
    CHECK(check.value().state == xfs::LogState::kDirty);
    CHECK_EQ(check.value().newest_record_lsn, inode_lsn);
    CHECK_EQ(check.value().num_ops, 5u);
  }

  CHECK(fs.value()->WriteCleanUnmountRecord().ok());
  check = xfs::CheckLogClean(*memory, geo, fs.value()->superblock());
  CHECK(check.ok());
  if (check.ok()) CHECK(check.value().state == xfs::LogState::kClean);
  fixture.Reset();
}

TEST(transaction_writes_log_before_metadata) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) return;
  const uint64_t ino = fixture.info().file_ino;

  // The filesystem owns the recording device; the test keeps a raw pointer to
  // read the operation order after the commit.
  auto* recorder = new RecordingDevice(memory);
  xfs::MountOptions options;
  options.read_write = true;
  std::unique_ptr<xfs::BlockDevice> device(recorder);
  auto fs = xfs::XfsFilesystem::Mount(std::move(device), options);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  const xfs::XfsGeometry& geo = fs.value()->geometry();
  const uint64_t log_byte = geo.FsbToByte(fs.value()->superblock().logstart);
  const uint64_t inode_offset = geo.InoToByteOffset(ino);
  const uint64_t data_start = geo.FsbToByte(fixture.info().data_fsb);

  recorder->ops.clear();
  std::vector<uint8_t> overlay(64, 0x5A);
  auto written = fs.value()->OverwriteFileData(ino, 4096, overlay.data(),
                                               overlay.size());
  CHECK(written.ok());
  if (!written.ok()) {
    fixture.Reset();
    return;
  }

  const auto& ops = recorder->ops;
  // The file data is written first, then the log record (header block, data
  // block), then a flush, and only then the inode. That is the write-ahead
  // rule: a crash can never leave metadata on disk that the log does not
  // describe.
  CHECK(!ops.empty());
  if (ops.empty()) {
    fixture.Reset();
    return;
  }
  CHECK(!ops[0].flush);
  CHECK_EQ(ops[0].offset, data_start + 4096);

  int data_write = 0;
  // The formatter's clean record occupies log blocks 0-1, so this transaction's
  // record starts at block 2: header at +1024, data at +1536.
  const uint64_t log_header_offset = log_byte + 2 * 512;
  int log_header = -1;
  int log_data = -1;
  int log_flush = -1;
  int inode_write = -1;
  int inode_flush = -1;
  for (size_t i = 0; i < ops.size(); ++i) {
    const auto& op = ops[i];
    if (op.flush) {
      if (log_header >= 0 && log_flush < 0) {
        log_flush = static_cast<int>(i);
      } else if (inode_write >= 0 && inode_flush < 0) {
        inode_flush = static_cast<int>(i);
      }
      continue;
    }
    if (log_header < 0 && op.offset == log_header_offset && op.size == 512) {
      log_header = static_cast<int>(i);
    } else if (log_header >= 0 && log_data < 0 &&
               op.offset == log_header_offset + 512) {
      log_data = static_cast<int>(i);
    } else if (log_header >= 0 && inode_write < 0 &&
               op.offset == inode_offset && op.size == 512) {
      inode_write = static_cast<int>(i);
    }
  }
  CHECK(log_header > data_write);
  CHECK_EQ(log_data, log_header + 1);
  CHECK(log_flush > log_data);
  CHECK(inode_write > log_flush);
  CHECK(inode_flush > inode_write);
  fixture.Reset();
}
