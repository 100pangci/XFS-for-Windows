// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - tests for RW-3: inode allocation, the inode free list and
// the inode counters (AGI / inobt / superblock), then namespace mutation
// (create, unlink, mkdir, rmdir) through the public API.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/ag.h"
#include "xfs/alloc.h"
#include "xfs/btree.h"
#include "xfs/endian.h"
#include "xfs/extent.h"
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


// ===========================================================================
// Namespace mutation: create, unlink, mkdir, rmdir
// ===========================================================================

namespace {

std::string FixedName(int index, size_t width = 8) {
  std::string out = "f";
  out += std::to_string(index);
  while (out.size() < width) out.insert(1, 1, '0');
  return out;
}

std::vector<std::string> ListNames(XfsFilesystem& fs, uint64_t ino) {
  std::vector<std::string> names;
  auto listing = fs.ListDirectory(ino);
  if (!listing.ok()) {
    std::printf("listing error: %s\n", listing.error().Format().c_str());
  }
  CHECK(listing.ok());
  if (!listing.ok()) return names;
  for (const XfsDirEntry& e : listing.value()) {
    if (e.name == "." || e.name == "..") continue;
    names.push_back(e.name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

// The directory format as written on disk (independent of the reader's
// detection helpers): shortform, block, leaf or node.
const char* DirFormatName(xfstest::FixtureImage& fixture, uint64_t ino) {
  auto fs = fixture.Mount(/*read_write=*/false);
  CHECK(fs.ok());
  if (!fs.ok()) return "?";
  auto inode = fs.value()->OpenInode(ino);
  CHECK(inode.ok());
  if (!inode.ok()) return "?";
  if (inode.value().format == ondisk::inode::kFormatLocal) return "shortform";
  const XfsGeometry& geo = fs.value()->geometry();
  const uint64_t leaf_blk = ondisk::dir::kLeafOffset >> geo.block_log;
  std::vector<XfsExtent> extents;
  Status loaded = LoadDataForkExtents(fs.value()->device(), geo,
                                      inode.value(), &extents);
  CHECK(loaded.ok());
  if (!loaded.ok()) return "?";
  uint64_t fsb0 = 0;
  bool mapped0 = false;
  for (const XfsExtent& e : extents) {
    if (e.startoff <= 0 && 0 < e.EndBlock()) {
      fsb0 = e.startblock;
      mapped0 = true;
    }
  }
  if (!mapped0) return "?";
  std::vector<uint8_t> block(geo.block_size, 0);
  CHECK(fixture.storage().Read(geo.FsbToByte(fsb0), block.data(), block.size()));
  const uint32_t magic = GetBE32(block.data(), 0);
  if (magic == ondisk::kDir3BlockMagic) return "block";
  if (magic != ondisk::kDir3DataMagic) return "?";
  // The leaf-space root block decides leaf vs node.
  for (const XfsExtent& e : extents) {
    if (e.startoff <= leaf_blk && leaf_blk < e.EndBlock()) {
      const uint64_t fsb = e.startblock + (leaf_blk - e.startoff);
      std::vector<uint8_t> root(geo.block_size, 0);
      CHECK(fixture.storage().Read(geo.FsbToByte(fsb), root.data(),
                                   root.size()));
      const uint16_t m = GetBE16(root.data(), 8);
      if (m == ondisk::kDir3Leaf1Magic) return "leaf";
      return "node";
    }
  }
  return "?";
}

}  // namespace

TEST(rw3_create_regular_file) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t root = 64;
  const SuperblockCounters before = ReadSuperblockCounters(fixture);
  uint64_t ino = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto created = fs.value()->CreateRegularFile(root, "hello.txt", 0100644, 0, 0);
    if (!created.ok()) {
      std::printf("create error: %s\n", created.error().Format().c_str());
    }
    CHECK(created.ok());
    if (!created.ok()) return;
    ino = created.value();
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  auto fs = fixture.Mount(/*read_write=*/false);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  auto inode = fs.value()->OpenInode(ino);
  CHECK(inode.ok());
  CHECK_EQ(inode.value().nlink, 1u);
  CHECK_EQ(inode.value().size, 0ull);
  CHECK_EQ(inode.value().nblocks, 0ull);
  CHECK_EQ(inode.value().format, ondisk::inode::kFormatExtents);
  CHECK_EQ(inode.value().mode & 07777, 0644);
  CHECK_EQ(inode.value().mode & kSIfmt, kSIfreg);
  auto found = fs.value()->LookupName(root, "hello.txt");
  CHECK(found.ok());
  CHECK_EQ(found.value().ino, ino);
  CHECK_EQ(found.value().ftype, ondisk::dir::kFtRegFile);
  const SuperblockCounters after = ReadSuperblockCounters(fixture);
  CHECK_EQ(after.icount, before.icount);
  CHECK_EQ(after.ifree, before.ifree - 1);
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw3_mkdir_and_rmdir) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t root = 64;
  const SuperblockCounters before = ReadSuperblockCounters(fixture);
  const uint32_t root_nlink_before = [&] {
    auto fs = fixture.Mount(false);
    auto inode = fs.value()->OpenInode(root);
    CHECK(inode.ok());
    return inode.value().nlink;
  }();
  uint64_t dir = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    auto mk = fs.value()->Mkdir(root, "sub", 0755, 0, 0);
    if (!mk.ok()) std::printf("mkdir error: %s\n", mk.error().Format().c_str());
    CHECK(mk.ok());
    auto found = fs.value()->LookupName(root, "sub");
    CHECK(found.ok());
    dir = found.value().ino;
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    auto inode = fs.value()->OpenInode(dir);
    CHECK(inode.ok());
    CHECK(inode.value().IsDir());
    CHECK_EQ(inode.value().nlink, 2u);
    CHECK_EQ(inode.value().format, ondisk::inode::kFormatLocal);
    CHECK_EQ(inode.value().size, 6ull);  // parent inode fits in 32 bits
    CHECK_EQ(inode.value().mode & 07777, 0755);
    auto parent = fs.value()->OpenInode(root);
    CHECK(parent.ok());
    CHECK_EQ(parent.value().nlink, root_nlink_before + 1);
  }
  CheckConsistent(fixture);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    auto rm = fs.value()->RemoveEmptyDirectory(root, "sub");
    if (!rm.ok()) std::printf("rmdir error: %s\n", rm.error().Format().c_str());
    CHECK(rm.ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    auto found = fs.value()->LookupName(root, "sub");
    CHECK(!found.ok());
    CHECK_EQ(found.error().code, XfsError::kNotFound);
    auto parent = fs.value()->OpenInode(root);
    CHECK(parent.ok());
    CHECK_EQ(parent.value().nlink, root_nlink_before);
  }
  const SuperblockCounters after = ReadSuperblockCounters(fixture);
  CHECK_EQ(after.icount, before.icount);
  CHECK_EQ(after.ifree, before.ifree);
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw3_unlink_frees_data_and_inode) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t root = 64;
  const SuperblockCounters before = ReadSuperblockCounters(fixture);
  uint64_t ino = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    auto created = fs.value()->CreateRegularFile(root, "doomed.bin", 0644, 0, 0);
    CHECK(created.ok());
    if (!created.ok()) return;
    ino = created.value();
    std::vector<uint8_t> data(8192, 0x5A);
    auto written = fs.value()->WriteFileData(ino, 0, data.data(), data.size());
    CHECK(written.ok());
    CHECK_EQ(written.value(), data.size());
    auto unlinked = fs.value()->Unlink(root, "doomed.bin", false);
    if (!unlinked.ok()) {
      std::printf("unlink error: %s\n", unlinked.error().Format().c_str());
    }
    CHECK(unlinked.ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    auto found = fs.value()->LookupName(root, "doomed.bin");
    CHECK(!found.ok());
    CHECK_EQ(found.error().code, XfsError::kNotFound);
    // The inode was freed: reading it now fails (mode 0).
    auto inode = fs.value()->OpenInode(ino);
    CHECK(!inode.ok());
  }
  const SuperblockCounters after = ReadSuperblockCounters(fixture);
  CHECK_EQ(after.icount, before.icount);
  CHECK_EQ(after.ifree, before.ifree);
  CHECK_EQ(after.fdblocks, before.fdblocks);
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw3_open_but_unlinked_defers_the_inode_free) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t root = 64;
  uint64_t ino = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    auto created = fs.value()->CreateRegularFile(root, "open.bin", 0644, 0, 0);
    CHECK(created.ok());
    if (!created.ok()) return;
    ino = created.value();
    std::vector<uint8_t> data(4096, 0x33);
    CHECK(fs.value()->WriteFileData(ino, 0, data.data(), data.size()).ok());
    auto unlinked = fs.value()->Unlink(root, "open.bin", true);
    if (!unlinked.ok()) {
      std::printf("unlink error: %s\n", unlinked.error().Format().c_str());
    }
    CHECK(unlinked.ok());
    // The inode is unlinked from the namespace but still readable: this is
    // the open-but-unlinked state.
    auto open = fs.value()->OpenInode(ino);
    CHECK(open.ok());
    if (!open.ok()) return;
    CHECK_EQ(open.value().nlink, 0u);
    CHECK(open.value().mode != 0);
    // The inode is on the AGI unlinked list (a single-element list has a
    // NULLAGINO next pointer, so di_next_unlinked alone does not prove it).
    {
      const XfsGeometry& geo = fs.value()->geometry();
      auto agi = ReadAgi(fixture.storage(), geo, geo.InoAgno(ino));
      CHECK(agi.ok());
      const uint32_t bucket = geo.InoAgino(ino) % ondisk::kAgiUnlinkedBuckets;
      CHECK_EQ(agi.value().unlinked[bucket], geo.InoAgino(ino));
    }
    std::vector<uint8_t> got(4096, 0);
    auto read = fs.value()->ReadFile(ino, 0, got.data(), got.size());
    CHECK(read.ok());
    CHECK(got == data);
    // Last handle closed: final inode cleanup.
    auto freed = fs.value()->FreeUnlinkedInode(ino);
    if (!freed.ok()) {
      std::printf("free error: %s\n", freed.error().Format().c_str());
    }
    CHECK(freed.ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw3_directory_grows_through_all_formats) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t root = 64;
  constexpr int kEntries = 640;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    for (int i = 0; i < kEntries; ++i) {
      const std::string name = FixedName(i);
      auto created = fs.value()->CreateRegularFile(root, name, 0644, 0, 0);
      if (!created.ok()) {
        std::printf("create %s error: %s\n", name.c_str(),
                    created.error().Format().c_str());
        CHECK(created.ok());
        return;
      }
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  // Every name resolves, the listing is exact (plus the fixture's hello.bin).
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    const std::vector<std::string> names = ListNames(*fs.value(), root);
    CHECK_EQ(names.size(), static_cast<size_t>(kEntries) + 1);
    for (int i = 0; i < kEntries; ++i) {
      const std::string name = FixedName(i);
      auto found = fs.value()->LookupName(root, name);
      CHECK(found.ok());
      if (!found.ok()) return;
      CHECK_EQ(found.value().ftype, ondisk::dir::kFtRegFile);
    }
  }
  // A 640-entry directory with 8-byte names has long left the shortform and
  // single-block formats; the on-disk format must be leaf or node.
  const std::string fmt = DirFormatName(fixture, root);
  CHECK(fmt == "leaf" || fmt == "node");
  CheckConsistent(fixture);
  // Delete half of the entries and verify the directory still enumerates.
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    for (int i = 0; i < kEntries; i += 2) {
      auto unlinked = fs.value()->Unlink(root, FixedName(i), false);
      if (!unlinked.ok()) {
        std::printf("unlink %s error: %s\n", FixedName(i).c_str(),
                    unlinked.error().Format().c_str());
        CHECK(unlinked.ok());
        return;
      }
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    const std::vector<std::string> names = ListNames(*fs.value(), root);
    CHECK_EQ(names.size(), static_cast<size_t>(kEntries / 2) + 1);
    for (int i = 0; i < kEntries; ++i) {
      auto found = fs.value()->LookupName(root, FixedName(i));
      CHECK_EQ(found.ok(), (i % 2) == 0 ? false : true);
    }
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw3_rmdir_of_a_grown_directory_frees_its_blocks) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t root = 64;
  uint64_t dir = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    CHECK(fs.value()->Mkdir(root, "big", 0755, 0, 0).ok());
    auto found = fs.value()->LookupName(root, "big");
    CHECK(found.ok());
    dir = found.value().ino;
    for (int i = 0; i < 200; ++i) {
      auto created = fs.value()->CreateRegularFile(dir, FixedName(i), 0644, 0, 0);
      CHECK(created.ok());
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CHECK(DirFormatName(fixture, dir) != std::string("shortform"));
  const SuperblockCounters before = ReadSuperblockCounters(fixture);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    // Remove every child; the directory stays (empty) in a grown format.
    for (int i = 0; i < 200; ++i) {
      CHECK(fs.value()->Unlink(dir, FixedName(i), false).ok());
    }
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    auto rm = fs.value()->RemoveEmptyDirectory(root, "big", false);
    if (!rm.ok()) std::printf("rmdir error: %s\n", rm.error().Format().c_str());
    CHECK(rm.ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  const SuperblockCounters after = ReadSuperblockCounters(fixture);
  // The directory's data/leaf blocks came back. Inode counters may move by a
  // whole chunk when the last inode of a chunk is freed (xfs_difree), so the
  // invariant to check is consistency, not equality.
  CHECK(after.fdblocks > before.fdblocks);
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw3_create_after_delete_reuses_the_inode_without_stale_data) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t root = 64;
  uint64_t first = 0;
  uint32_t first_gen = 0;
  std::vector<uint8_t> secret(8192, 0xC7);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    auto created = fs.value()->CreateRegularFile(root, "a.bin", 0644, 0, 0);
    CHECK(created.ok());
    if (!created.ok()) return;
    first = created.value();
    auto inode = fs.value()->OpenInode(first);
    CHECK(inode.ok());
    first_gen = inode.value().gen;
    CHECK(fs.value()->WriteFileData(first, 0, secret.data(), secret.size()).ok());
    CHECK(fs.value()->Unlink(root, "a.bin", false).ok());
    auto second = fs.value()->CreateRegularFile(root, "b.bin", 0644, 0, 0);
    CHECK(second.ok());
    if (!second.ok()) return;
    // The allocator hands out the first free inode: the one just freed.
    CHECK_EQ(second.value(), first);
    auto inode2 = fs.value()->OpenInode(second.value());
    CHECK(inode2.ok());
    CHECK(inode2.value().gen != first_gen);
    CHECK_EQ(inode2.value().size, 0ull);
    CHECK_EQ(inode2.value().nextents, 0u);
    CHECK_EQ(inode2.value().nblocks, 0ull);
    std::vector<uint8_t> got(4096, 0xAA);
    auto read = fs.value()->ReadFile(second.value(), 0, got.data(), got.size());
    CHECK(read.ok());
    CHECK_EQ(read.value(), 0ull);  // size 0: nothing visible
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw3_rmdir_rejects_a_non_empty_directory) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t root = 64;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    CHECK(fs.value()->Mkdir(root, "full", 0755, 0, 0).ok());
    auto found = fs.value()->LookupName(root, "full");
    CHECK(found.ok());
    const uint64_t dir = found.value().ino;
    CHECK(fs.value()->CreateRegularFile(dir, "child", 0644, 0, 0).ok());
    auto rm = fs.value()->RemoveEmptyDirectory(root, "full", false);
    CHECK(!rm.ok());
    CHECK_EQ(rm.error().code, XfsError::kDirectoryNotEmpty);
    // Nothing was modified.
    auto again = fs.value()->LookupName(root, "full");
    CHECK(again.ok());
    CHECK(fs.value()->LookupName(dir, "child").ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw3_create_rejects_invalid_names) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t root = 64;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    auto dot = fs.value()->CreateRegularFile(root, ".", 0644, 0, 0);
    CHECK(!dot.ok());
    auto dotdot = fs.value()->CreateRegularFile(root, "..", 0644, 0, 0);
    CHECK(!dotdot.ok());
    auto slash = fs.value()->CreateRegularFile(root, "a/b", 0644, 0, 0);
    CHECK(!slash.ok());
    auto empty = fs.value()->CreateRegularFile(root, "", 0644, 0, 0);
    CHECK(!empty.ok());
    const std::string too_long(256, 'x');
    auto long_name = fs.value()->CreateRegularFile(root, too_long, 0644, 0, 0);
    CHECK(!long_name.ok());
    CHECK_EQ(long_name.error().code, XfsError::kNameTooLong);
    const std::string max_name(255, 'y');
    auto max_ok = fs.value()->CreateRegularFile(root, max_name, 0644, 0, 0);
    if (!max_ok.ok()) {
      std::printf("255-byte create error: %s\n",
                  max_ok.error().Format().c_str());
    }
    CHECK(max_ok.ok());
    CHECK(fs.value()->LookupName(root, max_name).ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw3_create_duplicate_name_is_refused) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  const uint64_t root = 64;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    CHECK(fs.value()->CreateRegularFile(root, "dup", 0644, 0, 0).ok());
    auto again = fs.value()->CreateRegularFile(root, "dup", 0644, 0, 0);
    CHECK(!again.ok());
    CHECK_EQ(again.error().code, XfsError::kAlreadyExists);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  const std::vector<std::string> expected{"dup", "hello.bin"};
  auto fs = fixture.Mount(false);
  CHECK(fs.ok());
  if (!fs.ok()) return;
  const std::vector<std::string> names = ListNames(*fs.value(), root);
  CHECK(names == expected);
  CheckConsistent(fixture);
  fixture.Reset();
}
