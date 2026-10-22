// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// RW-6B reader tests. These build a deliberately synthetic rmapbt so the
// parser can be exercised without a Linux runner; they do NOT claim writer or
// Linux interoperability, which require the external CI fixtures.
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/ag.h"
#include "xfs/alloc.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/formatter.h"
#include "xfs/ondisk.h"
#include "xfs/rmap.h"
#include "xfs/superblock.h"
#include "xfs/transaction.h"

using namespace xfs;

namespace {

namespace bt = ondisk::btree;
namespace agf = ondisk::agf;
namespace sb = ondisk::sb;

uint64_t PackOffset(uint64_t off, uint32_t flags) {
  uint64_t v = off & kRmapOffsetMask;
  if (flags & kRmapAttrFork) v |= kRmapOffsetAttr;
  if (flags & kRmapBmbtBlock) v |= kRmapOffsetBmbt;
  if (flags & kRmapUnwritten) v |= kRmapOffsetUnwritten;
  return v;
}

struct Synthetic {
  std::unique_ptr<xfstest::MemoryBlockDevice> device;
  XfsGeometry geo;
  uint32_t agbno = 0;
  std::vector<RmapRecord> records;
};

void PutRmapRec(uint8_t* p, const RmapRecord& r) {
  PutBE32(p, 0, r.key.startblock);
  PutBE32(p, 4, r.blockcount);
  PutBE64(p, 8, r.key.owner);
  PutBE64(p, 16, PackOffset(r.key.offset, r.key.flags));
}

void PutRmapKey(uint8_t* p, const RmapKey& k) {
  PutBE32(p, 0, k.startblock);
  PutBE64(p, 4, k.owner);
  PutBE64(p, 12, PackOffset(k.offset, k.flags));
}

RmapKey RecordHigh(const RmapRecord& r) {
  RmapKey k = r.key;
  const uint32_t adj = r.blockcount - 1;
  k.startblock += adj;
  if (!RmapNonInodeOwner(k.owner) && !(k.flags & kRmapBmbtBlock)) {
    k.offset += adj;
  }
  return k;
}

void WriteBlock(Synthetic& image, uint32_t agbno, uint16_t level,
                uint16_t numrecs, uint32_t left, uint32_t right) {
  std::vector<uint8_t> b(image.geo.block_size, 0);
  PutBE32(b.data(), bt::kMagic, ondisk::kRmapCrcMagic);
  PutBE16(b.data(), bt::kLevel, level);
  PutBE16(b.data(), bt::kNumrecs, numrecs);
  PutBE32(b.data(), bt::kSLeftsib, left);
  PutBE32(b.data(), bt::kSRightSib, right);
  PutBE64(b.data(), bt::kSBlkno,
          image.geo.FsbToByte(image.geo.AgFsb(0, agbno)) / 512);
  PutBE64(b.data(), bt::kSLsn, 0);
  std::memcpy(b.data() + bt::kSUuid, image.geo.meta_uuid.data(), 16);
  PutBE32(b.data(), bt::kSOwner, 0);
  CHECK(image.device->Write(
      image.geo.FsbToByte(image.geo.AgFsb(0, agbno)), b.data(), b.size()));
}

void FinishBlock(Synthetic& image, uint32_t agbno) {
  std::vector<uint8_t> b(image.geo.block_size, 0);
  const uint64_t off = image.geo.FsbToByte(image.geo.AgFsb(0, agbno));
  CHECK(image.device->Read(off, b.data(), b.size()));
  UpdateXfsChecksum(b.data(), b.size(), bt::kSCrc);
  CHECK(image.device->Write(off, b.data(), b.size()));
}

void PatchAgf(Synthetic& image, uint32_t root, uint32_t level,
              uint32_t blocks, uint32_t btreeblks) {
  const uint64_t off = image.geo.AgByteOffset(0) + AgfByteOffset(image.geo);
  std::vector<uint8_t> b(image.geo.sector_size, 0);
  CHECK(image.device->Read(off, b.data(), b.size()));
  PutBE32(b.data(), agf::kRmapRoot, root);
  PutBE32(b.data(), agf::kRmapLevel, level);
  PutBE32(b.data(), agf::kRmapBlocks, blocks);
  PutBE32(b.data(), agf::kBtreeblks, btreeblks);
  UpdateXfsChecksum(b.data(), b.size(), agf::kCrc);
  CHECK(image.device->Write(off, b.data(), b.size()));
}

Synthetic BuildSynthetic() {
  Synthetic out;
  out.device = std::make_unique<xfstest::MemoryBlockDevice>(300ull << 20);
  mkfs::FormatOptions options;
  options.block_size = 4096;
  options.inode_size = 512;
  options.quiet = true;
  CHECK(mkfs::Format(*out.device, options).ok());

  MountOptions mount_options;
  mount_options.read_write = true;
  auto mounted = XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(out.device.get()),
      mount_options);
  CHECK(mounted.ok());
  Allocation a;
  {
    auto txn = mounted.value()->BeginTransaction();
    CHECK(txn.ok());
    BlockAllocator alloc(&txn.value(), out.device.get(),
                         &mounted.value()->geometry());
    CHECK(alloc.AllocateFromAg(0, 3, 1, &a).ok());
    CHECK(alloc.Commit().ok());
    CHECK(mounted.value()->CommitTransaction(txn.value()).ok());
  }
  out.geo = mounted.value()->geometry();
  CHECK(a.fsb >> out.geo.agblklog == 0);
  out.agbno = static_cast<uint32_t>(a.fsb & ((1ull << out.geo.agblklog) - 1));
  const uint64_t rootino = mounted.value()->superblock().rootino;
  mounted.value().reset();

  // Enable RMAPBT for the reader. The synthetic tree below is intentionally
  // not a forward-derived ownership image; only the parser contract is tested.
  std::vector<uint8_t> super(out.geo.sector_size, 0);
  CHECK(out.device->Read(0, super.data(), super.size()));
  PutBE32(super.data(), sb::kFeaturesRoCompat,
          GetBE32(super.data(), sb::kFeaturesRoCompat) |
              ondisk::kRoCompatRmapbt);
  UpdateXfsChecksum(super.data(), super.size(), sb::kCrc);
  CHECK(out.device->Write(0, super.data(), super.size()));
  auto fresh = ReadSuperblock(*out.device, 0, true);
  CHECK(fresh.ok());
  out.geo = MakeGeometry(fresh.value());
  CHECK(out.geo.features.rmapbt);

  CHECK(out.agbno + 3 <= out.geo.AgBlocksFor(0));
  const uint32_t agfl_block =
      static_cast<uint32_t>((3ull * out.geo.sector_size) / out.geo.block_size);
  RmapRecord fs;
  fs.key.startblock = 0;
  fs.blockcount = agfl_block + 1;
  fs.key.owner = kRmapOwnerFs;
  out.records.push_back(fs);
  // 85 records per non-root leaf at 4 KiB (max leaf = 168, minimum = 84).
  for (uint32_t i = 0; i < 170; ++i) {
    RmapRecord r;
    r.key.startblock = agfl_block + 2 + i;
    r.key.owner = rootino;
    r.key.offset = i;
    r.key.flags = 0;
    r.blockcount = 1;
    out.records.push_back(r);
  }

  WriteBlock(out, out.agbno + 1, 0, 85, 0xffffffffu, out.agbno + 2);
  WriteBlock(out, out.agbno + 2, 0, 86, out.agbno + 1, 0xffffffffu);
  {
    std::vector<uint8_t> leaf(out.geo.block_size, 0);
    for (uint32_t leafno = 0; leafno < 2; ++leafno) {
      const uint32_t count = 85 + leafno;
      const uint32_t base = leafno == 0 ? 0 : 85;
      const uint64_t off = out.geo.FsbToByte(
          out.geo.AgFsb(0, out.agbno + 1 + leafno));
      CHECK(out.device->Read(off, leaf.data(), leaf.size()));
      for (uint32_t i = 0; i < count; ++i) {
        PutRmapRec(leaf.data() + bt::kSHeaderLenV5 + i * 24,
                   out.records[base + i]);
      }
      CHECK(out.device->Write(off, leaf.data(), leaf.size()));
      FinishBlock(out, out.agbno + 1 + leafno);
    }
  }

  WriteBlock(out, out.agbno, 1, 2, 0xffffffffu, 0xffffffffu);
  {
    std::vector<uint8_t> root(out.geo.block_size, 0);
    const uint64_t off = out.geo.FsbToByte(out.geo.AgFsb(0, out.agbno));
    CHECK(out.device->Read(off, root.data(), root.size()));
    const uint32_t max_node =
        static_cast<uint32_t>((out.geo.block_size - bt::kSHeaderLenV5) / 44);
    for (uint32_t i = 0; i < 2; ++i) {
      const RmapRecord& first = out.records[i == 0 ? 0 : 85];
      const RmapRecord& last = out.records[i == 0 ? 84 : 170];
      uint8_t* p = root.data() + bt::kSHeaderLenV5 + i * 40;
      PutRmapKey(p, first.key);
      PutRmapKey(p + 20, RecordHigh(last));
      PutBE32(root.data() + bt::kSHeaderLenV5 + max_node * 40 + i * 4, 0,
              out.agbno + 1 + i);
    }
    CHECK(out.device->Write(off, root.data(), root.size()));
    FinishBlock(out, out.agbno);
  }
  PatchAgf(out, out.agbno, 2, 3, 2);
  return out;
}

}  // namespace

TEST(rw6b_rmap_reader_typed_lookup_and_internal_keys) {
  Synthetic image = BuildSynthetic();
  auto tree = LoadRmapTree(*image.device, image.geo, 0);
  if (!tree.ok()) ::xfstest::Fail(__FILE__, __LINE__, tree.error().Format());
  CHECK_EQ(tree.value().level, 2u);
  CHECK_EQ(tree.value().blocks, 3u);
  CHECK_EQ(tree.value().records.size(), 171u);
  CHECK_EQ(tree.value().records.front().key.owner, kRmapOwnerFs);

  const RmapRecord& target = image.records[22];
  auto eq = tree.value().LookupEq(target.key);
  CHECK(eq.ok());
  CHECK_EQ(eq.value().key.startblock, target.key.startblock);
  CHECK_EQ(eq.value().key.offset, target.key.offset);

  auto le = tree.value().LookupLE(target.key);
  CHECK(le.ok());
  CHECK_EQ(le.value().key.startblock, target.key.startblock);
  auto ge = tree.value().LookupGE(target.key);
  CHECK(ge.ok());
  CHECK_EQ(ge.value().key.startblock, target.key.startblock);
  auto pred = tree.value().Predecessor(target.key);
  CHECK(pred.ok());
  CHECK_EQ(pred.value().key.startblock, image.records[21].key.startblock);
  auto succ = tree.value().Successor(target.key);
  CHECK(succ.ok());
  CHECK_EQ(succ.value().key.startblock, image.records[23].key.startblock);
}

TEST(rw6b_rmap_reader_rejects_record_and_key_corruption) {
  Synthetic image = BuildSynthetic();
  const std::vector<uint8_t> pristine = image.device->bytes();
  const uint64_t leaf_off =
      image.geo.FsbToByte(image.geo.AgFsb(0, image.agbno + 1));
  const uint64_t root_off =
      image.geo.FsbToByte(image.geo.AgFsb(0, image.agbno));
  const size_t rec0 = bt::kSHeaderLenV5;

  // Zero blockcount.
  {
    std::vector<uint8_t> b(pristine.data() + leaf_off,
                           pristine.data() + leaf_off + image.geo.block_size);
    PutBE32(b.data(), rec0 + 4, 0);
    UpdateXfsChecksum(b.data(), b.size(), bt::kSCrc);
    CHECK(image.device->Write(leaf_off, b.data(), b.size()));
    auto tree = LoadRmapTree(*image.device, image.geo, 0);
    CHECK(!tree.ok());
  }
  image.device->bytes() = pristine;

  // Unsorted records.
  {
    std::vector<uint8_t> b(pristine.data() + leaf_off,
                           pristine.data() + leaf_off + image.geo.block_size);
    PutBE32(b.data(), rec0 + 24, image.records[0].key.startblock);
    UpdateXfsChecksum(b.data(), b.size(), bt::kSCrc);
    CHECK(image.device->Write(leaf_off, b.data(), b.size()));
    auto tree = LoadRmapTree(*image.device, image.geo, 0);
    CHECK(!tree.ok());
  }
  image.device->bytes() = pristine;

  // Parent high key that does not match the child subtree.
  {
    std::vector<uint8_t> b(pristine.data() + root_off,
                           pristine.data() + root_off + image.geo.block_size);
    PutBE32(b.data(), bt::kSHeaderLenV5 + 20 + 0, 0x100);
    UpdateXfsChecksum(b.data(), b.size(), bt::kSCrc);
    CHECK(image.device->Write(root_off, b.data(), b.size()));
    auto tree = LoadRmapTree(*image.device, image.geo, 0);
    CHECK(!tree.ok());
  }
  image.device->bytes() = pristine;

  // AGF rmap_blocks counter disagrees with the reachable tree.
  {
    const uint64_t agf_off =
        image.geo.AgByteOffset(0) + AgfByteOffset(image.geo);
    std::vector<uint8_t> b(image.geo.sector_size, 0);
    CHECK(image.device->Read(agf_off, b.data(), b.size()));
    PutBE32(b.data(), agf::kRmapBlocks, 2);
    UpdateXfsChecksum(b.data(), b.size(), agf::kCrc);
    CHECK(image.device->Write(agf_off, b.data(), b.size()));
    auto tree = LoadRmapTree(*image.device, image.geo, 0);
    CHECK(!tree.ok());
  }
  image.device->bytes() = pristine;
  CHECK(LoadRmapTree(*image.device, image.geo, 0).ok());
}
