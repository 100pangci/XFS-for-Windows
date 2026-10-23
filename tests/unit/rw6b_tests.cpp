// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// RW-6B reader tests. These build a deliberately synthetic rmapbt so the
// parser can be exercised without a Linux runner; they do NOT claim writer or
// Linux interoperability, which require the external CI fixtures.
#include <algorithm>
#include <cstdio>
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
#include "xfs/log_format.h"
#include "xfs/log_record.h"
#include "xfs/recovery.h"
#include "xfs/rmap.h"
#include "xfs/rmap_editor.h"
#include "xfs/rmap_intent.h"
#include "xfs/superblock.h"
#include "xfs/transaction.h"

using namespace xfs;

namespace {

namespace bt = ondisk::btree;
namespace agf = ondisk::agf;
namespace sb = ondisk::sb;
namespace lf = logfmt;

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

// ---------------------------------------------------------------------------
// RW-6B writer tests
//
// These use a real, CRC-protected on-disk rmapbt (not a raw byte fixture):
// every logical edit goes through RmapEditor, every physical split/merge
// allocates or frees a tree block through the injected hooks, and the result
// is re-read with the independent typed reader.  The Linux-created fixture in
// CI remains the final interoperability oracle.
// ---------------------------------------------------------------------------

namespace {

struct WritableRmap {
  std::unique_ptr<xfstest::MemoryBlockDevice> device;
  XfsGeometry geo;
  uint64_t rootino = 0;
  uint32_t root = 4;
  uint32_t next_tree_block = 20000;
  std::vector<uint32_t> freed_tree_blocks;
};

void PatchRmapAgf(WritableRmap& image, uint32_t root, uint32_t level,
                  uint32_t blocks) {
  const uint64_t off = image.geo.AgByteOffset(0) + AgfByteOffset(image.geo);
  std::vector<uint8_t> b(image.geo.sector_size, 0);
  CHECK(image.device->Read(off, b.data(), b.size()));
  PutBE32(b.data(), agf::kRmapRoot, root);
  PutBE32(b.data(), agf::kRmapLevel, level);
  PutBE32(b.data(), agf::kRmapBlocks, blocks);
  PutBE32(b.data(), agf::kBtreeblks, blocks - 1);
  UpdateXfsChecksum(b.data(), b.size(), agf::kCrc);
  CHECK(image.device->Write(off, b.data(), b.size()));
}

void WriteRmapLeaf(WritableRmap& image, uint32_t agbno,
                   const std::vector<RmapRecord>& records) {
  std::vector<uint8_t> b(image.geo.block_size, 0);
  PutBE32(b.data(), bt::kMagic, ondisk::kRmapCrcMagic);
  PutBE16(b.data(), bt::kLevel, 0);
  PutBE16(b.data(), bt::kNumrecs, static_cast<uint16_t>(records.size()));
  PutBE32(b.data(), bt::kSLeftsib, 0xffffffffu);
  PutBE32(b.data(), bt::kSRightSib, 0xffffffffu);
  PutBE64(b.data(), bt::kSBlkno,
          image.geo.FsbToByte(image.geo.AgFsb(0, agbno)) / 512);
  PutBE64(b.data(), bt::kSLsn, 0);
  std::memcpy(b.data() + bt::kSUuid, image.geo.meta_uuid.data(), 16);
  PutBE32(b.data(), bt::kSOwner, 0);
  for (size_t i = 0; i < records.size(); ++i) {
    PutRmapRec(b.data() + bt::kSHeaderLenV5 + i * 24, records[i]);
  }
  UpdateXfsChecksum(b.data(), b.size(), bt::kSCrc);
  CHECK(image.device->Write(
      image.geo.FsbToByte(image.geo.AgFsb(0, agbno)), b.data(), b.size()));
}

WritableRmap BuildWritableRmap() {
  WritableRmap image;
  image.device = std::make_unique<xfstest::MemoryBlockDevice>(300ull << 20);
  mkfs::FormatOptions options;
  options.block_size = 1024;
  options.inode_size = 512;
  options.quiet = true;
  CHECK(mkfs::Format(*image.device, options).ok());

  {
    MountOptions mount_options;
    mount_options.read_write = true;
    auto mounted = XfsFilesystem::Mount(
        std::make_unique<xfstest::BorrowedBlockDevice>(image.device.get()),
        mount_options);
    CHECK(mounted.ok());
    image.rootino = mounted.value()->superblock().rootino;
    image.geo = mounted.value()->geometry();
    mounted.value().reset();
  }

  std::vector<uint8_t> super(image.geo.sector_size, 0);
  CHECK(image.device->Read(0, super.data(), super.size()));
  PutBE32(super.data(), sb::kFeaturesRoCompat,
          GetBE32(super.data(), sb::kFeaturesRoCompat) |
              ondisk::kRoCompatRmapbt);
  UpdateXfsChecksum(super.data(), super.size(), sb::kCrc);
  CHECK(image.device->Write(0, super.data(), super.size()));
  auto fresh = ReadSuperblock(*image.device, 0, true);
  CHECK(fresh.ok());
  image.geo = MakeGeometry(fresh.value());

  // The static AG header record is mandatory: xfs_rmap_unmap relies on it as
  // the left record for every lookup.
  const uint32_t agfl_block =
      static_cast<uint32_t>((3ull * image.geo.sector_size) /
                            image.geo.block_size);
  RmapRecord fs;
  fs.key.startblock = 0;
  fs.blockcount = agfl_block + 1;
  fs.key.owner = kRmapOwnerFs;
  WriteRmapLeaf(image, image.root, {fs});
  PatchRmapAgf(image, image.root, 1, 1);
  return image;
}

RmapEditor::Hooks EditorHooks(WritableRmap* image) {
  RmapEditor::Hooks hooks;
  hooks.alloc_block = [image](uint32_t, uint32_t* agbno) -> Status {
    *agbno = image->next_tree_block++;
    if (*agbno >= image->geo.AgBlocksFor(0)) {
      return XFS_ERR(XfsError::kNoSpace, "test rmap block pool exhausted")
          .Metadata("rmapbt");
    }
    return OkStatus();
  };
  hooks.free_block = [image](uint32_t, uint32_t agbno) -> Status {
    image->freed_tree_blocks.push_back(agbno);
    return OkStatus();
  };
  hooks.publish_block = [image](uint32_t, uint32_t agbno, const uint8_t* data,
                                size_t len) -> Status {
    std::vector<uint8_t> b(data, data + len);
    UpdateXfsChecksum(b.data(), b.size(), bt::kSCrc);
    const uint64_t offset =
        image->geo.FsbToByte(image->geo.AgFsb(0, agbno));
    if (!image->device->Write(offset, b.data(), b.size())) {
      return XFS_ERR(XfsError::kIoError, "test rmap publish failed")
          .Metadata("rmapbt");
    }
    return OkStatus();
  };
  return hooks;
}

std::unique_ptr<RmapEditor> LoadEditor(WritableRmap* image) {
  auto agf = ReadAgf(*image->device, image->geo, 0);
  CHECK(agf.ok());
  auto editor = std::make_unique<RmapEditor>(image->device.get(), &image->geo,
                                             EditorHooks(image));
  CHECK(editor->Load(0, agf.value().rmap_root, agf.value().rmap_level,
                     agf.value().rmap_blocks)
            .ok());
  return editor;
}

void Republish(WritableRmap* image, RmapEditor* editor) {
  CHECK(editor->Verify(0).ok());
  CHECK(editor->Commit().ok());
  PatchRmapAgf(*image, editor->Root(0), editor->Level(0),
               editor->Blocks(0));
}

RmapRecord DataRec(uint64_t owner, uint32_t start, uint32_t len,
                   uint64_t offset, bool unwritten = false) {
  RmapRecord rec;
  rec.key.startblock = start;
  rec.key.owner = owner;
  rec.key.offset = offset;
  rec.key.flags = unwritten ? kRmapUnwritten : 0;
  rec.blockcount = len;
  return rec;
}

}  // namespace

TEST(rw6b_rmap_editor_map_merge_matrix) {
  WritableRmap image = BuildWritableRmap();
  auto editor = LoadEditor(&image);
  const uint64_t ino = image.rootino;

  CHECK(editor->MapExtent(0, DataRec(ino, 100, 3, 10)).ok());
  CHECK(editor->MapExtent(0, DataRec(ino, 200, 2, 50)).ok());
  // Right neighbour: physical + logical contiguous with A.
  CHECK(editor->MapExtent(0, DataRec(ino, 103, 2, 13)).ok());
  // Left neighbour: extends A to [97, 8).
  CHECK(editor->MapExtent(0, DataRec(ino, 97, 3, 7)).ok());
  auto merged = editor->LookupEq(0, RmapKey{97, ino, 7, 0});
  CHECK(merged.ok());
  CHECK_EQ(merged.value().blockcount, 8u);
  // Physical adjacency alone is not enough: the logical offset jumps.
  CHECK(editor->MapExtent(0, DataRec(ino, 105, 2, 100)).ok());
  CHECK(editor->LookupEq(0, RmapKey{105, ino, 100, 0}).ok());
  // Different owner: never merged.
  CHECK(editor->MapExtent(0, DataRec(ino + 1, 107, 1, 17)).ok());
  CHECK(editor->LookupEq(0, RmapKey{107, ino + 1, 17, 0}).ok());
  // Same owner, different state: never merged.
  CHECK(editor->MapExtent(0, DataRec(ino, 108, 1, 18, true)).ok());
  CHECK(editor->LookupEq(0, RmapKey{108, ino, 18, 0}).ok());

  // Merge both sides.  C is inserted last so it has two neighbours.
  CHECK(editor->MapExtent(0, DataRec(ino, 300, 10, 300)).ok());
  CHECK(editor->MapExtent(0, DataRec(ino, 320, 10, 320)).ok());
  CHECK(editor->MapExtent(0, DataRec(ino, 310, 10, 310)).ok());
  auto both = editor->LookupEq(0, RmapKey{300, ino, 300, 0});
  CHECK(both.ok());
  CHECK_EQ(both.value().blockcount, 30u);
  CHECK(!editor->LookupEq(0, RmapKey{310, ino, 310, 0}).ok());

  Republish(&image, editor.get());
  auto tree = LoadRmapTree(*image.device, image.geo, 0);
  CHECK(tree.ok());
  auto record = tree.value().LookupEq(RmapKey{300, ino, 300, 0});
  CHECK(record.ok());
  CHECK_EQ(record.value().blockcount, 30u);
}

TEST(rw6b_rmap_editor_unmap_and_convert_matrix) {
  WritableRmap image = BuildWritableRmap();
  auto editor = LoadEditor(&image);
  const uint64_t ino = image.rootino;

  CHECK(editor->MapExtent(0, DataRec(ino, 100, 30, 100)).ok());
  // Middle removal leaves two residuals.
  CHECK(editor->UnmapExtent(0, RmapKey{110, ino, 110, 0}, 10).ok());
  CHECK(editor->LookupEq(0, RmapKey{100, ino, 100, 0}).value().blockcount == 10);
  CHECK(editor->LookupEq(0, RmapKey{120, ino, 120, 0}).value().blockcount == 10);
  // Prefix/suffix trims move both the physical and logical offsets.
  CHECK(editor->UnmapExtent(0, RmapKey{100, ino, 100, 0}, 5).ok());
  CHECK(editor->LookupEq(0, RmapKey{105, ino, 105, 0}).value().blockcount == 5);
  CHECK(editor->UnmapExtent(0, RmapKey{125, ino, 125, 0}, 5).ok());
  CHECK(editor->LookupEq(0, RmapKey{120, ino, 120, 0}).value().blockcount == 5);
  // A non-covering normal unmap is corruption, not a silent no-op.
  CHECK(!editor->UnmapExtent(0, RmapKey{100, ino, 100, 0}, 1).ok());

  // OWN_UNKNOWN is idempotent only when nothing overlaps.
  CHECK(editor->UnmapExtent(0, RmapKey{400, kRmapOwnerUnknown, 0, 0}, 10,
                            true)
            .ok());
  // Fully covered by an actual mapping: remove the real owner record.
  CHECK(editor->MapExtent(0, DataRec(ino, 500, 10, 500)).ok());
  CHECK(editor->UnmapExtent(0, RmapKey{500, kRmapOwnerUnknown, 0, 0}, 10,
                            true)
            .ok());
  CHECK(!editor->LookupEq(0, RmapKey{500, ino, 500, 0}).ok());
  // Extending past the covering record is partial overlap -> refuse.
  CHECK(editor->MapExtent(0, DataRec(ino, 600, 10, 600)).ok());
  CHECK(!editor->UnmapExtent(0, RmapKey{605, kRmapOwnerUnknown, 0, 0}, 20,
                             true)
             .ok());

  // Convert matrix: written target between two unwritten neighbours.
  CHECK(editor->MapExtent(0, DataRec(ino, 700, 10, 700, true)).ok());
  CHECK(editor->MapExtent(0, DataRec(ino, 720, 10, 720, true)).ok());
  CHECK(editor->MapExtent(0, DataRec(ino, 710, 10, 710, false)).ok());
  CHECK(editor->ConvertExtent(0, RmapKey{710, ino, 710, 0}, 10, false).ok());
  auto all_unwritten = editor->LookupEq(0, RmapKey{700, ino, 700, 0});
  CHECK(all_unwritten.ok());
  CHECK_EQ(all_unwritten.value().blockcount, 30u);
  CHECK(all_unwritten.value().unwritten());

  // Reverse: convert the middle back to written; it must split three ways.
  CHECK(editor->ConvertExtent(0, RmapKey{710, ino, 710, 0}, 10, true).ok());
  auto left = editor->LookupEq(0, RmapKey{700, ino, 700, 0});
  auto middle = editor->LookupEq(0, RmapKey{710, ino, 710, 0});
  auto right = editor->LookupEq(0, RmapKey{720, ino, 720, 0});
  CHECK(left.ok() && middle.ok() && right.ok());
  CHECK_EQ(left.value().blockcount, 10u);
  CHECK_EQ(middle.value().blockcount, 10u);
  CHECK_EQ(right.value().blockcount, 10u);
  CHECK(left.value().unwritten());
  CHECK(!middle.value().unwritten());
  CHECK(right.value().unwritten());

  // Prefix and suffix conversions keep the untouched state in place.
  CHECK(editor->MapExtent(0, DataRec(ino, 800, 10, 800, true)).ok());
  CHECK(editor->ConvertExtent(0, RmapKey{800, ino, 800, 0}, 3, true).ok());
  CHECK(!editor->LookupEq(0, RmapKey{800, ino, 800, 0}).value().unwritten());
  CHECK(editor->LookupEq(0, RmapKey{803, ino, 803, 0}).value().unwritten());
  CHECK(editor->ConvertExtent(0, RmapKey{807, ino, 807, 0}, 3, true).ok());
  CHECK(editor->LookupEq(0, RmapKey{803, ino, 803, 0}).value().blockcount == 4);
  CHECK(!editor->LookupEq(0, RmapKey{807, ino, 807, 0}).value().unwritten());

  Republish(&image, editor.get());
  auto tree = LoadRmapTree(*image.device, image.geo, 0);
  CHECK(tree.ok());
}

namespace {

void FlushTestTransaction(WritableRmap* image, XfsTransaction* txn) {
  for (const TransactionWrite& write : txn->writes()) {
    CHECK(image->device->Write(write.offset, write.data.data(),
                               write.data.size()));
  }
}

}  // namespace

namespace {

std::vector<uint8_t> MakeRui(uint64_t id, uint64_t owner, uint64_t startblock,
                             uint64_t startoff, uint32_t len, uint32_t flags,
                             uint32_t nextents = 1, size_t truncate = 0) {
  std::vector<uint8_t> rui(lf::kRuiFormatSize +
                               static_cast<size_t>(nextents) *
                                   lf::kRuiExtentSize,
                           0);
  StoreLE16(rui.data(), lf::kItemRui);
  StoreLE16(rui.data() + 2, 1);
  StoreLE32(rui.data() + lf::kIntentNextentsOffset, nextents);
  StoreLE64(rui.data() + lf::kIntentIdOffset, id);
  if (nextents != 0) {
    StoreLE64(rui.data() + lf::kRuiFormatSize + lf::kRuiOwnerOffset, owner);
    StoreLE64(rui.data() + lf::kRuiFormatSize + lf::kRuiStartblockOffset,
              startblock);
    StoreLE64(rui.data() + lf::kRuiFormatSize + lf::kRuiStartoffOffset,
              startoff);
    StoreLE32(rui.data() + lf::kRuiFormatSize + lf::kRuiLenOffset, len);
    StoreLE32(rui.data() + lf::kRuiFormatSize + lf::kRuiFlagsOffset, flags);
  }
  if (truncate != 0 && truncate < rui.size()) rui.resize(truncate);
  return rui;
}

}  // namespace

TEST(rw6b_rui_rud_preflight_matrix) {
  WritableRmap image = BuildWritableRmap();
  const uint64_t ino = image.rootino;
  const uint32_t agbno = 100;
  const uint64_t fsb = image.geo.AgFsb(0, agbno);

  auto valid = ParseRui(
      MakeRui(7, ino, fsb, 500, 4, lf::kRmapExtentMap), image.geo);
  CHECK(valid.ok());
  CHECK_EQ(valid.value().id, 7u);
  CHECK_EQ(valid.value().extents.size(), 1u);

  // Shared/reflink operations must be refused even before replay.
  auto shared = ParseRui(
      MakeRui(8, ino, fsb, 500, 4, lf::kRmapExtentMapShared), image.geo);
  CHECK(!shared.ok());
  CHECK(shared.error().code == XfsError::kUnsupportedFeature);

  // Structural matrix.
  CHECK(!ParseRui(MakeRui(9, ino, fsb, 500, 4, lf::kRmapExtentMap,
                          /*nextents=*/0),
                  image.geo)
             .ok());
  CHECK(!ParseRui(MakeRui(10, ino, fsb, 500, 4, lf::kRmapExtentMap, 1, 12),
                  image.geo)
             .ok());
  CHECK(!ParseRui(MakeRui(11, ino, fsb, 500, 0, lf::kRmapExtentMap),
                  image.geo)
             .ok());
  CHECK(!ParseRui(MakeRui(12, ino, 0xffffffffffffffffULL, 500, 4,
                          lf::kRmapExtentMap),
                  image.geo)
             .ok());
  CHECK(!ParseRui(MakeRui(13, ino, fsb,
                          (1ULL << 54) + (1ULL << 21), 4,
                          lf::kRmapExtentMap),
                  image.geo)
             .ok());
  CHECK(!ParseRui(MakeRui(14, 0, fsb, 0, 4, lf::kRmapExtentMap), image.geo)
             .ok());
  CHECK(!ParseRui(MakeRui(15, ino, fsb, 500, 4,
                          lf::kRmapExtentMap | (1u << 8)),
                  image.geo)
             .ok());
  CHECK(!ParseRui(MakeRui(16, ino, fsb, 500, 4, 0xff), image.geo).ok());
  // CONVERT cannot target attr/bmbt.
  CHECK(!ParseRui(MakeRui(17, ino, fsb, 0, 4,
                          lf::kRmapExtentConvert | lf::kRmapExtentBmbtBlock),
                  image.geo)
             .ok());
  // Metadata owners have zero offsets and no fork flags.
  CHECK(!ParseRui(MakeRui(18, kRmapOwnerAg, fsb, 4, 1, lf::kRmapExtentAlloc),
                  image.geo)
             .ok());
  // Reflink-only OWN_REFC/OWN_COW are unsupported.
  CHECK(ParseRui(MakeRui(19, kRmapOwnerRefc, fsb, 0, 1,
                         lf::kRmapExtentAlloc),
                 image.geo)
            .error()
            .code == XfsError::kUnsupportedFeature);

  std::vector<uint8_t> rud(lf::kRudFormatSize, 0);
  StoreLE16(rud.data(), lf::kItemRud);
  StoreLE16(rud.data() + 2, 1);
  StoreLE64(rud.data() + lf::kIntentIdOffset, 7);
  auto id = ParseRud(rud);
  CHECK(id.ok());
  CHECK_EQ(id.value(), 7u);
  StoreLE32(rud.data() + 4, 1);
  CHECK(!ParseRud(rud).ok());
  rud.resize(12);
  CHECK(!ParseRud(rud).ok());
}

TEST(rw6b_rui_rud_lifetime_is_ordered_and_type_keyed) {
  LogScan scan;
  auto add = [&](uint16_t type, uint64_t id, uint32_t tid) {
    RecoveredTransaction trans;
    trans.tid = tid;
    trans.lsn = MakeLsn(1, tid);
    RecoveredItem item;
    item.type = type;
    item.tid = tid;
    if (type == lf::kItemRui) {
      item.regions.push_back(MakeRui(id, 128, 0, 0, 1, lf::kRmapExtentMap));
    } else {
      std::vector<uint8_t> rud(lf::kRudFormatSize, 0);
      StoreLE16(rud.data(), type);
      StoreLE16(rud.data() + 2, 1);
      StoreLE64(rud.data() + lf::kIntentIdOffset, id);
      item.regions.push_back(std::move(rud));
    }
    trans.items.push_back(std::move(item));
    scan.transactions.push_back(std::move(trans));
  };
  add(lf::kItemRui, 1, 1);
  add(lf::kItemRui, 2, 2);
  add(lf::kItemRud, 1, 3);  // releases RUI 1
  add(lf::kItemRui, 1, 4);  // id reuse: a new active intent
  add(lf::kItemRud, 2, 5);  // releases RUI 2
  auto pending = CollectPendingIntents(scan);
  CHECK_EQ(pending.size(), 1u);
  if (!pending.empty()) {
    CHECK_EQ(pending[0].type, lf::kItemRui);
    const uint64_t id = static_cast<uint64_t>(LoadLE32(pending[0].regions[0].data() + 8)) |
                        (static_cast<uint64_t>(LoadLE32(pending[0].regions[0].data() + 12)) << 32);
    CHECK_EQ(id, 1u);
  }
  CHECK(ClassifyLogItem(lf::kItemRui) == RecoveryItemClass::kIntent);
  CHECK(ClassifyLogItem(lf::kItemRud) == RecoveryItemClass::kIntentDone);
}

TEST(rw6b_allocator_owned_rmap_roundtrip) {
  WritableRmap image;
  image.device = std::make_unique<xfstest::MemoryBlockDevice>(300ull << 20);
  mkfs::FormatOptions options;
  options.block_size = 4096;
  options.inode_size = 512;
  options.finobt = true;
  options.rmapbt = true;
  options.quiet = true;
  CHECK(mkfs::Format(*image.device, options).ok());

  auto sb = ReadSuperblock(*image.device, 0, true);
  CHECK(sb.ok());
  image.geo = MakeGeometry(sb.value());
  image.rootino = sb.value().rootino;
  CHECK(image.geo.features.rmapbt);

  // The formatter's rmap tree must satisfy the independent reader for every
  // AG before the allocator is allowed to mutate it.
  for (uint32_t agno = 0; agno < image.geo.agcount; ++agno) {
    auto tree = LoadRmapTree(*image.device, image.geo, agno);
    if (!tree.ok()) ::xfstest::Fail(__FILE__, __LINE__, tree.error().Format());
  }

  Allocation data;
  {
    XfsTransaction txn;
    BlockAllocator alloc(&txn, image.device.get(), &image.geo);
    CHECK(alloc
              .AllocateOwned(0, 5,
                             RmapOwner::Data(image.rootino, 123, false), &data)
              .ok());
    CHECK(alloc.Commit().ok());
    FlushTestTransaction(&image, &txn);
  }
  const uint32_t data_agno =
      static_cast<uint32_t>(data.fsb >> image.geo.agblklog);
  const uint32_t data_agbno =
      static_cast<uint32_t>(data.fsb & ((1ULL << image.geo.agblklog) - 1));
  {
    auto tree = LoadRmapTree(*image.device, image.geo, data_agno);
    CHECK(tree.ok());
    auto rec = tree.value().LookupEq(
        RmapKey{data_agbno, image.rootino, 123, 0});
    CHECK(rec.ok());
    CHECK_EQ(rec.value().blockcount, 5u);
  }

  {
    XfsTransaction txn;
    BlockAllocator alloc(&txn, image.device.get(), &image.geo);
    CHECK(alloc
              .FreeOwned(data.fsb, 5,
                         RmapOwner::Data(image.rootino, 123, false))
              .ok());
    CHECK(alloc.Commit().ok());
    FlushTestTransaction(&image, &txn);
  }
  {
    auto tree = LoadRmapTree(*image.device, image.geo, data_agno);
    CHECK(tree.ok());
    CHECK(!tree.value()
               .LookupEq(RmapKey{data_agbno, image.rootino, 123, 0})
               .ok());
  }

  // BMBT ownership, conversion and metadata ownership all go through the
  // same editor/allocator transaction.
  Allocation bmbt;
  Allocation conv;
  {
    XfsTransaction txn;
    BlockAllocator alloc(&txn, image.device.get(), &image.geo);
    CHECK(alloc
              .AllocateOwned(0, 1, RmapOwner::Bmbt(image.rootino), &bmbt)
              .ok());
    CHECK(alloc
              .AllocateOwned(0, 4,
                             RmapOwner::Data(image.rootino, 900, true), &conv)
              .ok());
    CHECK(alloc.Commit().ok());
    FlushTestTransaction(&image, &txn);
  }
  {
    const uint32_t agno = static_cast<uint32_t>(conv.fsb >> image.geo.agblklog);
    const uint32_t bno =
        static_cast<uint32_t>(conv.fsb & ((1ULL << image.geo.agblklog) - 1));
    const uint32_t bmbt_bno =
        static_cast<uint32_t>(bmbt.fsb & ((1ULL << image.geo.agblklog) - 1));
    auto tree = LoadRmapTree(*image.device, image.geo, agno);
    CHECK(tree.ok());
    auto bmbt_rec = tree.value().LookupEq(
        RmapKey{bmbt_bno, image.rootino, 0, kRmapBmbtBlock});
    CHECK(bmbt_rec.ok());
    auto conv_rec = tree.value().LookupEq(RmapKey{bno, image.rootino, 900, 0});
    CHECK(conv_rec.ok());
    CHECK(conv_rec.value().unwritten());

    XfsTransaction txn;
    BlockAllocator alloc(&txn, image.device.get(), &image.geo);
    CHECK(alloc
              .ConvertOwned(conv.fsb, 2,
                            RmapOwner::Data(image.rootino, 900, true), true)
              .ok());
    CHECK(alloc.Commit().ok());
    FlushTestTransaction(&image, &txn);
  }
  {
    const uint32_t agno = static_cast<uint32_t>(conv.fsb >> image.geo.agblklog);
    const uint32_t bno =
        static_cast<uint32_t>(conv.fsb & ((1ULL << image.geo.agblklog) - 1));
    auto tree = LoadRmapTree(*image.device, image.geo, agno);
    CHECK(tree.ok());
    auto written = tree.value().LookupEq(RmapKey{bno, image.rootino, 900, 0});
    CHECK(written.ok());
    CHECK_EQ(written.value().blockcount, 2u);
    CHECK(!written.value().unwritten());
    auto rest =
        tree.value().LookupEq(RmapKey{bno + 2, image.rootino, 902, 0});
    CHECK(rest.ok());
    CHECK_EQ(rest.value().blockcount, 2u);
    CHECK(rest.value().unwritten());
  }
}

TEST(rw6b_rmap_editor_tree_split_grow_merge_collapse) {
  WritableRmap image = BuildWritableRmap();
  auto editor = LoadEditor(&image);
  const uint64_t ino = image.rootino;
  const uint32_t before_level = editor->Level(0);
  uint32_t peak_level = before_level;
  uint32_t peak_blocks = editor->Blocks(0);

  // At 1 KiB: max_leaf = 40, max_node = 22.  2000 records need ~50 leaves,
  // which forces a node split and a third level via the real split path.
  constexpr uint32_t kCount = 2000;
  for (uint32_t i = 0; i < kCount; ++i) {
    const uint32_t start = 1000 + i * 2;
    CHECK(editor->MapExtent(0, DataRec(ino, start, 1, 10000 + i * 2)).ok());
    peak_level = std::max(peak_level, editor->Level(0));
    peak_blocks = std::max(peak_blocks, editor->Blocks(0));
  }
  std::printf("rw6b rmapbt levels: before=%u peak=%u after-insert=%u, "
              "blocks peak=%u\n",
              before_level, peak_level, editor->Level(0), peak_blocks);
  CHECK(peak_level >= 3u);
  CHECK(editor->stats().split_leaf > 0);
  CHECK(editor->stats().split_node > 0);
  CHECK(editor->stats().root_grow > 0);

  Republish(&image, editor.get());
  auto peak = LoadRmapTree(*image.device, image.geo, 0);
  CHECK(peak.ok());
  CHECK(peak.value().level >= 3u);
  CHECK_EQ(peak.value().records.size(), kCount + 1);
  CHECK_EQ(peak.value().records.front().key.owner, kRmapOwnerFs);

  // Delete most records in an order that leaves underfull siblings behind.
  for (uint32_t i = 1; i < kCount; i += 2) {
    const uint32_t start = 1000 + i * 2;
    CHECK(editor->DeleteExact(0, RmapKey{start, ino, 10000 + i * 2, 0}).ok());
  }
  for (uint32_t i = 0; i < kCount; i += 2) {
    if (i < 100) continue;
    const uint32_t start = 1000 + i * 2;
    CHECK(editor->DeleteExact(0, RmapKey{start, ino, 10000 + i * 2, 0}).ok());
  }
  const uint32_t after_level = editor->Level(0);
  std::printf("rw6b rmapbt levels: peak=%u after-delete=%u, blocks after=%u\n",
              peak_level, after_level, editor->Blocks(0));
  CHECK(editor->stats().merge_leaf > 0);
  CHECK(editor->stats().merge_node > 0);
  CHECK(editor->stats().root_collapse > 0);
  CHECK(after_level < peak_level);

  Republish(&image, editor.get());
  auto final_tree = LoadRmapTree(*image.device, image.geo, 0);
  CHECK(final_tree.ok());
  CHECK(final_tree.value().level == after_level);
  CHECK_EQ(final_tree.value().records.size(), 50u + 1u);
}
