// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - RW-STABLE-1 gate regressions.
#include <cstdint>
#include <memory>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/block_device.h"
#include "xfs/filesystem.h"
#include "xfs/superblock.h"

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
