// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - tests for the read-write layer (RW-0).
//
// Scope: in-place overwrites inside existing allocated written extents.
// Two kinds of tests:
//   - pure range mapping (no device): holes, unwritten extents, EOF, flags;
//   - integration against a real, consistent image (tests/support/fixture_*):
//     device writes, cache invalidation, mount policy, crash injection config.
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/cache.h"
#include "xfs/crash.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/inode.h"
#include "xfs/log_checker.h"
#include "xfs/ondisk.h"
#include "xfs/write.h"

using namespace xfs;

namespace {

constexpr uint64_t kBlock = 4096;

XfsGeometry TestGeometry() {
  XfsGeometry geo;
  geo.block_size = 4096;
  geo.sector_size = 512;
  geo.inode_size = 512;
  geo.block_log = 12;
  geo.sector_log = 9;
  geo.inode_log = 9;
  geo.inopblog = 3;
  geo.agblklog = 10;
  geo.agcount = 1;
  geo.agblocks = 1024;
  geo.dblocks = 1024;
  geo.dir_block_size = 4096;
  geo.meta_uuid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  geo.uuid = geo.meta_uuid;
  geo.features.ftype = true;
  return geo;
}

XfsInode TestFileInode(uint64_t size) {
  XfsInode inode;
  inode.ino = 100;
  inode.mode = 0100644;
  inode.format = ondisk::inode::kFormatExtents;
  inode.aformat = ondisk::inode::kFormatExtents;
  inode.size = size;
  inode.nlink = 1;
  return inode;
}

XfsExtent Ext(uint64_t startoff, uint64_t startblock, uint64_t count,
              bool unwritten = false) {
  XfsExtent e;
  e.startoff = startoff;
  e.startblock = startblock;
  e.blockcount = count;
  e.unwritten = unwritten;
  return e;
}

}  // namespace

// ---------------------------------------------------------------------------
// Range mapping: what may be written in place
// ---------------------------------------------------------------------------

TEST(write_mapping_in_place_rules) {
  const XfsGeometry geo = TestGeometry();

  // Blocks 0, 1 and 3 written; block 2 is a hole.
  XfsInode inode = TestFileInode(4 * kBlock);
  std::vector<XfsExtent> extents = {Ext(0, 10, 1), Ext(1, 20, 1),
                                    Ext(3, 30, 1)};
  std::vector<WriteMapping> mappings;

  // Whole-block and partial writes inside a written extent.
  CHECK(MapWrittenRange(geo, inode, extents, 0, 100, &mappings).ok());
  CHECK_EQ(mappings.size(), 1u);
  CHECK_EQ(mappings[0].file_offset, 0u);
  CHECK_EQ(mappings[0].fs_block, 10u);
  CHECK_EQ(mappings[0].byte_count, 100u);

  CHECK(MapWrittenRange(geo, inode, extents, kBlock, kBlock, &mappings).ok());
  CHECK_EQ(mappings.size(), 1u);
  CHECK_EQ(mappings[0].fs_block, 20u);
  CHECK_EQ(mappings[0].byte_count, kBlock);

  CHECK(MapWrittenRange(geo, inode, extents, kBlock + 5, 10, &mappings).ok());
  CHECK_EQ(mappings.size(), 1u);
  CHECK_EQ(mappings[0].fs_block, 20u);
  CHECK_EQ(mappings[0].byte_count, 10u);

  // Exactly to EOF is allowed.
  CHECK(MapWrittenRange(geo, inode, extents, 4 * kBlock - 10, 10, &mappings)
            .ok());
  CHECK_EQ(mappings[0].byte_count, 10u);

  // Zero length is a no-op.
  CHECK(MapWrittenRange(geo, inode, extents, kBlock, 0, &mappings).ok());
  CHECK(mappings.empty());

  // A hole inside the file cannot be written in place.
  Status hole = MapWrittenRange(geo, inode, extents, 2 * kBlock, 1, &mappings);
  CHECK(!hole.ok());
  CHECK_EQ(hole.error().code, XfsError::kWriteNeedsAllocation);

  // A range that runs into a hole is refused as a whole.
  Status spanning =
      MapWrittenRange(geo, inode, extents, 2 * kBlock - 96, 200, &mappings);
  CHECK(!spanning.ok());
  CHECK_EQ(spanning.error().code, XfsError::kWriteNeedsAllocation);

  // Past EOF is refused, even by one byte.
  Status beyond =
      MapWrittenRange(geo, inode, extents, 4 * kBlock - 10, 11, &mappings);
  CHECK(!beyond.ok());
  CHECK_EQ(beyond.error().code, XfsError::kWriteExtendsFile);

  // Unwritten (preallocated) extents need a conversion transaction.
  XfsInode unwritten_inode = TestFileInode(kBlock);
  std::vector<XfsExtent> unwritten_extents = {Ext(0, 40, 1, true)};
  Status conversion =
      MapWrittenRange(geo, unwritten_inode, unwritten_extents, 100, 100,
                      &mappings);
  CHECK(!conversion.ok());
  CHECK_EQ(conversion.error().code, XfsError::kWriteNeedsConversion);

  // A fully sparse file has no writable blocks at all.
  XfsInode sparse_inode = TestFileInode(kBlock);
  Status sparse =
      MapWrittenRange(geo, sparse_inode, {}, 0, 1, &mappings);
  CHECK(!sparse.ok());
  CHECK_EQ(sparse.error().code, XfsError::kWriteNeedsAllocation);

  // Immutable and append-only files refuse writes.
  XfsInode immutable = TestFileInode(kBlock);
  immutable.flags = ondisk::inode::kDiFlagImmutable;
  Status immutable_status =
      MapWrittenRange(geo, immutable, extents, 0, 1, &mappings);
  CHECK(!immutable_status.ok());
  CHECK_EQ(immutable_status.error().code, XfsError::kPermissionDenied);

  XfsInode append_only = TestFileInode(kBlock);
  append_only.flags = ondisk::inode::kDiFlagAppend;
  CHECK(!MapWrittenRange(geo, append_only, extents, 0, 1, &mappings).ok());

  // Directories are not writable through this API.
  XfsInode dir = TestFileInode(kBlock);
  dir.mode = 0040755;
  CHECK(!MapWrittenRange(geo, dir, extents, 0, 1, &mappings).ok());
}

TEST(write_mapping_splits_at_extent_boundaries) {
  const XfsGeometry geo = TestGeometry();
  // Two adjacent extents: a single write must produce one mapping per extent.
  XfsInode inode = TestFileInode(4 * kBlock);
  std::vector<XfsExtent> extents = {Ext(0, 10, 2), Ext(2, 20, 1)};
  std::vector<WriteMapping> mappings;
  CHECK(MapWrittenRange(geo, inode, extents, 2 * kBlock - 96, 96 + kBlock,
                        &mappings)
            .ok());
  CHECK_EQ(mappings.size(), 2u);
  CHECK_EQ(mappings[0].file_offset, 2 * kBlock - 96);
  CHECK_EQ(mappings[0].fs_block, 11u);  // second block of the first extent
  CHECK_EQ(mappings[0].byte_count, 96u);
  CHECK_EQ(mappings[1].file_offset, 2 * kBlock);
  CHECK_EQ(mappings[1].fs_block, 20u);
  CHECK_EQ(mappings[1].byte_count, kBlock);

  // One byte more runs into the hole at block 3.
  Status status =
      MapWrittenRange(geo, inode, extents, 2 * kBlock, kBlock + 1, &mappings);
  CHECK(!status.ok());
  CHECK_EQ(status.error().code, XfsError::kWriteNeedsAllocation);
}

// ---------------------------------------------------------------------------
// Writable mount policy
// ---------------------------------------------------------------------------

TEST(writable_profile_allow_list) {
  XfsSuperblock sb;
  sb.versionnum = 5 | 0x8000;  // v5 with more bits
  sb.features2 = ondisk::kVersion2Crc | ondisk::kVersion2Ftype;
  sb.features_incompat = ondisk::kIncompatFtype;
  sb.features.ftype = true;
  sb.logstart = 4;
  sb.logblocks = 512;
  XfsGeometry geo = MakeGeometry(sb);
  CHECK(CheckWritableProfile(sb, geo).ok());

  // Features this build cannot keep consistent are refused, with the feature
  // named in the message.
  auto refused = [&](uint32_t incompat, uint32_t ro_compat, const char* name) {
    XfsSuperblock copy = sb;
    copy.features_incompat = incompat;
    copy.features_ro_compat = ro_compat;
    XfsGeometry g = MakeGeometry(copy);
    Status s = CheckWritableProfile(copy, g);
    CHECK(!s.ok());
    CHECK_EQ(s.error().code, XfsError::kUnsupportedFeature);
    if (name && std::string(s.error().message).find(name) ==
                    std::string::npos) {
      ::xfstest::Fail(__FILE__, __LINE__,
           std::string("message does not name '") + name +
               "': " + s.error().message);
    }
  };
  refused(ondisk::kIncompatFtype | ondisk::kIncompatParent, 0, "parent");
  refused(ondisk::kIncompatFtype | ondisk::kIncompatMetadir, 0, "metadir");
  refused(ondisk::kIncompatFtype | ondisk::kIncompatZoned, 0, "zoned");
  refused(ondisk::kIncompatFtype, ondisk::kRoCompatReflink, "reflink");
  refused(ondisk::kIncompatFtype,
          ondisk::kRoCompatRmapbt | ondisk::kRoCompatReflink, "reflink");

  // RW-6A features are part of the writable profile.
  const uint32_t modern_incompat = ondisk::kIncompatFtype |
                                   ondisk::kIncompatSpinodes |
                                   ondisk::kIncompatBigtime |
                                   ondisk::kIncompatNrext64;
  const uint32_t modern_ro =
      ondisk::kRoCompatFinobt | ondisk::kRoCompatInobtcnt;
  {
    XfsSuperblock modern = sb;
    modern.features_incompat = modern_incompat;
    modern.features_ro_compat = modern_ro;
    modern.spino_align = 2;
    modern.inoalignmt = 8;
    modern.blocksize = 4096;
    modern.inodesize = 512;
    modern.inopblock = 8;
    modern.inopblog = 3;
    modern.blocklog = 12;
    modern.inodelog = 9;
    XfsGeometry g = MakeGeometry(modern);
    CHECK(CheckWritableProfile(modern, g).ok());
  }

  // RW-6B: rmapbt=1,reflink=0 is writable through the ordinary profile gate.
  XfsSuperblock rmap = sb;
  rmap.features_ro_compat |= ondisk::kRoCompatRmapbt;
  rmap.features_ro_compat &= ~ondisk::kRoCompatReflink;
  rmap.features.rmapbt = true;
  rmap.features.reflink = false;
  CHECK(CheckWritableProfile(rmap, MakeGeometry(rmap)).ok());
  // Reflink and the rmapbt+reflink combination stay refused.
  XfsSuperblock reflink = sb;
  reflink.features_ro_compat |= ondisk::kRoCompatReflink;
  reflink.features.reflink = true;
  CHECK(!CheckWritableProfile(reflink, MakeGeometry(reflink)).ok());
  XfsSuperblock both = rmap;
  both.features_ro_compat |= ondisk::kRoCompatReflink;
  both.features.reflink = true;
  CHECK(!CheckWritableProfile(both, MakeGeometry(both)).ok());

  // Features with no on-disk impact for our mutations are allowed.
  XfsSuperblock allowed = sb;
  allowed.features_incompat = ondisk::kIncompatFtype |
                              ondisk::kIncompatMetaUuid |
                              ondisk::kIncompatExchrange;
  allowed.features.meta_uuid = true;
  CHECK(CheckWritableProfile(allowed, MakeGeometry(allowed)).ok());

  // "No quota inode" is encoded as 0 (mkfs) or as NULLFSINO, all ones (what a
  // mounted kernel writes back); neither means quota is enabled.
  XfsSuperblock no_quota = sb;
  no_quota.uquotino = ~0ull;
  no_quota.gquotino = ~0ull;
  no_quota.pquotino = ~0ull;
  CHECK(CheckWritableProfile(no_quota, MakeGeometry(no_quota)).ok());
  XfsSuperblock quota_inode = sb;
  quota_inode.uquotino = 70;
  CHECK(!CheckWritableProfile(quota_inode, MakeGeometry(quota_inode)).ok());

  // Geometry-level refusals.
  struct Case {
    const char* what;
    void (*apply)(XfsSuperblock&);
  };
  const Case cases[] = {
      {"realtime",
       [](XfsSuperblock& s) {
         s.rblocks = 1024;
         s.rextents = 8;
       }},
      {"quota", [](XfsSuperblock& s) { s.qflags = 0x0001; }},
      {"directory block", [](XfsSuperblock& s) { s.dirblklog = 1; }},      {"external log", [](XfsSuperblock& s) { s.logstart = 0; }},
      {"log-incompat",
       [](XfsSuperblock& s) { s.features_log_incompat = 0x1; }},
      {"asciici", [](XfsSuperblock& s) { s.features.asciici = true; }},
  };
  for (const auto& test : cases) {
    XfsSuperblock copy = sb;
    test.apply(copy);
    Status s = CheckWritableProfile(copy, MakeGeometry(copy));
    CHECK(!s.ok());
    if (!s.ok()) {
      if (std::string(s.error().message).find(test.what) ==
          std::string::npos) {
        ::xfstest::Fail(__FILE__, __LINE__, std::string("profile refusal does not "
                                             "mention '") +
                                    test.what + "': " + s.error().message);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Integration: fixture image with one allocated file
// ---------------------------------------------------------------------------

TEST(fixture_image_reads_back) {
  auto& fixture = xfstest::FixtureImage::Instance();
  auto fs = fixture.Mount(/*read_write=*/false);
  CHECK(fs.ok());
  if (!fs.ok()) return;

  CheckReport report = fs.value()->CheckConsistency(CheckOptions{});
  if (report.Overall() != CheckStatus::kOk) {
    for (const auto& item : report.items) {
      std::printf("fixture check item %s: %s %s\n", item.name.c_str(),
                  ToString(item.status), item.message.c_str());
    }
  }
  CHECK(report.Overall() == CheckStatus::kOk);

  auto inode = fs.value()->LookupPath("/" + fixture.FileName());
  CHECK(inode.ok());
  if (!inode.ok()) return;
  CHECK_EQ(inode.value().ino, fixture.info().file_ino);
  CHECK_EQ(inode.value().size, fixture.info().file_size);
  CHECK_EQ(inode.value().nextents, 1u);

  std::vector<uint8_t> data(static_cast<size_t>(fixture.info().file_size));
  auto read = fs.value()->ReadFile(inode.value().ino, 0, data.data(),
                                  data.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    if (data[i] != xfstest::FixturePatternByte(i)) {
      ::xfstest::Fail(__FILE__, __LINE__, "fixture data mismatch at offset " +
                                   std::to_string(i));
    }
  }
}

TEST(write_overwrites_in_place_and_persists) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t file_ino = fixture.info().file_ino;
  const uint64_t file_size = fixture.info().file_size;

  // Bytes to overwrite: a partial block in the middle and the last block tail.
  const uint64_t offset = 5000;
  std::vector<uint8_t> overlay(300);
  for (size_t i = 0; i < overlay.size(); ++i) overlay[i] = 0xE0 + (i & 0x0F);

  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(fs.value()->writable());
    auto before = fs.value()->GetFileInfo(file_ino);
    CHECK(before.ok());
    auto written = fs.value()->OverwriteFileData(file_ino, offset,
                                                 overlay.data(), overlay.size());
    CHECK(written.ok());
    CHECK_EQ(written.value(), overlay.size());
    // RW-1: a successful data write stamps mtime/ctime through a metadata
    // transaction (the data itself is not journalled).
    auto after = fs.value()->GetFileInfo(file_ino);
    CHECK(after.ok());
    CHECK_EQ(after.value().size, file_size);
    if (before.ok() && after.ok()) {
      if (after.value().mtime.sec == before.value().mtime.sec &&
          after.value().mtime.nsec == before.value().mtime.nsec) {
        ::xfstest::Fail(__FILE__, __LINE__, "mtime was not updated");
      }
    }
    // Unmount properly: the log must end with a clean-unmount record, otherwise
    // the next mount sees a dirty journal.
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }

  // The data must be on the device itself: check the bytes through the
  // geometry, not through the (invalidated) cache.
  const auto& geo = fixture.storage().Info();
  (void)geo;
  {
    // Fresh mount (new caches): the new bytes must come from the image.
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    std::vector<uint8_t> data(static_cast<size_t>(file_size));
    auto read = fs.value()->ReadFile(file_ino, 0, data.data(), data.size());
    CHECK(read.ok());
    CHECK_EQ(read.value(), data.size());
    for (size_t i = 0; i < data.size(); ++i) {
      const uint8_t expected = (i >= offset && i < offset + overlay.size())
                                   ? overlay[i - offset]
                                   : xfstest::FixturePatternByte(i);
      if (data[i] != expected) {
        ::xfstest::Fail(__FILE__, __LINE__, "post-write data mismatch at offset " +
                                     std::to_string(i));
      }
    }
    // Size and block counts are unchanged: only the timestamps were logged.
    auto info = fs.value()->GetFileInfo(file_ino);
    CHECK(info.ok());
    CHECK_EQ(info.value().size, file_size);
  }
}

TEST(write_updates_only_inode_and_log) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const auto& pristine = fixture.pristine();
  const uint64_t file_ino = fixture.info().file_ino;
  auto* memory = dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  if (!memory) return;

  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  const XfsGeometry& geo = fs.value()->geometry();
  const uint64_t inode_offset = geo.InoToByteOffset(file_ino);
  std::vector<uint8_t> overlay(4096, 0x5A);
  auto written = fs.value()->OverwriteFileData(file_ino, 8192, overlay.data(),
                                               overlay.size());
  CHECK(written.ok());
  CHECK(fs.value()->Flush().ok());

  // RW-1: exactly the file data, its inode and the log may change. Everything
  // else - AG headers, btree roots, the other inode slots, the superblock
  // copies - must be byte-identical to the pristine image, because it can only
  // be modified through a transaction that this write did not run.
  const uint64_t data_start = geo.FsbToByte(fixture.info().data_fsb);
  const uint64_t data_end =
      data_start + fixture.info().data_blocks * geo.block_size;
  const uint64_t log_start = geo.FsbToByte(geo.logstart);
  const uint64_t log_end = log_start + 64 * 1024;
  auto allowed_change = [&](uint64_t offset) {
    if (offset >= data_start && offset < data_end) return true;      // data
    if (offset >= inode_offset && offset < inode_offset + geo.inode_size)
      return true;                                                   // inode
    // Log: the mkfs clean record (blocks 0-1) must stay untouched; our
    // transaction record starts at block 2.
    if (offset >= log_start + 1024 && offset < log_end) return true;
    return false;
  };
  for (uint64_t offset = 0; offset + 512 <= pristine.size(); offset += 512) {
    if (std::memcmp(memory->bytes().data() + offset, pristine.data() + offset,
                    static_cast<size_t>(512)) == 0) {
      continue;
    }
    if (!allowed_change(offset)) {
      ::xfstest::Fail(__FILE__, __LINE__,
                      "unexpected change outside data/inode/log at device "
                      "offset " + std::to_string(offset));
      break;
    }
  }
  // The mkfs clean-unmount record itself and every superblock copy are intact.
  auto expect_same = [&](uint64_t offset, uint64_t length, const char* what) {
    if (std::memcmp(memory->bytes().data() + offset, pristine.data() + offset,
                    static_cast<size_t>(length)) != 0) {
      ::xfstest::Fail(__FILE__, __LINE__,
           std::string("metadata region changed: ") + what);
    }
  };
  expect_same(log_start, 1024, "mkfs clean log record");
  for (uint32_t ag = 0; ag < geo.agcount; ++ag) {
    expect_same(geo.AgByteOffset(ag), geo.sector_size, "superblock copy");
  }
  // The inode itself did change (times, di_lsn, CRC), and it must still parse.
  CHECK(std::memcmp(memory->bytes().data() + inode_offset,
                    pristine.data() + inode_offset,
                    static_cast<size_t>(geo.inode_size)) != 0);
  auto reparsed = fs.value()->OpenInode(file_ino);
  CHECK(reparsed.ok());
  if (reparsed.ok()) {
    CHECK_EQ(reparsed.value().size, fixture.info().file_size);
    CHECK_EQ(reparsed.value().nextents, 1u);
  }

  // ... while the file data did change on the device.
  std::vector<XfsExtent> extents;
  CHECK(LoadDataForkExtents(*memory, geo, reparsed.value(), &extents).ok());
  CHECK_EQ(extents.size(), 1u);
  const uint64_t device_offset = geo.FsbToByte(extents[0].startblock) + 8192;
  CHECK_EQ(memory->bytes()[device_offset], 0x5A);
  fixture.Reset();
}

TEST(write_invalidates_affected_data_chunks_only) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t file_ino = fixture.info().file_ino;
  const uint64_t file_size = fixture.info().file_size;
  CHECK(file_size > 8192);

  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) return;

  // Fill the chunk cache with the original contents, and repeat a few times so
  // that the read path really serves from the cache.
  {
    std::vector<uint8_t> data(static_cast<size_t>(file_size));
    for (int i = 0; i < 3; ++i) {
      auto read = fs.value()->ReadFile(file_ino, 0, data.data(), data.size());
      CHECK(read.ok());
      CHECK_EQ(data[100], xfstest::FixturePatternByte(100));
    }
  }

  std::vector<uint8_t> overlay(64, 0x77);
  auto written = fs.value()->OverwriteFileData(file_ino, 64, overlay.data(),
                                               overlay.size());
  CHECK(written.ok());

  // A cached read must not return the stale bytes.
  std::vector<uint8_t> data(static_cast<size_t>(file_size));
  auto read = fs.value()->ReadFile(file_ino, 0, data.data(), data.size());
  CHECK(read.ok());
  for (size_t i = 0; i < overlay.size(); ++i) {
    CHECK_EQ(data[64 + i], overlay[i]);
  }
  // Untouched parts of the same chunk still match the fixture pattern.
  for (size_t i = 0; i < 64; ++i) {
    CHECK_EQ(data[i], xfstest::FixturePatternByte(i));
  }
  for (size_t i = 64 + overlay.size(); i < 4096; ++i) {
    CHECK_EQ(data[i], xfstest::FixturePatternByte(i));
  }

  // Invalidating a sub-range must not drop unrelated inodes' chunks: read the
  // fixture file again (cached) and check it still matches.
  for (int pass = 0; pass < 2; ++pass) {
    auto again = fs.value()->ReadFile(file_ino, 4096, data.data(), 4096);
    CHECK(again.ok());
    for (size_t i = 0; i < 4096; ++i) {
      CHECK_EQ(data[i], xfstest::FixturePatternByte(4096 + i));
    }
  }
}

TEST(write_rejected_on_read_only_mount) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto fs = fixture.Mount(/*read_write=*/false);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  CHECK(!fs.value()->writable());
  uint8_t byte = 0x11;
  auto written = fs.value()->OverwriteFileData(fixture.info().file_ino, 0,
                                               &byte, 1);
  CHECK(!written.ok());
  CHECK_EQ(written.error().code, XfsError::kReadOnlyVolume);
}

TEST(write_rejects_beyond_eof_without_touching_data) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t file_ino = fixture.info().file_ino;
  const uint64_t file_size = fixture.info().file_size;
  const auto& pristine = fixture.pristine();

  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  std::vector<uint8_t> overlay(64, 0x33);
  auto written = fs.value()->OverwriteFileData(file_ino, file_size - 32,
                                               overlay.data(), overlay.size());
  CHECK(!written.ok());
  CHECK_EQ(written.error().code, XfsError::kWriteExtendsFile);

  // Nothing may have been written: the whole file region must be untouched.
  auto* memory =
      dynamic_cast<xfstest::MemoryBlockDevice*>(&fixture.storage());
  CHECK(memory != nullptr);
  const XfsGeometry& geo = fs.value()->geometry();
  auto inode = fs.value()->OpenInode(file_ino);
  CHECK(inode.ok());
  std::vector<XfsExtent> extents;
  CHECK(LoadDataForkExtents(*memory, geo, inode.value(), &extents).ok());
  for (const auto& extent : extents) {
    const uint64_t offset = geo.FsbToByte(extent.startblock);
    const uint64_t length = extent.blockcount * geo.block_size;
    if (std::memcmp(memory->bytes().data() + offset,
                    pristine.data() + offset, length) != 0) {
      ::xfstest::Fail(__FILE__, __LINE__,
           "rejected write modified the file's data blocks");
    }
  }
}

TEST(mount_refuses_read_write_on_dirty_log) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();

  // Read-only mount to find the log location, then scribble on the log so it
  // can no longer be recognised as clean.
  uint64_t log_offset = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    log_offset = fs.value()->geometry().FsbToByte(
        fs.value()->geometry().logstart);
  }
  std::vector<uint8_t> junk(512, 0xAB);
  CHECK(fixture.storage().Write(log_offset, junk.data(), junk.size()));

  auto ro = fixture.Mount(/*read_write=*/false);
  CHECK(!ro.ok());  // unknown log content refuses even a read-only mount
  if (!ro.ok()) CHECK_EQ(ro.error().code, XfsError::kDirtyLog);

  auto forced = [&] {
    xfs::MountOptions options;
    options.force_unsafe_ro = true;
    auto borrowed = std::make_unique<xfstest::BorrowedBlockDevice>(
        &fixture.storage());
    return XfsFilesystem::Mount(std::move(borrowed), options);
  }();
  CHECK(forced.ok());

  auto rw = fixture.Mount(/*read_write=*/true);
  CHECK(!rw.ok());
  if (!rw.ok()) CHECK_EQ(rw.error().code, XfsError::kDirtyLog);

  fixture.Reset();
}

TEST(mount_refuses_read_write_on_read_only_device) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  auto borrowed =
      std::make_unique<xfstest::BorrowedBlockDevice>(&fixture.storage());
  borrowed->SetWritable(false);
  MountOptions options;
  options.read_write = true;
  auto fs = XfsFilesystem::Mount(std::move(borrowed), options);
  CHECK(!fs.ok());
  if (!fs.ok()) CHECK_EQ(fs.error().code, XfsError::kReadOnlyVolume);
}

// ---------------------------------------------------------------------------
// Crash injection configuration
// ---------------------------------------------------------------------------

TEST(crash_injection_config_parsing) {
  auto set_env = [](const char* value) {
#ifdef _WIN32
    _putenv_s("XFSWIN_CRASH_AT", value);
#else
    if (value && *value) {
      setenv("XFSWIN_CRASH_AT", value, 1);
    } else {
      unsetenv("XFSWIN_CRASH_AT");
    }
#endif
    ReloadCrashConfig();
  };
  set_env("");
  CHECK(!ShouldCrash(CrashPoint::kBeforeDataWrite));

  set_env("before_data_write");
  CrashConfig config = GetCrashConfig();
  CHECK(config.enabled);
  CHECK(config.point == CrashPoint::kBeforeDataWrite);
  CHECK_EQ(config.hit, 1u);
  // Armed for one occurrence only.
  CHECK(ShouldCrash(CrashPoint::kBeforeDataWrite));
  CHECK(!ShouldCrash(CrashPoint::kBeforeDataWrite));
  CHECK(!ShouldCrash(CrashPoint::kAfterDataWrite));

  set_env("after_log_commit:2");
  CHECK(!ShouldCrash(CrashPoint::kAfterLogCommit));
  CHECK(ShouldCrash(CrashPoint::kAfterLogCommit));

  set_env("not_a_point");
  CHECK(!GetCrashConfig().enabled);
  CHECK(!ShouldCrash(CrashPoint::kBeforeDataWrite));

  // Point names are stable (scripts and CI refer to them).
  CrashPoint parsed = CrashPoint::kCount;
  CHECK(ParseCrashPoint("before_metadata_write", &parsed));
  CHECK(parsed == CrashPoint::kBeforeMetadataWrite);
  CHECK(ParseCrashPoint("after_device_flush", &parsed));
  CHECK(parsed == CrashPoint::kAfterDeviceFlush);
  CHECK(!ParseCrashPoint("bogus", &parsed));
  set_env("");
}

// ---------------------------------------------------------------------------
// Cache invalidation plumbing
// ---------------------------------------------------------------------------

TEST(cache_invalidation_builders) {
  CacheInvalidation inode = CacheInvalidation::Inode(7);
  CHECK(inode.Has(CacheInvalidation::kInode));
  CHECK(inode.Has(CacheInvalidation::kInodeChunk));
  CHECK(inode.Has(CacheInvalidation::kFileInfo));
  CHECK(!inode.Has(CacheInvalidation::kData));
  CHECK_EQ(inode.ino, 7u);

  CacheInvalidation extents = CacheInvalidation::Extents(7);
  CHECK(extents.Has(CacheInvalidation::kExtents));
  CHECK(extents.Has(CacheInvalidation::kData));
  CHECK(extents.Has(CacheInvalidation::kInode));

  CacheInvalidation range = CacheInvalidation::DataRange(9, 100, 50);
  CHECK(range.Has(CacheInvalidation::kData));
  CHECK(!range.Has(CacheInvalidation::kInode));
  CHECK_EQ(range.data_offset, 100u);
  CHECK_EQ(range.data_length, 50u);

  CacheInvalidation dir = CacheInvalidation::Directory(3);
  CHECK(dir.Has(CacheInvalidation::kDirectory));
  CHECK(dir.Has(CacheInvalidation::kInode));

  CHECK(CacheInvalidation::Everything().Has(CacheInvalidation::kAll));
}
