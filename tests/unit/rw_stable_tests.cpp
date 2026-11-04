// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - RW-STABLE-1 gate regressions.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/ag.h"
#include "xfs/block_device.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/formatter.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"

using namespace xfs;

namespace {

// Injects a non-terminal failure on the first metadata write after a
// transaction's log record has been written and flushed.  The log range is
// supplied after mount so setup writes are never intercepted.
class FailFirstMetadataWrite final : public xfs::BlockDevice {
 public:
  explicit FailFirstMetadataWrite(xfs::BlockDevice* backing)
      : backing_(backing) {}

  void SetLogRange(uint64_t start, uint64_t end) {
    log_start_ = start;
    log_end_ = end;
  }
  void Enable() { enabled_ = true; }
  void Disable() { enabled_ = false; }
  bool failed() const { return failed_; }

  bool Read(uint64_t offset, void* buffer, size_t size) override {
    return backing_->Read(offset, buffer, size);
  }
  bool Write(uint64_t offset, const void* buffer, size_t size) override {
    const bool in_log =
        offset >= log_start_ && offset + size <= log_end_;
    if (enabled_ && !in_log) {
      enabled_ = false;
      failed_ = true;
      last_error_ = "injected metadata write failure";
      return false;
    }
    return backing_->Write(offset, buffer, size);
  }
  bool Flush() override { return backing_->Flush(); }
  uint64_t Size() const override { return backing_->Size(); }
  uint32_t SectorSize() const override { return backing_->SectorSize(); }
  const xfs::DeviceInfo& Info() const override { return backing_->Info(); }
  void Close() override {}

 private:
  xfs::BlockDevice* backing_;
  uint64_t log_start_ = 0;
  uint64_t log_end_ = 0;
  bool enabled_ = false;
  bool failed_ = false;
};

// Fails one exact AGI-sector read and nothing else.
class FailAgiRead final : public xfs::BlockDevice {
 public:
  FailAgiRead(xfs::BlockDevice* backing, uint64_t agi_offset)
      : backing_(backing), agi_offset_(agi_offset) {}

  bool Read(uint64_t offset, void* buffer, size_t size) override {
    if (offset == agi_offset_ && size <= 512) {
      last_error_ = "injected AGI read failure";
      return false;
    }
    return backing_->Read(offset, buffer, size);
  }
  bool Write(uint64_t offset, const void* buffer, size_t size) override {
    return backing_->Write(offset, buffer, size);
  }
  bool Flush() override { return backing_->Flush(); }
  uint64_t Size() const override { return backing_->Size(); }
  uint32_t SectorSize() const override { return backing_->SectorSize(); }
  const xfs::DeviceInfo& Info() const override { return backing_->Info(); }
  void Close() override {}

 private:
  xfs::BlockDevice* backing_;
  uint64_t agi_offset_;
};

// Fails primary-superblock reads after the mount has completed.
class FailPrimarySuperblockReads final : public xfs::BlockDevice {
 public:
  explicit FailPrimarySuperblockReads(xfs::BlockDevice* backing)
      : backing_(backing) {}

  void Enable() { enabled_ = true; }

  bool Read(uint64_t offset, void* buffer, size_t size) override {
    if (enabled_ && offset == 0 && size >= 512) {
      last_error_ = "injected primary superblock read failure";
      return false;
    }
    return backing_->Read(offset, buffer, size);
  }
  bool Write(uint64_t offset, const void* buffer, size_t size) override {
    return backing_->Write(offset, buffer, size);
  }
  bool Flush() override { return backing_->Flush(); }
  uint64_t Size() const override { return backing_->Size(); }
  uint32_t SectorSize() const override { return backing_->SectorSize(); }
  const xfs::DeviceInfo& Info() const override { return backing_->Info(); }
  void Close() override {}

 private:
  xfs::BlockDevice* backing_;
  bool enabled_ = false;
};

std::vector<uint8_t> ReadLog(xfstest::FixtureImage& fixture, uint64_t start,
                             uint64_t length) {
  std::vector<uint8_t> bytes(length);
  CHECK(fixture.storage().Read(start, bytes.data(), bytes.size()));
  return bytes;
}

// Sets an unknown read-only-compatible bit and keeps the superblock CRC valid,
// so the only reason to refuse is the writable-profile allow-list.
void SetUnknownRoCompat(xfstest::FixtureImage& fixture) {
  auto sb = ReadSuperblock(fixture.storage(), 0, true);
  CHECK(sb.ok());
  if (!sb.ok()) return;
  std::vector<uint8_t> super(sb.value().sectsize, 0);
  CHECK(fixture.storage().Read(0, super.data(), super.size()));
  PutBE32(super.data(), ondisk::sb::kFeaturesRoCompat,
          GetBE32(super.data(), ondisk::sb::kFeaturesRoCompat) | (1u << 4));
  UpdateXfsChecksum(super.data(), super.size(), ondisk::sb::kCrc);
  CHECK(fixture.storage().Write(0, super.data(), super.size()));
}

void CorruptSuperblockCrc(xfstest::FixtureImage& fixture) {
  auto sb = ReadSuperblock(fixture.storage(), 0, true);
  CHECK(sb.ok());
  if (!sb.ok()) return;
  std::vector<uint8_t> super(sb.value().sectsize, 0);
  CHECK(fixture.storage().Read(0, super.data(), super.size()));
  super[ondisk::sb::kCrc] ^= 0xff;
  CHECK(fixture.storage().Write(0, super.data(), super.size()));
}

// A structurally valid geometry whose byte size is exactly 2^64: 2^24 AGs of
// 2^28 4 KiB blocks.  dblocks * block_size wraps to zero in uint64.
void SetOverflowingGeometry(xfstest::FixtureImage& fixture) {
  auto sb = ReadSuperblock(fixture.storage(), 0, true);
  CHECK(sb.ok());
  if (!sb.ok()) return;
  std::vector<uint8_t> super(sb.value().sectsize, 0);
  CHECK(fixture.storage().Read(0, super.data(), super.size()));
  PutBE32(super.data(), ondisk::sb::kAgblocks, 1u << 28);
  PutBE32(super.data(), ondisk::sb::kAgcount, 1u << 24);
  PutBE64(super.data(), ondisk::sb::kDblocks, 1ull << 52);
  super[ondisk::sb::kAgblklog] = 28;
  UpdateXfsChecksum(super.data(), super.size(), ondisk::sb::kCrc);
  CHECK(fixture.storage().Write(0, super.data(), super.size()));
}

}  // namespace

// Once a transaction's log record has been written and flushed, a later
// metadata write failure must disable the mount.  The log position was not
// advanced, so accepting another transaction would reuse the same blocks and
// overwrite the only durable copy of the failed transaction.
TEST(rw_stable_commit_failure_after_log_flush_isolates_mount) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();

  auto device = std::make_unique<FailFirstMetadataWrite>(&fixture.storage());
  auto* faults = device.get();
  xfs::MountOptions options;
  options.read_write = true;
  auto mounted = xfs::XfsFilesystem::Mount(std::move(device), options);
  CHECK(mounted.ok());
  if (!mounted.ok()) {
    fixture.Reset();
    return;
  }
  xfs::XfsFilesystem& fs = *mounted.value();
  const xfs::XfsGeometry& geo = fs.geometry();
  const xfs::XfsSuperblock& sb = fs.superblock();
  const uint64_t log_start = geo.FsbToByte(sb.logstart);
  const uint64_t log_length = static_cast<uint64_t>(sb.logblocks) * 512;
  faults->SetLogRange(log_start, log_start + log_length);

  const uint64_t ino = fixture.info().file_ino;
  const xfs::XfsTimestamp first_stamp{1700030000, 1};
  faults->Enable();
  auto first = fs.SetFileTimes(ino, first_stamp, first_stamp, first_stamp);
  CHECK(!first.ok());
  CHECK(faults->failed());
  CHECK(!fs.Flush().ok());

  const std::vector<uint8_t> log_before =
      ReadLog(fixture, log_start, log_length);

  // Let writes succeed again.  A mount that kept accepting transactions would
  // now reuse the same log position; the isolated mount must refuse instead.
  faults->Disable();
  const xfs::XfsTimestamp second_stamp{1700030100, 2};
  auto second = fs.SetFileTimes(ino, second_stamp, second_stamp, second_stamp);
  CHECK(!second.ok());
  const std::vector<uint8_t> log_after =
      ReadLog(fixture, log_start, log_length);
  CHECK(log_after == log_before);
  CHECK(!fs.Flush().ok());

  fixture.Reset();
}

// `xfswin recover` on a clean log still processes the AGI unlinked lists with
// transactions.  An unsupported writable profile must be refused before that
// first metadata write; the image digest is the evidence.
TEST(rw_stable_clean_log_recover_checks_profile_before_first_write) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) {
    fixture.Reset();
    return;
  }

  uint64_t ino = 0;
  uint32_t agno = 0;
  uint32_t bucket = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) {
      fixture.Reset();
      return;
    }
    auto created = fs.value()->CreateRegularFile(
        fs.value()->RootInode(), "clean-recover-unlinked", 0644, 0, 0);
    CHECK(created.ok());
    if (!created.ok()) {
      fixture.Reset();
      return;
    }
    ino = created.value();
    CHECK(fs.value()->Unlink(fs.value()->RootInode(), "clean-recover-unlinked",
                             /*defer_inode_free=*/true)
              .ok());
    // Close the log without processing the unlinked list: Linux's clean
    // unmount has the same shape, and the next explicit recover must finish
    // the list.
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    agno = fs.value()->geometry().InoAgno(ino);
    bucket = fs.value()->geometry().InoAgino(ino) % 64;
  }
  {
    auto agi = xfs::ReadAgi(*memory, xfs::MakeGeometry(
                                       xfs::ReadSuperblock(*memory, 0, true)
                                           .value()),
                            agno);
    CHECK(agi.ok());
    if (agi.ok()) CHECK(agi.value().unlinked[bucket] != 0xffffffffu);
  }

  // Make CheckWritableProfile refuse the image while keeping every other
  // structure valid: set an unknown read-only-compatible feature bit.
  SetUnknownRoCompat(fixture);
  const std::vector<uint8_t> before = memory->bytes();

  xfs::MountOptions options;
  options.read_write = true;
  options.clean_log_after_recovery = true;  // the `xfswin recover` shape
  options.recovery_only = true;
  auto mounted = xfs::XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(&fixture.storage()),
      options);
  CHECK(!mounted.ok());
  if (!mounted.ok()) CHECK_EQ(mounted.error().code,
                              xfs::XfsError::kUnsupportedFeature);
  CHECK(memory->bytes() == before);
  {
    auto agi = xfs::ReadAgi(*memory, xfs::MakeGeometry(
                                       xfs::ReadSuperblock(*memory, 0, true)
                                           .value()),
                            agno);
    CHECK(agi.ok());
    if (agi.ok()) CHECK(agi.value().unlinked[bucket] != 0xffffffffu);
  }

  fixture.Reset();
}

// The public RecoverJournal API can be called on a mount created with
// check_log=false.  It must refuse an unsupported writable profile before the
// first replay write, just like a read-write mount and `xfswin recover`.
TEST(rw_stable_recover_journal_checks_profile_before_replay) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) {
    fixture.Reset();
    return;
  }

  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) {
      fixture.Reset();
      return;
    }
    const XfsTimestamp stamp{1700031000, 3};
    CHECK(fs.value()
              ->SetFileTimes(fixture.info().file_ino, stamp, stamp, stamp)
              .ok());
    // Dropped without a clean-unmount record: the log is dirty.
  }
  SetUnknownRoCompat(fixture);
  const std::vector<uint8_t> before = memory->bytes();

  MountOptions options;
  options.check_log = false;  // the standalone recovery shape
  auto fs = XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(&fixture.storage()),
      options);
  CHECK(fs.ok());
  if (!fs.ok()) {
    fixture.Reset();
    return;
  }
  auto report =
      fs.value()->RecoverJournal(XfsFilesystem::RecoveryFinish::kMount);
  CHECK(!report.ok());
  if (!report.ok()) {
    CHECK_EQ(report.error().code, XfsError::kUnsupportedFeature);
  }
  CHECK(memory->bytes() == before);
  fs.value().reset();

  fixture.Reset();
}

TEST(rw_stable_mount_rejects_overflowing_geometry) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) {
    fixture.Reset();
    return;
  }
  SetOverflowingGeometry(fixture);
  const std::vector<uint8_t> before = memory->bytes();

  MountOptions options;
  options.read_write = true;
  auto mounted = XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(&fixture.storage()),
      options);
  CHECK(!mounted.ok());
  if (!mounted.ok()) {
    CHECK_EQ(mounted.error().code, XfsError::kCorruptSuperblock);
    CHECK(mounted.error().message.find("larger than the device") !=
          std::string::npos);
  }
  CHECK(memory->bytes() == before);
  fixture.Reset();
}

TEST(rw_stable_read_write_mount_requires_verified_superblock_crc) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) {
    fixture.Reset();
    return;
  }
  CorruptSuperblockCrc(fixture);
  const std::vector<uint8_t> before = memory->bytes();

  MountOptions options;
  options.read_write = true;
  options.verify_crc = false;
  auto mounted = XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(&fixture.storage()),
      options);
  CHECK(!mounted.ok());
  if (!mounted.ok()) {
    CHECK_EQ(mounted.error().code, XfsError::kUnsupportedFeature);
    CHECK(mounted.error().message.find("CRC") != std::string::npos);
  }
  CHECK(memory->bytes() == before);
  fixture.Reset();
}

TEST(rw_stable_unreadable_agi_fails_clean_mount_recovery) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) {
    fixture.Reset();
    return;
  }
  auto sb = ReadSuperblock(*memory, 0, true);
  CHECK(sb.ok());
  if (!sb.ok()) {
    fixture.Reset();
    return;
  }
  const XfsGeometry geo = MakeGeometry(sb.value());
  const uint64_t agi_offset =
      geo.AgByteOffset(0) + 2ull * geo.sector_size;
  const std::vector<uint8_t> before = memory->bytes();

  auto device = std::make_unique<FailAgiRead>(&fixture.storage(), agi_offset);
  MountOptions options;
  options.read_write = true;
  auto mounted = XfsFilesystem::Mount(std::move(device), options);
  CHECK(!mounted.ok());
  if (!mounted.ok()) CHECK_EQ(mounted.error().code, XfsError::kIoError);
  CHECK(memory->bytes() == before);

  fixture.Reset();
}

// A discovery caller pins the filesystem UUID.  A replacement filesystem on
// the same path must be refused before recovery or metadata writes, and the
// matching UUID must still mount.
TEST(rw_stable_mount_enforces_expected_filesystem_uuid) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) {
    fixture.Reset();
    return;
  }
  auto sb = ReadSuperblock(*memory, 0, true);
  CHECK(sb.ok());
  if (!sb.ok()) {
    fixture.Reset();
    return;
  }

  xfs::Uuid wrong = sb.value().uuid;
  wrong[15] ^= 0x01;
  MountOptions options;
  options.read_write = true;
  options.expected_uuid = wrong;
  const std::vector<uint8_t> before = memory->bytes();
  auto refused = XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(&fixture.storage()),
      options);
  CHECK(!refused.ok());
  if (!refused.ok()) {
    CHECK_EQ(refused.error().code, XfsError::kBadUuid);
    CHECK(refused.error().message.find("expected") != std::string::npos);
  }
  CHECK(memory->bytes() == before);

  options.expected_uuid = sb.value().uuid;
  auto mounted = XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(&fixture.storage()),
      options);
  CHECK(mounted.ok());
  if (mounted.ok()) mounted.value().reset();
  fixture.Reset();
}

// The in-place overwrite API writes mapped blocks directly and never performs
// copy-on-write.  On a reflink image, a range still shared through refcountbt
// must be refused before the first data byte is written: otherwise the write
// would silently change the source file's bytes as well.
TEST(rw_stable_in_place_overwrite_refuses_reflink_shared_range) {
  auto device = std::make_unique<xfstest::MemoryBlockDevice>(300ull << 20);
  mkfs::FormatOptions format_options;
  format_options.block_size = 4096;
  format_options.inode_size = 512;
  format_options.quiet = true;
  format_options.reflink = true;
  auto formatted = mkfs::Format(*device, format_options);
  CHECK(formatted.ok());
  if (!formatted.ok()) return;

  MountOptions options;
  options.read_write = true;
  auto mounted = XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(device.get()), options);
  CHECK(mounted.ok());
  if (!mounted.ok()) return;
  XfsFilesystem& fs = *mounted.value();

  auto src = fs.CreateRegularFile(fs.RootInode(), "src", 0644, 0, 0);
  auto dst = fs.CreateRegularFile(fs.RootInode(), "dst", 0644, 0, 0);
  CHECK(src.ok());
  CHECK(dst.ok());
  if (!src.ok() || !dst.ok()) return;

  std::vector<uint8_t> data(8192);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>((i * 29u) ^ 0x3du);
  }
  CHECK(fs.WriteFileData(src.value(), 0, data.data(), data.size()).ok());
  auto cloned = fs.ReflinkRange(src.value(), 0, dst.value(), 0, 0);
  CHECK(cloned.ok());
  if (!cloned.ok()) return;

  const std::vector<uint8_t> before = device->bytes();
  const std::vector<uint8_t> overlay(4096, 0xee);
  auto wrote =
      fs.OverwriteFileData(dst.value(), 0, overlay.data(), overlay.size());
  CHECK(!wrote.ok());
  if (!wrote.ok()) {
    CHECK_EQ(wrote.error().code, XfsError::kWriteNeedsCow);
    CHECK(wrote.error().message.find("shared") != std::string::npos);
  }
  // Refused before the data write and before the timestamp transaction.
  CHECK(device->bytes() == before);

  std::vector<uint8_t> recheck(data.size(), 0);
  auto read_src = fs.ReadFile(src.value(), 0, recheck.data(), recheck.size());
  CHECK(read_src.ok());
  CHECK(recheck == data);
  std::fill(recheck.begin(), recheck.end(), static_cast<uint8_t>(0));
  auto read_dst = fs.ReadFile(dst.value(), 0, recheck.data(), recheck.size());
  CHECK(read_dst.ok());
  CHECK(recheck == data);

  // A private file on the same reflink filesystem must still take the
  // in-place path; the check may not refuse every overwrite.
  auto plain = fs.CreateRegularFile(fs.RootInode(), "plain", 0644, 0, 0);
  CHECK(plain.ok());
  if (plain.ok()) {
    CHECK(fs.WriteFileData(plain.value(), 0, data.data(), data.size()).ok());
    CHECK(fs.OverwriteFileData(plain.value(), 0, overlay.data(),
                               overlay.size())
              .ok());
  }
}

// `xfswin check` re-reads the primary superblock for live counters.  An
// unreadable primary must be reported as corruption, not silently replaced by
// the mount-time copy and reported OK.  Inode enumeration must fail too.
TEST(rw_stable_check_reports_unreadable_primary_superblock) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto device =
      std::make_unique<FailPrimarySuperblockReads>(&fixture.storage());
  auto* faults = device.get();
  MountOptions options;
  options.read_write = true;
  auto mounted = XfsFilesystem::Mount(std::move(device), options);
  CHECK(mounted.ok());
  if (!mounted.ok()) {
    fixture.Reset();
    return;
  }
  XfsFilesystem& fs = *mounted.value();
  faults->Enable();

  CheckReport report = fs.CheckConsistency(CheckOptions{});
  bool saw_superblock = false;
  for (const CheckItem& item : report.items) {
    if (item.name != "superblock") continue;
    saw_superblock = true;
    CHECK(item.status == CheckStatus::kCorrupt);
    CHECK(item.message.find("failed to read superblock") !=
          std::string::npos);
  }
  CHECK(saw_superblock);
  CHECK(report.Overall() != CheckStatus::kOk);

  auto inodes = fs.ListAllocatedInodes();
  CHECK(!inodes.ok());
  if (!inodes.ok()) CHECK_EQ(inodes.error().code, XfsError::kIoError);

  mounted.value().reset();
  fixture.Reset();
}
