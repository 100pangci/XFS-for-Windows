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

TEST(log_record_rejects_bad_parameters) {
  // Record larger than a single header block's worth of cycle data.
  LogRecordParams params = BaseParams();
  std::vector<LogOp> huge =
      FrameOps(0x11223344, {std::vector<uint8_t>(40 * 1024, 0x33)});
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
