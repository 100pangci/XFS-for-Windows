// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - tests for RW-6A: finobt, inobtcount, sparse inodes,
// bigtime and NREXT64.
#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/ag.h"
#include "xfs/alloc.h"
#include "xfs/btree.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/formatter.h"
#include "xfs/ialloc.h"
#include "xfs/inode.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"

using namespace xfs;

namespace {

namespace di = ondisk::inode;

struct ModernImage {
  std::unique_ptr<xfstest::MemoryBlockDevice> device;
  std::unique_ptr<XfsFilesystem> fs;
};

ModernImage MakeModernImage(uint64_t size, uint32_t block_size = 4096,
                            uint32_t inode_size = 512,
                            bool finobt = true, bool inobtcount = true,
                            bool sparse = true, bool bigtime = true,
                            bool nrext64 = true) {
  ModernImage out;
  out.device = std::make_unique<xfstest::MemoryBlockDevice>(size, 512);
  mkfs::FormatOptions options;
  options.block_size = block_size;
  options.inode_size = inode_size;
  options.quiet = true;
  options.finobt = finobt;
  options.inobtcount = inobtcount;
  options.sparse_inodes = sparse;
  options.bigtime = bigtime;
  options.nrext64 = nrext64;
  auto report = mkfs::Format(*out.device, options);
  CHECK(report.ok());
  MountOptions mount;
  mount.read_write = true;
  auto borrowed =
      std::make_unique<xfstest::BorrowedBlockDevice>(out.device.get());
  auto mounted = XfsFilesystem::Mount(std::move(borrowed), mount);
  CHECK(mounted.ok());
  out.fs = std::move(mounted.value());
  return out;
}

void ExpectCheckOk(const CheckReport& report) {
  if (report.Overall() == CheckStatus::kOk) return;
  std::string detail = report.Summary();
  for (const CheckItem& item : report.items) {
    if (item.status == CheckStatus::kOk) continue;
    detail += "\n";
    detail += ToString(item.status);
    detail += " ";
    detail += item.name;
    detail += ": ";
    detail += item.message;
  }
  ::xfstest::Fail(__FILE__, __LINE__, detail);
}

std::vector<uint8_t> ReadInodeImage(BlockDevice& device,
                                    const XfsGeometry& geo, uint64_t ino) {
  std::vector<uint8_t> image(geo.inode_size);
  CHECK(device.Read(geo.InoToByteOffset(ino), image.data(), image.size()));
  return image;
}

std::vector<InobtRecord> ReadInobt(BlockDevice& device,
                                   const XfsGeometry& geo, uint32_t agno) {
  auto agi = ReadAgi(device, geo, agno);
  CHECK(agi.ok());
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
    *daddr = geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))) /
             512;
    return true;
  };
  std::vector<InobtRecord> out;
  BtreeWalkCallbacks cb;
  cb.read_block = [&](uint64_t agbno, std::vector<uint8_t>* buf) {
    if (agbno >= geo.AgBlocksFor(agno)) return false;
    buf->resize(geo.block_size);
    return device.Read(
        geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))),
        buf->data(), buf->size());
  };
  cb.ptr_in_range = [&](uint64_t agbno) {
    return agbno < geo.AgBlocksFor(agno);
  };
  cb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                 const uint8_t* buf) -> Status {
    if (meta.level != 0) return OkStatus();
    for (uint16_t i = 0; i < meta.numrecs; ++i) {
      const uint8_t* rec = BtreeLeafRec(buf, meta, spec, i);
      InobtRecord r;
      r.startino = GetBE32(rec);
      r.free = GetBE64(rec + 8);
      if (geo.features.spinodes) {
        r.holemask = GetBE16(rec + 4);
        r.count = GetU8(rec + 6);
        r.freecount = GetU8(rec + 7);
      } else {
        r.count = 64;
        r.freecount = GetBE32(rec + 4);
      }
      out.push_back(r);
    }
    return OkStatus();
  };
  CHECK(WalkBtree(spec, agi.value().root, cb).ok());
  return out;
}

struct FreeExtent {
  uint32_t start = 0;
  uint32_t len = 0;
};

std::vector<FreeExtent> ReadFreeExtents(BlockDevice& device,
                                        const XfsGeometry& geo,
                                        uint32_t agno) {
  auto agf = ReadAgf(device, geo, agno);
  CHECK(agf.ok());
  BtreeSpec spec;
  spec.name = "bnobt";
  spec.format = BtreeFormat::kShort;
  spec.block_size = geo.block_size;
  spec.key_size = ondisk::btree::kAllocKeySize;
  spec.ptr_size = ondisk::btree::kAllocPtrSize;
  spec.rec_size = ondisk::btree::kAllocRecSize;
  spec.magic = ondisk::kBtbCrcMagic;
  spec.null_ptr = ondisk::btree::kNullAgBlock;
  spec.expected_owner = agno;
  spec.owner_size = 4;
  spec.uuid = geo.meta_uuid;
  spec.ptr_to_daddr = [&](uint64_t agbno, uint64_t* daddr) {
    if (agbno >= geo.AgBlocksFor(agno)) return false;
    *daddr = geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))) /
             512;
    return true;
  };
  std::vector<FreeExtent> out;
  BtreeWalkCallbacks cb;
  cb.read_block = [&](uint64_t agbno, std::vector<uint8_t>* buf) {
    if (agbno >= geo.AgBlocksFor(agno)) return false;
    buf->resize(geo.block_size);
    return device.Read(
        geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))),
        buf->data(), buf->size());
  };
  cb.ptr_in_range = [&](uint64_t agbno) {
    return agbno < geo.AgBlocksFor(agno);
  };
  cb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                 const uint8_t* buf) -> Status {
    if (meta.level != 0) return OkStatus();
    for (uint16_t i = 0; i < meta.numrecs; ++i) {
      const uint8_t* rec = BtreeLeafRec(buf, meta, spec, i);
      out.push_back({GetBE32(rec), GetBE32(rec + 4)});
    }
    return OkStatus();
  };
  CHECK(WalkBtree(spec, agf.value().bno_root, cb).ok());
  return out;
}

}  // namespace

TEST(rw6_modern_profile_feature_bits) {
  ModernImage image = MakeModernImage(300ull << 20);
  const XfsSuperblock& sb = image.fs->superblock();
  CHECK(sb.features.finobt);
  CHECK(sb.features.inobtcnt);
  CHECK(sb.features.spinodes);
  CHECK(sb.features.bigtime);
  CHECK(sb.features.nrext64);
  CHECK(sb.spino_align != 0);
  CHECK(sb.inoalignmt >= sb.spino_align);

  auto created = image.fs->CreateRegularFile(image.fs->RootInode(), "modern",
                                             0644, 0, 0);
  CHECK(created.ok());
  auto raw = ReadInodeImage(image.fs->device(), image.fs->geometry(),
                            created.value());
  CHECK((GetBE64(raw.data(), di::kFlags2) & di::kDiFlag2Bigtime) != 0);
  CHECK((GetBE64(raw.data(), di::kFlags2) & di::kDiFlag2Nrext64) != 0);
  CHECK_EQ(GetBE16(raw.data(), di::kAnextents), 0);  // di_nrext64_pad
  CHECK(image.fs->WriteCleanUnmountRecord().ok());
  CheckReport report = image.fs->CheckConsistency(CheckOptions{});
  ExpectCheckOk(report);
}

TEST(rw6_mixed_feature_profiles) {
  struct Profile {
    bool finobt;
    bool inobtcount;
    bool sparse;
    bool bigtime;
    bool nrext64;
    const char* name;
  };
  const Profile profiles[] = {
      {true, false, false, false, false, "finobt-only"},
      {true, true, true, false, false, "finobt-sparse"},
      {false, false, false, true, true, "inode-modern-only"},
  };
  for (const Profile& profile : profiles) {
    ModernImage image = MakeModernImage(300ull << 20, 4096, 512,
                                        profile.finobt, profile.inobtcount,
                                        profile.sparse, profile.bigtime,
                                        profile.nrext64);
    auto created = image.fs->CreateRegularFile(
        image.fs->RootInode(), profile.name, 0644, 0, 0);
    CHECK(created.ok());
    const uint8_t data = 0x5a;
    CHECK(image.fs->WriteFileData(created.value(), 0, &data, 1).ok());
    CHECK(image.fs->WriteCleanUnmountRecord().ok());
    ExpectCheckOk(image.fs->CheckConsistency(CheckOptions{}));
  }
}

TEST(rw6_finobt_and_inobtcount_survive_a_split) {
  // 1 KiB blocks make an inobt/finobt leaf hold 60 records, so 64 chunks
  // force a real split; freeing them again exercises merge and root collapse.
  ModernImage image = MakeModernImage(512ull << 20, 1024, 512);

  // Make the formatter's root chunk fully allocated so PreallocateChunk()
  // will create new chunks. The root/rt inodes (bits 0-2) stay allocated.
  {
    auto txn = image.fs->BeginTransaction();
    CHECK(txn.ok());
    BlockAllocator alloc(&txn.value(), &image.fs->device(),
                         &image.fs->geometry());
    InodeAllocator inodes(&txn.value(), &alloc, &image.fs->device(),
                          &image.fs->superblock(), &image.fs->geometry());
    for (uint32_t i = 0; i < 61; ++i) {
      uint64_t ino = 0;
      CHECK(inodes.AllocateInode(0, 0, &ino).ok());
    }
    CHECK(alloc.Commit().ok());
    CHECK(inodes.Commit().ok());
    CHECK(image.fs->CommitTransaction(txn.value()).ok());
  }

  // Grow 64 full chunks, then free one inode per chunk. The inobt now has 65
  // records and the finobt 64; both exceed one 1 KiB leaf (60 records).
  std::vector<uint64_t> inos;
  std::vector<uint64_t> chunk_first;
  for (uint32_t i = 0; i < 64; ++i) {
    {
      auto txn = image.fs->BeginTransaction();
      CHECK(txn.ok());
      BlockAllocator alloc(&txn.value(), &image.fs->device(),
                           &image.fs->geometry());
      InodeAllocator inodes(&txn.value(), &alloc, &image.fs->device(),
                            &image.fs->superblock(), &image.fs->geometry());
      CHECK(inodes.PreallocateChunk(0).ok());
      CHECK(alloc.Commit().ok());
      CHECK(inodes.Commit().ok());
      CHECK(image.fs->CommitTransaction(txn.value()).ok());
    }
    auto txn = image.fs->BeginTransaction();
    CHECK(txn.ok());
    BlockAllocator alloc(&txn.value(), &image.fs->device(),
                         &image.fs->geometry());
    InodeAllocator inodes(&txn.value(), &alloc, &image.fs->device(),
                          &image.fs->superblock(), &image.fs->geometry());
    for (uint32_t j = 0; j < 64; ++j) {
      uint64_t ino = 0;
      CHECK(inodes.AllocateInode(0, 0, &ino).ok());
      if (j == 0) chunk_first.push_back(ino);
      inos.push_back(ino);
    }
    CHECK(alloc.Commit().ok());
    CHECK(inodes.Commit().ok());
    CHECK(image.fs->CommitTransaction(txn.value()).ok());
  }
  {
    auto txn = image.fs->BeginTransaction();
    CHECK(txn.ok());
    BlockAllocator alloc(&txn.value(), &image.fs->device(),
                         &image.fs->geometry());
    InodeAllocator inodes(&txn.value(), &alloc, &image.fs->device(),
                          &image.fs->superblock(), &image.fs->geometry());
    for (uint64_t ino : chunk_first) CHECK(inodes.FreeInode(ino).ok());
    CHECK(alloc.Commit().ok());
    CHECK(inodes.Commit().ok());
    CHECK(image.fs->CommitTransaction(txn.value()).ok());
  }

  auto split = ReadAgi(image.fs->device(), image.fs->geometry(), 0);
  CHECK(split.ok());
  CHECK(split.value().level >= 2);
  CHECK(split.value().free_level >= 2);
  CHECK(split.value().iblocks > 1);
  CHECK(split.value().fblocks > 1);

  // Free the remaining inodes. Each chunk becomes fully free and is removed
  // from both trees; the finobt collapses to an empty leaf root.
  std::set<uint64_t> already_free(chunk_first.begin(), chunk_first.end());
  std::vector<uint64_t> remaining;
  for (uint64_t ino : inos) {
    if (already_free.count(ino) == 0) remaining.push_back(ino);
  }
  for (size_t i = 0; i < remaining.size(); i += 64) {
    auto txn = image.fs->BeginTransaction();
    CHECK(txn.ok());
    BlockAllocator alloc(&txn.value(), &image.fs->device(),
                         &image.fs->geometry());
    InodeAllocator inodes(&txn.value(), &alloc, &image.fs->device(),
                          &image.fs->superblock(), &image.fs->geometry());
    const size_t end = std::min<size_t>(i + 64, remaining.size());
    for (size_t j = i; j < end; ++j) {
      CHECK(inodes.FreeInode(remaining[j]).ok());
    }
    CHECK(alloc.Commit().ok());
    CHECK(inodes.Commit().ok());
    CHECK(image.fs->CommitTransaction(txn.value()).ok());
  }

  auto collapsed = ReadAgi(image.fs->device(), image.fs->geometry(), 0);
  CHECK(collapsed.ok());
  CHECK_EQ(collapsed.value().level, 1u);
  CHECK_EQ(collapsed.value().free_level, 1u);
  CHECK_EQ(collapsed.value().iblocks, 1u);
  CHECK_EQ(collapsed.value().fblocks, 1u);
  CHECK(image.fs->WriteCleanUnmountRecord().ok());
  CheckReport report = image.fs->CheckConsistency(CheckOptions{});
  ExpectCheckOk(report);
}

TEST(rw6_sparse_chunk_alloc_free_and_holes) {
  ModernImage image = MakeModernImage(512ull << 20, 1024, 512);
  BlockDevice& dev = image.fs->device();
  const XfsGeometry& geo = image.fs->geometry();

  // Fill the formatter's root chunk so the AG has no free inodes.
  {
    auto txn = image.fs->BeginTransaction();
    CHECK(txn.ok());
    BlockAllocator alloc(&txn.value(), &dev, &geo);
    InodeAllocator inodes(&txn.value(), &alloc, &dev,
                          &image.fs->superblock(), &geo);
    for (uint32_t i = 0; i < 61; ++i) {
      uint64_t ino = 0;
      CHECK(inodes.AllocateInode(0, 0, &ino).ok());
    }
    CHECK(alloc.Commit().ok());
    CHECK(inodes.Commit().ok());
    CHECK(image.fs->CommitTransaction(txn.value()).ok());
  }

  // Consume every free extent in AG0. A fresh 1 KiB-block filesystem has only
  // a couple of large extents, so this is a few transactions.
  std::vector<FreeExtent> extents = ReadFreeExtents(dev, geo, 0);
  bool found_target = false;
  uint32_t target_start = 0;
  for (const FreeExtent& extent : extents) {
    const uint32_t aligned = static_cast<uint32_t>(
        (static_cast<uint64_t>(extent.start) + 15) & ~uint64_t{15});
    if (aligned >= 32 && aligned + 16 <= extent.start + extent.len) {
      found_target = true;
      target_start = aligned;
      break;
    }
  }
  CHECK(found_target);
  std::sort(extents.begin(), extents.end(),
            [](const FreeExtent& a, const FreeExtent& b) {
              return a.len > b.len;
            });
  for (const FreeExtent& extent : extents) {
    auto txn = image.fs->BeginTransaction();
    CHECK(txn.ok());
    BlockAllocator alloc(&txn.value(), &dev, &geo);
    Allocation allocation;
    CHECK(alloc.AllocateFromAg(0, extent.len, 1, &allocation).ok());
    CHECK_EQ(allocation.len, extent.len);
    CHECK(alloc.Commit().ok());
    CHECK(image.fs->CommitTransaction(txn.value()).ok());
  }

  // Re-open one cluster-sized (16 block) aligned hole. A full 32-block chunk
  // cannot fit, but the sparse allocator can.
  {
    auto txn = image.fs->BeginTransaction();
    CHECK(txn.ok());
    BlockAllocator alloc(&txn.value(), &dev, &geo);
    CHECK(alloc.Free(geo.AgFsb(0, target_start), 16).ok());
    CHECK(alloc.Commit().ok());
    CHECK(image.fs->CommitTransaction(txn.value()).ok());
  }
  {
    auto txn = image.fs->BeginTransaction();
    CHECK(txn.ok());
    BlockAllocator alloc(&txn.value(), &dev, &geo);
    InodeAllocator inodes(&txn.value(), &alloc, &dev,
                          &image.fs->superblock(), &geo);
    CHECK(inodes.PreallocateChunk(0).ok());
    CHECK(alloc.Commit().ok());
    CHECK(inodes.Commit().ok());
    CHECK(image.fs->CommitTransaction(txn.value()).ok());
  }

  auto records = ReadInobt(dev, geo, 0);
  const InobtRecord* sparse = nullptr;
  for (const InobtRecord& r : records) {
    if (r.holemask != 0) sparse = &r;
  }
  CHECK(sparse != nullptr);
  CHECK_EQ(sparse->count, 32);       // 16 blocks * 2 inodes per block
  CHECK_EQ(sparse->freecount, 32u);
  CHECK_EQ(sparse->free, ~0ULL);
  CHECK_EQ(InobtAllocMask(sparse->holemask) & ~sparse->free, 0ULL);

  // Allocate every backed inode, then free them again: the chunk becomes
  // fully free and both the record and exactly the 16 backed blocks go away.
  std::vector<uint64_t> sparse_inos;
  for (int round = 0; round < 2; ++round) {
    auto txn = image.fs->BeginTransaction();
    CHECK(txn.ok());
    BlockAllocator alloc(&txn.value(), &dev, &geo);
    InodeAllocator inodes(&txn.value(), &alloc, &dev,
                          &image.fs->superblock(), &geo);
    for (uint32_t i = 0; i < 32; ++i) {
      if (round == 0) {
        uint64_t ino = 0;
        CHECK(inodes.AllocateInode(0, 0, &ino).ok());
        sparse_inos.push_back(ino);
      } else {
        CHECK(inodes.FreeInode(sparse_inos[i]).ok());
      }
    }
    CHECK(alloc.Commit().ok());
    CHECK(inodes.Commit().ok());
    CHECK(image.fs->CommitTransaction(txn.value()).ok());
  }

  records = ReadInobt(dev, geo, 0);
  for (const InobtRecord& r : records) CHECK_EQ(r.holemask, 0u);
  auto agi = ReadAgi(dev, geo, 0);
  CHECK(agi.ok());
  CHECK_EQ(agi.value().level, 1u);
  CHECK_EQ(agi.value().free_level, 1u);
  CHECK_EQ(agi.value().iblocks, 1u);
  CHECK_EQ(agi.value().fblocks, 1u);
  CHECK(image.fs->WriteCleanUnmountRecord().ok());
  ExpectCheckOk(image.fs->CheckConsistency(CheckOptions{}));
}

TEST(rw6_bigtime_reads_and_writes_the_full_range) {
  ModernImage image = MakeModernImage(300ull << 20);
  auto created = image.fs->CreateRegularFile(image.fs->RootInode(), "times",
                                             0644, 0, 0);
  CHECK(created.ok());
  const XfsTimestamp atime{-1, 500000000};
  const XfsTimestamp mtime{4102444800LL, 123456789};   // 2100-01-01
  const XfsTimestamp ctime{8000000000LL, 987654321};   // ~2223
  auto updated =
      image.fs->SetFileTimes(created.value(), atime, mtime, ctime);
  CHECK(updated.ok());
  CHECK_EQ(updated.value().atime.sec, atime.sec);
  CHECK_EQ(updated.value().atime.nsec, atime.nsec);
  CHECK_EQ(updated.value().mtime.sec, mtime.sec);
  CHECK_EQ(updated.value().mtime.nsec, mtime.nsec);
  CHECK_EQ(updated.value().ctime.sec, ctime.sec);
  CHECK_EQ(updated.value().ctime.nsec, ctime.nsec);

  auto inode = image.fs->OpenInode(created.value());
  CHECK(inode.ok());
  CHECK(inode.value().bigtime);
  CHECK(inode.value().nrext64);

  // The legacy encoding cannot represent 2100; the writer must refuse rather
  // than truncate the timestamp.
  ModernImage legacy = MakeModernImage(300ull << 20, 4096, 512, false, false,
                                       false, false, false);
  auto legacy_file =
      legacy.fs->CreateRegularFile(legacy.fs->RootInode(), "legacy", 0644, 0, 0);
  CHECK(legacy_file.ok());
  auto refused = legacy.fs->SetFileTimes(legacy_file.value(), mtime, mtime,
                                         mtime);
  CHECK(!refused.ok());
  CHECK_EQ(refused.error().code, XfsError::kOutOfRange);
}

TEST(rw6_nrext64_extent_counter_layout) {
  ModernImage image = MakeModernImage(300ull << 20);
  auto created = image.fs->CreateRegularFile(image.fs->RootInode(), "many",
                                             0644, 0, 0);
  CHECK(created.ok());
  const uint32_t block = image.fs->geometry().block_size;
  // 30 single-block writes separated by holes produce 30 distinct extents and
  // force the data fork into btree format.
  constexpr uint32_t kExtents = 30;
  for (uint32_t i = 0; i < kExtents; ++i) {
    const uint8_t byte = static_cast<uint8_t>(i);
    auto written = image.fs->WriteFileData(
        created.value(), static_cast<uint64_t>(i) * 2 * block, &byte, 1);
    CHECK(written.ok());
  }
  auto info = image.fs->GetFileInfo(created.value());
  CHECK(info.ok());
  CHECK_EQ(info.value().nextents, kExtents);

  auto inode = image.fs->OpenInode(created.value());
  CHECK(inode.ok());
  CHECK_EQ(inode.value().format, di::kFormatBtree);
  auto raw = ReadInodeImage(image.fs->device(), image.fs->geometry(),
                            created.value());
  CHECK((GetBE64(raw.data(), di::kFlags2) & di::kDiFlag2Nrext64) != 0);
  CHECK_EQ(GetBE64(raw.data(), di::kBigNextents),
           static_cast<uint64_t>(kExtents));
  CHECK_EQ(GetBE32(raw.data(), di::kBigAnextents), 0u);
  CHECK_EQ(GetBE16(raw.data(), di::kAnextents), 0u);  // pad
  CHECK_EQ(InodeDataExtentCount(raw.data()),
           static_cast<uint64_t>(kExtents));
  CHECK_EQ(InodeAttrExtentCount(raw.data()), 0u);
  CHECK(image.fs->WriteCleanUnmountRecord().ok());
  CheckReport report = image.fs->CheckConsistency(CheckOptions{});
  ExpectCheckOk(report);

  // Freeing an NREXT64 inode resets di_flags2 to new_diflags2 and must leave a
  // zero big count / pad, otherwise the next reader rejects the free inode.
  CHECK(image.fs->Unlink(image.fs->RootInode(), "many").ok());
  ExpectCheckOk(image.fs->CheckConsistency(CheckOptions{}));
}
