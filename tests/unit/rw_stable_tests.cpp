// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - RW-STABLE-1 gate regressions.
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
