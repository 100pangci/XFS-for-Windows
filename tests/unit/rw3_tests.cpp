// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - tests for RW-3: inode allocation, the inode free list and
// the inode counters (AGI / inobt / superblock).
//
// The tests drive the same internal path the high-level namespace operations
// use (BlockAllocator + InodeAllocator inside a transaction), verify the
// on-disk result after a fresh remount, and cross-check the AGI counters, the
// inobt records and the superblock counters independently of the writer.
#include <cstring>
#include <string>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/ag.h"
#include "xfs/alloc.h"
#include "xfs/btree.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/ialloc.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"

using namespace xfs;

namespace {

constexpr uint32_t kNullAgino = 0xffffffffu;

struct SuperblockCounters {
  uint64_t icount = 0;
  uint64_t ifree = 0;
  uint64_t fdblocks = 0;
};

SuperblockCounters ReadSuperblockCounters(xfstest::FixtureImage& fixture) {
  std::vector<uint8_t> sector(512, 0);
  CHECK(fixture.storage().Read(0, sector.data(), sector.size()));
  SuperblockCounters out;
  out.icount = GetBE64(sector.data(), ondisk::sb::kIcount);
  out.ifree = GetBE64(sector.data(), ondisk::sb::kIfree);
  out.fdblocks = GetBE64(sector.data(), ondisk::sb::kFdblocks);
  return out;
}

struct InobtRecordView {
  uint32_t startino = 0;
  uint32_t freecount = 0;
  uint64_t free = 0;
};

// Reads every inobt record of AG0 straight from disk (independent of the
// writer's in-memory tree).
std::vector<InobtRecordView> ReadInobtRecords(xfstest::FixtureImage& fixture,
                                              uint32_t agno = 0) {
  std::vector<InobtRecordView> out;
  auto fs = fixture.Mount(/*read_write=*/false);
  CHECK(fs.ok());
  if (!fs.ok()) return out;
  const XfsGeometry& geo = fs.value()->geometry();
  auto agi = ReadAgi(fixture.storage(), geo, agno);
  CHECK(agi.ok());
  if (!agi.ok()) return out;
  BtreeSpec spec;
  spec.name = "inobt";
  spec.format = BtreeFormat::kShort;
  spec.block_size = geo.block_size;
  spec.key_size = ondisk::btree::kInobtKeySize;
  spec.ptr_size = ondisk::btree::kInobtPtrSize;
  spec.rec_size = ondisk::btree::kInobtRecSize;
  spec.magic = ondisk::kIbtCrcMagic;
  spec.null_ptr = ondisk::btree::kNullAgBlock;
  spec.expected_owner = agno;
  spec.owner_size = 4;
  spec.uuid = geo.meta_uuid;
  spec.ptr_to_daddr = [&](uint64_t agbno, uint64_t* daddr) {
    if (agbno >= geo.AgBlocksFor(agno)) return false;
    *daddr = geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))) / 512;
    return true;
  };
  BtreeWalkCallbacks cb;
  cb.read_block = [&](uint64_t fsb, std::vector<uint8_t>* buf) {
    buf->resize(geo.block_size);
    return fixture.storage().Read(geo.FsbToByte(fsb), buf->data(), buf->size());
  };
  cb.ptr_in_range = [&](uint64_t ptr) {
    return ptr < geo.AgBlocksFor(agno);
  };
  cb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                 const uint8_t* buf) -> Status {
    if (meta.level != 0) return OkStatus();
    for (uint16_t i = 0; i < meta.numrecs; ++i) {
      const uint8_t* rec = BtreeLeafRec(buf, meta, spec, i);
      InobtRecordView v;
      v.startino = GetBE32(rec + ondisk::btree::kInobtRecStartino);
      v.freecount = GetBE32(rec + ondisk::btree::kInobtRecFreecount);
      v.free = GetBE64(rec + ondisk::btree::kInobtRecFree);
      out.push_back(v);
    }
    return OkStatus();
  };
  CHECK(WalkBtree(spec, agi.value().root, cb).ok());
  return out;
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

// Runs `body` in one transaction with a block allocator and an inode
// allocator, committing both allocator halves, then writes the clean unmount
// record.
template <typename Body>
void RunAllocTransaction(xfstest::FixtureImage& fixture, Body body) {
  auto fs = fixture.Mount(/*read_write=*/true);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  Status status = fs.value()->RunTransaction(
      [&](XfsTransaction& txn) -> Status {
        BlockAllocator blocks(&txn, &fs.value()->device(),
                              &fs.value()->geometry());
        InodeAllocator inodes(&txn, &blocks, &fs.value()->device(),
                              &fs.value()->superblock(),
                              &fs.value()->geometry());
        Status s = body(txn, blocks, inodes);
        if (!s.ok()) return s;
        s = blocks.Commit();
        if (!s.ok()) return s;
        return inodes.Commit();
      });
  if (!status.ok()) {
    std::printf("transaction error: %s\n", status.error().Format().c_str());
  }
  CHECK(status.ok());
  CHECK(fs.value()->WriteCleanUnmountRecord().ok());
  CHECK(fs.value()->Flush().ok());
}

// Reads the AGI (through the reader's verified path).
XfsAgi ReadAg0Agi(xfstest::FixtureImage& fixture) {
  auto fs = fixture.Mount(/*read_write=*/false);
  CHECK(fs.ok());
  auto agi = ReadAgi(fixture.storage(), fs.value()->geometry(), 0);
  CHECK(agi.ok());
  return agi.value();
}

}  // namespace

// ---------------------------------------------------------------------------
// Allocation from an existing chunk.
// ---------------------------------------------------------------------------

TEST(rw3_allocates_first_free_inode) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const SuperblockCounters before = ReadSuperblockCounters(fixture);
  const XfsAgi agi_before = ReadAg0Agi(fixture);
  CHECK_EQ(agi_before.count, 64u);
  CHECK_EQ(agi_before.freecount, 60u);

  uint64_t ino = 0;
  RunAllocTransaction(fixture, [&](XfsTransaction&, BlockAllocator&,
                                   InodeAllocator& inodes) -> Status {
    return inodes.AllocateInode(0, 0, &ino);
  });

  // Inodes 64..67 are in use (root, rt bitmap, rt summary, fixture file), so
  // 68 is the first free inode.
  CHECK_EQ(ino, 68ull);
  const XfsAgi agi = ReadAg0Agi(fixture);
  CHECK_EQ(agi.count, 64u);
  CHECK_EQ(agi.freecount, 59u);
  const SuperblockCounters after = ReadSuperblockCounters(fixture);
  CHECK_EQ(after.icount, before.icount);
  CHECK_EQ(after.ifree, before.ifree - 1);
  CHECK_EQ(after.fdblocks, before.fdblocks);

  auto records = ReadInobtRecords(fixture);
  CHECK_EQ(records.size(), 1u);
  CHECK_EQ(records[0].freecount, 59u);
  CHECK((records[0].free & (1ull << (ino - records[0].startino))) == 0);
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw3_allocates_multiple_inodes_in_order) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();

  std::vector<uint64_t> inos;
  RunAllocTransaction(fixture, [&](XfsTransaction&, BlockAllocator&,
                                   InodeAllocator& inodes) -> Status {
    for (int i = 0; i < 5; ++i) {
      uint64_t ino = 0;
      Status s = inodes.AllocateInode(0, 0, &ino);
      if (!s.ok()) return s;
      inos.push_back(ino);
    }
    return OkStatus();
  });
  CHECK_EQ(inos.size(), 5u);
  for (size_t i = 1; i < inos.size(); ++i) {
    CHECK_EQ(inos[i], inos[i - 1] + 1);
  }
  CHECK_EQ(inos[0], 68ull);
  const XfsAgi agi = ReadAg0Agi(fixture);
  CHECK_EQ(agi.freecount, 55u);
  auto records = ReadInobtRecords(fixture);
  CHECK_EQ(records[0].freecount, 55u);
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Free and reuse.
// ---------------------------------------------------------------------------

TEST(rw3_free_inode_restores_counters_and_allows_reuse) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const SuperblockCounters before = ReadSuperblockCounters(fixture);

  uint64_t ino = 0;
  RunAllocTransaction(fixture, [&](XfsTransaction&, BlockAllocator&,
                                   InodeAllocator& inodes) -> Status {
    return inodes.AllocateInode(0, 0, &ino);
  });
  CHECK_EQ(ino, 68ull);

  RunAllocTransaction(fixture, [&](XfsTransaction&, BlockAllocator&,
                                   InodeAllocator& inodes) -> Status {
    return inodes.FreeInode(ino);
  });
  const SuperblockCounters after_free = ReadSuperblockCounters(fixture);
  CHECK_EQ(after_free.icount, before.icount);
  CHECK_EQ(after_free.ifree, before.ifree);
  {
    const XfsAgi agi = ReadAg0Agi(fixture);
    CHECK_EQ(agi.count, 64u);
    CHECK_EQ(agi.freecount, 60u);
  }

  // The freed inode is handed out again first.
  uint64_t reused = 0;
  RunAllocTransaction(fixture, [&](XfsTransaction&, BlockAllocator&,
                                   InodeAllocator& inodes) -> Status {
    return inodes.AllocateInode(0, 0, &reused);
  });
  CHECK_EQ(reused, ino);
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// A new inode chunk when the existing one has no free inode.
// ---------------------------------------------------------------------------

TEST(rw3_allocates_new_inode_chunk) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const SuperblockCounters before = ReadSuperblockCounters(fixture);

  // Fill the existing chunk (60 free inodes), then one more allocation must
  // create a new 64-inode chunk.
  std::vector<uint64_t> inos;
  RunAllocTransaction(fixture, [&](XfsTransaction&, BlockAllocator&,
                                   InodeAllocator& inodes) -> Status {
    for (int i = 0; i < 61; ++i) {
      uint64_t ino = 0;
      Status s = inodes.AllocateInode(0, 0, &ino);
      if (!s.ok()) return s;
      inos.push_back(ino);
    }
    return OkStatus();
  });
  CHECK_EQ(inos.size(), 61u);
  CHECK_EQ(inos[59], 127ull);  // last inode of the first chunk
  CHECK((inos[60] & 63) == 0);  // first inode of a chunk

  const XfsAgi agi = ReadAg0Agi(fixture);
  CHECK_EQ(agi.count, 128u);
  CHECK_EQ(agi.freecount, 63u);
  const SuperblockCounters after = ReadSuperblockCounters(fixture);
  CHECK_EQ(after.icount, before.icount + 64);
  CHECK_EQ(after.ifree, before.ifree + 64 - 61);
  CHECK(after.fdblocks < before.fdblocks);

  auto records = ReadInobtRecords(fixture);
  CHECK_EQ(records.size(), 2u);
  CHECK_EQ(records[0].freecount, 0u);
  CHECK_EQ(records[0].free, 0ull);
  CHECK_EQ(records[1].freecount, 63u);
  CHECK_EQ(records[1].startino, static_cast<uint32_t>(inos[60]) & ~63u);
  CHECK((records[1].free & 1ull) == 0);  // first inode of the chunk is used
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw3_fully_freed_chunk_is_removed_and_blocks_returned) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const SuperblockCounters before = ReadSuperblockCounters(fixture);

  // Fill the first chunk, then allocate exactly one inode of the new chunk.
  std::vector<uint64_t> first_chunk;
  uint64_t new_chunk_ino = 0;
  RunAllocTransaction(fixture, [&](XfsTransaction&, BlockAllocator&,
                                   InodeAllocator& inodes) -> Status {
    for (int i = 0; i < 60; ++i) {
      uint64_t ino = 0;
      Status s = inodes.AllocateInode(0, 0, &ino);
      if (!s.ok()) return s;
      first_chunk.push_back(ino);
    }
    return inodes.AllocateInode(0, 0, &new_chunk_ino);
  });
  CHECK_EQ(new_chunk_ino & 63, 0ull);
  const SuperblockCounters after_alloc = ReadSuperblockCounters(fixture);
  CHECK_EQ(ReadAg0Agi(fixture).count, 128u);

  // Freeing that single inode makes the new chunk completely free: the chunk
  // is removed from the inobt and its eight blocks are released.
  RunAllocTransaction(fixture, [&](XfsTransaction&, BlockAllocator&,
                                   InodeAllocator& inodes) -> Status {
    return inodes.FreeInode(new_chunk_ino);
  });
  const XfsAgi agi = ReadAg0Agi(fixture);
  CHECK_EQ(agi.count, 64u);
  CHECK_EQ(agi.freecount, 0u);

  auto records = ReadInobtRecords(fixture);
  CHECK_EQ(records.size(), 1u);
  CHECK_EQ(records[0].freecount, 0u);

  // The eight chunk blocks are free again; only the block allocator's inobt
  // state is otherwise unchanged, so fdblocks returns to the pre-allocation
  // value. The first chunk is completely allocated by now (60 regular inodes
  // plus the four system inodes), so ifree is back to zero.
  const SuperblockCounters after_free = ReadSuperblockCounters(fixture);
  CHECK_EQ(after_free.fdblocks, before.fdblocks);
  CHECK_EQ(after_free.icount, before.icount);
  CHECK_EQ(after_free.ifree, 0ull);

  // The same inode number can be allocated again by a new chunk.
  uint64_t again = 0;
  RunAllocTransaction(fixture, [&](XfsTransaction&, BlockAllocator&,
                                   InodeAllocator& inodes) -> Status {
    return inodes.AllocateInode(0, 0, &again);
  });
  CHECK_EQ(again & 63, 0ull);
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// AGI unlinked list buckets.
// ---------------------------------------------------------------------------

TEST(rw3_unlinked_bucket_roundtrip) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  CHECK_EQ(ReadAg0Agi(fixture).unlinked[7], kNullAgino);

  RunAllocTransaction(fixture, [&](XfsTransaction&, BlockAllocator&,
                                   InodeAllocator& inodes) -> Status {
    return inodes.SetUnlinkedBucketHead(0, 7, 68);
  });
  CHECK_EQ(ReadAg0Agi(fixture).unlinked[7], 68u);

  RunAllocTransaction(fixture, [&](XfsTransaction&, BlockAllocator&,
                                   InodeAllocator& inodes) -> Status {
    auto head = inodes.UnlinkedBucketHead(0, 7);
    if (!head.ok()) return head.error();
    if (head.value() != 68) {
      return XFS_ERR(XfsError::kInternal, "unexpected unlinked bucket head");
    }
    return inodes.SetUnlinkedBucketHead(0, 7, kNullAgino);
  });
  CHECK_EQ(ReadAg0Agi(fixture).unlinked[7], kNullAgino);
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// New chunk geometry: aligned, full, and correctly counted.
// ---------------------------------------------------------------------------

TEST(rw3_new_chunk_is_a_full_64_inode_aligned_chunk) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();

  std::vector<uint64_t> inos;
  RunAllocTransaction(fixture, [&](XfsTransaction&, BlockAllocator&,
                                   InodeAllocator& inodes) -> Status {
    for (int i = 0; i < 61; ++i) {
      uint64_t ino = 0;
      Status s = inodes.AllocateInode(0, 0, &ino);
      if (!s.ok()) return s;
      inos.push_back(ino);
    }
    return OkStatus();
  });

  auto fs = fixture.Mount(/*read_write=*/false);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  const XfsGeometry& geo = fs.value()->geometry();
  const XfsInodeGeometry igeo =
      MakeInodeGeometry(fs.value()->superblock(), geo);
  CHECK_EQ(igeo.ialloc_inos, 64u);
  CHECK_EQ(igeo.ialloc_blks, 8u);
  // The chunk start block must satisfy the alignment rule the inobt record is
  // checked against (chunk start at an inode-chunk boundary).
  const uint32_t startino = static_cast<uint32_t>(inos[60]) & ~63u;
  CHECK_EQ(startino % igeo.ialloc_inos, 0u);
  const uint32_t agbno = startino >> geo.inopblog;
  CHECK_EQ(agbno % igeo.ialloc_blks, 0u);
  CheckConsistent(fixture);
  fixture.Reset();
}
