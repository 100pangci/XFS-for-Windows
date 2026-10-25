// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - tests for RW-2: block allocator, extent map mutation,
// file extension and truncate.
//
// The tests use the fixture image (one regular file with one allocated
// extent) and verify the *on-disk* result after a fresh remount: size, data,
// extent map, free space accounting. CI then runs xfs_repair -n and mounts the
// same operations with a real Linux kernel (tests/scripts/verify_rw2_linux.sh).
#include <cstring>
#include <string>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/ag.h"
#include "xfs/btree.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"

using namespace xfs;

namespace {

constexpr uint64_t kBlock = 4096;

uint64_t ReadFdblocks(xfstest::FixtureImage& fixture) {
  auto fs = fixture.Mount(/*read_write=*/false);
  CHECK(fs.ok());
  if (!fs.ok()) return 0;
  std::vector<uint8_t> sector(fs.value()->geometry().sector_size, 0);
  CHECK(fixture.storage().Read(0, sector.data(), sector.size()));
  return GetBE64(sector.data(), ondisk::sb::kFdblocks);
}

void CheckConsistent(xfstest::FixtureImage& fixture) {
  auto fs = fixture.Mount(/*read_write=*/false);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  CheckReport report = fs.value()->CheckConsistency(CheckOptions{});
  if (report.Overall() != CheckStatus::kOk) {
    for (const auto& item : report.items) {
      std::printf("  check: %s -> %s %s\n", item.name.c_str(),
                  ToString(item.status), item.message.c_str());
    }
  }
  CHECK(report.Overall() == CheckStatus::kOk);
}

std::vector<uint8_t> Pattern(uint64_t offset, size_t length, uint8_t seed) {
  std::vector<uint8_t> out(length);
  for (size_t i = 0; i < length; ++i) {
    out[i] = static_cast<uint8_t>(((offset + i) * 37u) ^ (seed * 11u) ^
                                  ((offset + i) >> 5));
  }
  return out;
}

// Collects the free space extents of AG0 straight from the bnobt, independent
// of the allocator implementation.
std::vector<XfsExtent> ReadAg0FreeExtents(xfstest::FixtureImage& fixture) {
  std::vector<XfsExtent> out;
  auto fs = fixture.Mount(/*read_write=*/false);
  CHECK(fs.ok());
  if (!fs.ok()) return out;
  const XfsGeometry& geo = fs.value()->geometry();
  auto agf = ReadAgf(fixture.storage(), geo, 0);
  CHECK(agf.ok());
  if (!agf.ok()) return out;
  BtreeSpec spec;
  spec.name = "bnobt";
  spec.format = BtreeFormat::kShort;
  spec.block_size = geo.block_size;
  spec.key_size = ondisk::btree::kAllocKeySize;
  spec.ptr_size = ondisk::btree::kAllocPtrSize;
  spec.rec_size = ondisk::btree::kAllocRecSize;
  spec.magic = ondisk::kBtbCrcMagic;
  spec.null_ptr = ondisk::btree::kNullAgBlock;
  spec.expected_owner = 0;
  spec.owner_size = 4;
  spec.uuid = geo.meta_uuid;
  spec.ptr_to_daddr = [&](uint64_t agbno, uint64_t* daddr) {
    if (agbno >= agf.value().length) return false;
    *daddr = geo.FsbToByte(geo.AgFsb(0, static_cast<uint32_t>(agbno))) / 512;
    return true;
  };
  BtreeWalkCallbacks cb;
  cb.read_block = [&](uint64_t fsb, std::vector<uint8_t>* buf) {
    buf->resize(geo.block_size);
    return fixture.storage().Read(geo.FsbToByte(fsb), buf->data(), buf->size());
  };
  cb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.value().length; };
  cb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                 const uint8_t* buf) -> Status {
    if (meta.level != 0) return OkStatus();
    for (uint16_t i = 0; i < meta.numrecs; ++i) {
      const uint8_t* rec = BtreeLeafRec(buf, meta, spec, i);
      XfsExtent e;
      e.startoff = GetBE32(rec);
      e.startblock = e.startoff;
      e.blockcount = GetBE32(rec + 4);
      out.push_back(e);
    }
    return OkStatus();
  };
  CHECK(WalkBtree(spec, agf.value().bno_root, cb).ok());
  return out;
}

void ExpectContent(xfstest::FixtureImage& fixture, uint64_t ino,
                   uint64_t size, uint64_t offset, const std::vector<uint8_t>& data) {
  auto fs = fixture.Mount(/*read_write=*/false);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  auto inode = fs.value()->OpenInode(ino);
  CHECK(inode.ok());
  if (!inode.ok()) return;
  CHECK_EQ(inode.value().size, size);
  std::vector<uint8_t> got(static_cast<size_t>(size));
  auto read = fs.value()->ReadFile(ino, 0, got.data(), got.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), got.size());
  for (size_t i = 0; i < data.size(); ++i) {
    const uint64_t pos = offset + i;
    if (pos >= size) break;
    if (got[static_cast<size_t>(pos)] != data[i]) {
      ::xfstest::Fail(__FILE__, __LINE__,
                      "content mismatch at byte " + std::to_string(pos));
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Write past EOF: one block, then a multi-block append.
// ---------------------------------------------------------------------------

TEST(rw2_fallocate_then_convert_is_zero_safe) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(fs.value()->SetFileSize(ino, 0).ok());
    auto prealloc = fs.value()->AllocateUnwritten(ino, 0, 3 * kBlock);
    CHECK(prealloc.ok());
    if (prealloc.ok()) CHECK_EQ(prealloc.value().size, 3 * kBlock);
    auto extents = fs.value()->GetDataExtents(ino);
    CHECK(extents.ok());
    if (extents.ok()) {
      CHECK_EQ(extents.value().size(), 1u);
      if (!extents.value().empty()) {
        CHECK(extents.value()[0].unwritten);
        CHECK_EQ(extents.value()[0].blockcount, 3u);
      }
    }
    // Conversion must zero-fill the range before exposing it as written.
    auto converted = fs.value()->ConvertUnwritten(ino, 0, 2 * kBlock);
    CHECK(converted.ok());
    extents = fs.value()->GetDataExtents(ino);
    CHECK(extents.ok());
    if (extents.ok()) {
      CHECK_EQ(extents.value().size(), 2u);
      if (extents.value().size() == 2) {
        CHECK(!extents.value()[0].unwritten);
        CHECK_EQ(extents.value()[0].blockcount, 2u);
        CHECK(extents.value()[1].unwritten);
      }
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    std::vector<uint8_t> data(3 * kBlock, 0xAA);
    auto read = fs.value()->ReadFile(ino, 0, data.data(), data.size());
    CHECK(read.ok());
    for (uint8_t byte : data) CHECK(byte == 0);
  }
  CheckConsistent(fixture);
}

TEST(rw2_convert_range_inside_one_extent_keeps_tail) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(fs.value()->SetFileSize(ino, 0).ok());
    auto prealloc = fs.value()->AllocateUnwritten(ino, 0, 4 * kBlock);
    CHECK(prealloc.ok());
    // Removing the converted middle range must leave both the head and the
    // tail of the original unwritten extent in place.
    auto converted = fs.value()->ConvertUnwritten(ino, kBlock, 2 * kBlock);
    CHECK(converted.ok());
    auto extents = fs.value()->GetDataExtents(ino);
    CHECK(extents.ok());
    if (extents.ok()) {
      CHECK_EQ(extents.value().size(), 3u);
      if (extents.value().size() == 3) {
        CHECK(extents.value()[0].unwritten);
        CHECK_EQ(extents.value()[0].startoff, 0u);
        CHECK_EQ(extents.value()[0].blockcount, 1u);
        CHECK(!extents.value()[1].unwritten);
        CHECK_EQ(extents.value()[1].startoff, 1u);
        CHECK_EQ(extents.value()[1].blockcount, 2u);
        CHECK(extents.value()[2].unwritten);
        CHECK_EQ(extents.value()[2].startoff, 3u);
        CHECK_EQ(extents.value()[2].blockcount, 1u);
      }
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
  }
  CheckConsistent(fixture);
}

TEST(rw2_append_one_block) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  const uint64_t old_size = fixture.info().file_size;
  const uint64_t fd_before = ReadFdblocks(fixture);

  const std::vector<uint8_t> payload = Pattern(old_size, kBlock, 0x21);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto written = fs.value()->WriteFileData(ino, old_size, payload.data(),
                                             payload.size());
    if (!written.ok()) {
      std::printf("write error: %s\n", written.error().Format().c_str());
    }
    CHECK_EQ(written.value(), payload.size());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  ExpectContent(fixture, ino, old_size + kBlock, old_size, payload);
  // One data block was allocated.
  CHECK_EQ(ReadFdblocks(fixture), fd_before - 1);
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw2_append_multiple_blocks) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  const uint64_t old_size = fixture.info().file_size;
  const uint64_t fd_before = ReadFdblocks(fixture);
  const size_t length = 5 * kBlock + 123;

  const std::vector<uint8_t> payload = Pattern(old_size, length, 0x37);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto written = fs.value()->WriteFileData(ino, old_size, payload.data(),
                                             payload.size());
    if (!written.ok()) {
      std::printf("write error: %s\n", written.error().Format().c_str());
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  ExpectContent(fixture, ino, old_size + length, old_size, payload);
  CHECK_EQ(ReadFdblocks(fixture), fd_before - 6);
  CheckConsistent(fixture);
  fixture.Reset();
}

// Append handles (WinFsp write_to_end): the offset is resolved inside the
// mutation lock, so consecutive appends must land at the current EOF.
TEST(rw2_append_resolves_offset_at_eof) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  const uint64_t old_size = fixture.info().file_size;

  const std::vector<uint8_t> first = Pattern(old_size, 3 * kBlock, 0x41);
  const std::vector<uint8_t> second =
      Pattern(old_size + first.size(), 2 * kBlock + 11, 0x62);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto written = fs.value()->WriteFileData(ino, 0, first.data(),
                                             first.size(), /*append=*/true);
    if (!written.ok()) {
      std::printf("append error: %s\n", written.error().Format().c_str());
    }
    CHECK_EQ(written.value(), first.size());
    written = fs.value()->WriteFileData(ino, 0, second.data(), second.size(),
                                        /*append=*/true);
    CHECK_EQ(written.value(), second.size());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  // The second append starts where the first one ended.
  ExpectContent(fixture, ino, old_size + first.size() + second.size(),
                old_size, first);
  ExpectContent(fixture, ino, old_size + first.size() + second.size(),
                old_size + first.size(), second);
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Sparse extension: the gap is a hole, only the written range is allocated.
// ---------------------------------------------------------------------------

TEST(rw2_sparse_extension_keeps_a_hole) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  const uint64_t old_size = fixture.info().file_size;
  const uint64_t offset = old_size + 3 * kBlock;
  const uint64_t fd_before = ReadFdblocks(fixture);
  const std::vector<uint8_t> payload = Pattern(offset, kBlock + 7, 0x5B);

  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto written = fs.value()->WriteFileData(ino, offset, payload.data(),
                                             payload.size());
    if (!written.ok()) {
      std::printf("write error: %s\n", written.error().Format().c_str());
      return;
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  ExpectContent(fixture, ino, offset + payload.size(), offset, payload);
  // 2 blocks (the range spans two blocks), the 3-block gap stays a hole.
  CHECK_EQ(ReadFdblocks(fixture), fd_before - 2);
  {
    // The gap must read as zeros.
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    std::vector<uint8_t> gap(3 * kBlock, 0xAA);
    auto read = fs.value()->ReadFile(ino, old_size, gap.data(), gap.size());
    CHECK(read.ok());
    CHECK_EQ(read.value(), gap.size());
    for (uint8_t b : gap) CHECK_EQ(b, 0);
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Write into a hole inside the file (no size change).
// ---------------------------------------------------------------------------

TEST(rw2_write_into_hole_inside_file) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  const uint64_t old_size = fixture.info().file_size;
  const uint64_t fd_before = ReadFdblocks(fixture);

  // Punch a hole in the fixture file: shrink to 8 KiB, grow back sparsely,
  // then write inside the hole.
  const uint64_t hole_offset = 2 * kBlock;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto info = fs.value()->SetFileSize(ino, 2 * kBlock);
    CHECK(info.ok());
    if (!info.ok()) {
      std::printf("truncate error: %s\n", info.error().Format().c_str());
      return;
    }
    info = fs.value()->SetFileSize(ino, old_size);
    CHECK(info.ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  // The truncated blocks were freed (6 of the 8 blocks).
  const uint64_t fd_after_truncate = ReadFdblocks(fixture);
  CHECK_EQ(fd_after_truncate, fd_before + 6);
  const std::vector<uint8_t> payload = Pattern(hole_offset, kBlock, 0x71);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto written = fs.value()->WriteFileData(ino, hole_offset, payload.data(),
                                             payload.size());
    if (!written.ok()) {
      std::printf("write error: %s\n", written.error().Format().c_str());
      return;
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  ExpectContent(fixture, ino, old_size, hole_offset, payload);
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Truncate: shrink, grow, shrink to zero.
// ---------------------------------------------------------------------------

TEST(rw2_truncate_shrinks_and_frees) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  const uint64_t old_size = fixture.info().file_size;
  const uint64_t fd_before = ReadFdblocks(fixture);
  CHECK_EQ(old_size, 32768u);  // the expectations below assume eight 4 KiB blocks

  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto info = fs.value()->SetFileSize(ino, 5000);
    CHECK(info.ok());
    if (!info.ok()) {
      std::printf("truncate error: %s\n", info.error().Format().c_str());
      return;
    }
    CHECK_EQ(info.value().size, 5000u);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  // A 32 KiB file becomes 5000 bytes: blocks 2..7 are freed, blocks 0..1 stay.
  CHECK_EQ(ReadFdblocks(fixture), fd_before + 6);
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    std::vector<uint8_t> data(6000, 0xCC);
    auto read = fs.value()->ReadFile(ino, 0, data.data(), data.size());
    CHECK(read.ok());
    CHECK_EQ(read.value(), 5000u);
    for (size_t i = 0; i < 4096; ++i) {
      CHECK_EQ(data[i], xfstest::FixturePatternByte(i));
    }
    for (size_t i = 4096; i < 5000; ++i) {
      CHECK_EQ(data[i], xfstest::FixturePatternByte(i));
    }
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw2_truncate_grows_sparsely) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  const uint64_t old_size = fixture.info().file_size;
  const uint64_t fd_before = ReadFdblocks(fixture);

  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto info = fs.value()->SetFileSize(ino, old_size + 10 * kBlock);
    CHECK(info.ok());
    if (!info.ok()) {
      std::printf("truncate error: %s\n", info.error().Format().c_str());
      return;
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  // Growth does not allocate.
  CHECK_EQ(ReadFdblocks(fixture), fd_before);
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    std::vector<uint8_t> tail(10 * kBlock, 0xAA);
    auto read = fs.value()->ReadFile(ino, old_size, tail.data(), tail.size());
    CHECK(read.ok());
    CHECK_EQ(read.value(), tail.size());
    for (uint8_t b : tail) CHECK_EQ(b, 0);
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw2_truncate_to_zero_frees_everything) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  const uint64_t fd_before = ReadFdblocks(fixture);

  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto info = fs.value()->SetFileSize(ino, 0);
    CHECK(info.ok());
    if (!info.ok()) return;
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CHECK_EQ(ReadFdblocks(fixture), fd_before + 8);
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto inode = fs.value()->OpenInode(ino);
    CHECK(inode.ok());
    if (inode.ok()) {
      CHECK_EQ(inode.value().size, 0u);
      CHECK_EQ(inode.value().nextents, 0u);
      CHECK_EQ(inode.value().nblocks, 0u);
    }
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Reuse: the blocks a truncate frees are handed out again.
// ---------------------------------------------------------------------------

TEST(rw2_freed_blocks_are_reused) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  const uint64_t data_fsb = fixture.info().data_fsb;
  const uint64_t data_blocks = fixture.info().data_blocks;
  const uint64_t fd_before = ReadFdblocks(fixture);

  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto info = fs.value()->SetFileSize(ino, 0);
    CHECK(info.ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CHECK_EQ(ReadFdblocks(fixture), fd_before + data_blocks);

  // The freed blocks merged back into the surrounding free extent, and a later
  // allocation reuses blocks from that extent.
  const std::vector<XfsExtent> free_after_truncate = ReadAg0FreeExtents(fixture);
  const XfsExtent* merged = nullptr;
  for (const XfsExtent& e : free_after_truncate) {
    if (e.startoff <= data_fsb && data_fsb < e.startoff + e.blockcount) {
      merged = &e;
      break;
    }
  }
  CHECK(merged != nullptr);
  if (merged == nullptr) return;
  CHECK(merged->blockcount >= data_blocks);

  const std::vector<uint8_t> payload = Pattern(0, 3 * kBlock, 0x13);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto written = fs.value()->WriteFileData(ino, 0, payload.data(),
                                             payload.size());
    CHECK(written.ok());
    if (!written.ok()) return;
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  // The new extent is carved out of the extent that held the truncated blocks.
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto inode = fs.value()->OpenInode(ino);
    CHECK(inode.ok());
    if (!inode.ok()) return;
    std::vector<XfsExtent> extents;
    CHECK(LoadDataForkExtents(fixture.storage(), fs.value()->geometry(),
                              inode.value(), &extents)
              .ok());
    CHECK_EQ(extents.size(), 1u);
    CHECK(extents[0].startblock >= merged->startoff);
    CHECK(extents[0].startblock + extents[0].blockcount <=
          merged->startoff + merged->blockcount);
    CHECK_EQ(extents[0].blockcount, 3u);
  }
  CHECK_EQ(ReadFdblocks(fixture), fd_before + data_blocks - 3);
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Extent map: many extents force the btree format, truncate collapses it.
// ---------------------------------------------------------------------------

TEST(rw2_extent_map_converts_to_btree_and_back) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  const uint64_t old_size = fixture.info().file_size;
  // Build 30 extents: one block every two blocks (holes in between) beyond the
  // fixture file, so nothing merges.
  const size_t count = 30;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    for (size_t i = 0; i < count; ++i) {
      const uint64_t offset = old_size + i * 2 * kBlock;
      const std::vector<uint8_t> payload = Pattern(offset, kBlock, 0x40);
      auto written = fs.value()->WriteFileData(ino, offset, payload.data(),
                                               payload.size());
      CHECK(written.ok());
      if (!written.ok()) {
        std::printf("write %zu error: %s\n", i,
                    written.error().Format().c_str());
        return;
      }
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  const uint64_t new_size = old_size + (count * 2 - 1) * kBlock;
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto inode = fs.value()->OpenInode(ino);
    CHECK(inode.ok());
    if (!inode.ok()) return;
    CHECK_EQ(inode.value().size, new_size);
    CHECK_EQ(inode.value().format, ondisk::inode::kFormatBtree);
    CHECK_EQ(inode.value().nextents, 1u + count);
    std::vector<XfsExtent> extents;
    CHECK(LoadDataForkExtents(fixture.storage(), fs.value()->geometry(),
                              inode.value(), &extents)
              .ok());
    CHECK_EQ(extents.size(), 1u + count);
  }
  CheckConsistent(fixture);
  // Truncate back to the original size: 1 extent, extents format again.
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto info = fs.value()->SetFileSize(ino, old_size);
    if (!info.ok()) {
      std::printf("truncate error: %s\n", info.error().Format().c_str());
      return;
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto inode = fs.value()->OpenInode(ino);
    CHECK(inode.ok());
    if (!inode.ok()) return;
    CHECK_EQ(inode.value().size, old_size);
    CHECK_EQ(inode.value().format, ondisk::inode::kFormatExtents);
    CHECK_EQ(inode.value().nextents, 1u);
    CHECK_EQ(inode.value().nblocks, fixture.info().data_blocks);
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// The allocator's on-disk state stays exactly consistent.
// ---------------------------------------------------------------------------

TEST(rw2_allocator_counters_match_after_mixed_operations) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  const uint64_t old_size = fixture.info().file_size;
  const uint64_t fd_before = ReadFdblocks(fixture);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    std::vector<uint8_t> data(3 * kBlock, 0x42);
    // allocate 3 blocks beyond EOF
    auto written = fs.value()->WriteFileData(ino, old_size, data.data(),
                                             data.size());
    CHECK(written.ok());
    if (!written.ok()) return;
    // free 2 of them again
    auto info = fs.value()->SetFileSize(ino, old_size + kBlock);
    CHECK(info.ok());
    if (!info.ok()) return;
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  // 3 blocks were allocated and 2 freed.
  CHECK_EQ(ReadFdblocks(fixture), fd_before - 1);
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Truncate zeroes the tail of the partially kept block, so growing the file
// again can only ever expose zeros beyond the old EOF.
// ---------------------------------------------------------------------------

TEST(rw2_truncate_zeroes_partial_block_tail) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t ino = fixture.info().file_ino;
  const uint64_t old_size = fixture.info().file_size;
  {
    auto fs = fixture.Mount(true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto info = fs.value()->SetFileSize(ino, 5000);
    CHECK(info.ok());
    if (!info.ok()) return;
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  // Grow again: the bytes between the new and the old EOF must read as zeros,
  // both in the partial last block and in the hole.
  {
    auto fs = fixture.Mount(true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto info = fs.value()->SetFileSize(ino, old_size);
    CHECK(info.ok());
    if (!info.ok()) return;
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    std::vector<uint8_t> data(static_cast<size_t>(old_size), 0xAA);
    auto read = fs.value()->ReadFile(ino, 0, data.data(), data.size());
    CHECK(read.ok());
    CHECK_EQ(read.value(), data.size());
    for (size_t i = 0; i < 5000; ++i) {
      CHECK_EQ(data[i], xfstest::FixturePatternByte(i));
    }
    for (size_t i = 5000; i < old_size; ++i) {
      if (data[i] != 0) {
        ::xfstest::Fail(__FILE__, __LINE__,
                        "byte " + std::to_string(i) +
                            " beyond the truncated EOF is not zero");
      }
    }
  }
  CheckConsistent(fixture);
  fixture.Reset();
}
