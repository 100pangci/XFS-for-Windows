// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// RW-6C: reflink / refcountbt / CoW tests.
//
// These tests exercise the on-disk structures and the complete native clone,
// CoW and shared-lifetime paths on images created by this project's formatter.
// Linux-created images, genuine CUI/CUD log records and crash replay remain
// external CI gates; nothing here claims those gates.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/ag.h"
#include "xfs/alloc.h"
#include "xfs/bmap_intent.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/formatter.h"
#include "xfs/log_format.h"
#include "xfs/log_record.h"
#include "xfs/ondisk.h"
#include "xfs/recovery.h"
#include "xfs/refcount.h"
#include "xfs/refcount_editor.h"
#include "xfs/refcount_intent.h"
#include "xfs/rmap.h"
#include "xfs/superblock.h"
#include "xfs/transaction.h"

using namespace xfs;

namespace {

namespace bt = ondisk::btree;
namespace di = ondisk::inode;
namespace lf = logfmt;

constexpr uint32_t kNullAgBlock = bt::kNullAgBlock;

struct Image {
  std::unique_ptr<xfstest::MemoryBlockDevice> device;
  XfsGeometry geo;
  std::unique_ptr<XfsFilesystem> fs;
};

Image MakeImage(bool reflink, bool rmapbt, uint32_t block_size = 4096,
                bool mount = false) {
  Image image;
  image.device = std::make_unique<xfstest::MemoryBlockDevice>(300ull << 20);
  mkfs::FormatOptions options;
  options.block_size = block_size;
  options.inode_size = 512;
  options.quiet = true;
  options.reflink = reflink;
  options.rmapbt = rmapbt;
  auto formatted = mkfs::Format(*image.device, options);
  if (!formatted.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, formatted.error().Format());
  }
  auto sb = ReadSuperblock(*image.device, 0, true);
  CHECK(sb.ok());
  image.geo = MakeGeometry(sb.value());
  if (mount) {
    MountOptions mount_options;
    mount_options.read_write = true;
    auto borrowed =
        std::make_unique<xfstest::BorrowedBlockDevice>(image.device.get());
    auto mounted = XfsFilesystem::Mount(std::move(borrowed), mount_options);
    if (!mounted.ok()) {
      ::xfstest::Fail(__FILE__, __LINE__, mounted.error().Format());
    }
    image.fs = std::move(mounted.value());
  }
  return image;
}

std::vector<uint8_t> Pattern(size_t size, uint32_t seed = 0x6c2026) {
  std::vector<uint8_t> out(size);
  uint32_t x = seed;
  for (size_t i = 0; i < size; ++i) {
    x = x * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(x >> 24);
  }
  return out;
}

std::vector<uint8_t> ReadAt(const XfsFilesystem& fs, uint64_t ino,
                            uint64_t offset, size_t length) {
  std::vector<uint8_t> out(length);
  auto read = fs.ReadFile(ino, offset, out.data(), out.size());
  if (!read.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, read.error().Format());
  }
  if (read.value() != length) {
    char message[192];
    std::snprintf(message, sizeof(message),
                  "short read ino=%llu offset=%llu: %llu of %llu bytes",
                  static_cast<unsigned long long>(ino),
                  static_cast<unsigned long long>(offset),
                  static_cast<unsigned long long>(read.value()),
                  static_cast<unsigned long long>(length));
    ::xfstest::Fail(__FILE__, __LINE__, message);
  }
  return out;
}

void ExpectBytes(const std::vector<uint8_t>& actual,
                 const std::vector<uint8_t>& expected) {
  CHECK_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < actual.size(); ++i) {
    if (actual[i] != expected[i]) {
      char message[160];
      std::snprintf(message, sizeof(message),
                    "byte %llu differs: got 0x%02x expected 0x%02x",
                    static_cast<unsigned long long>(i), actual[i],
                    expected[i]);
      ::xfstest::Fail(__FILE__, __LINE__, message);
    }
  }
}

const XfsExtent* ExtentAt(const std::vector<XfsExtent>& extents,
                          uint64_t block) {
  for (const XfsExtent& extent : extents) {
    if (extent.startoff <= block && block < extent.EndBlock()) {
      return &extent;
    }
  }
  return nullptr;
}

RefcountRecord SharedRecord(BlockDevice& device, const XfsGeometry& geo,
                            uint64_t fsb, uint32_t len,
                            uint32_t expected_refcount) {
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo.agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo.agblklog) - 1));
  auto tree = LoadRefcountTree(device, geo, agno);
  CHECK(tree.ok());
  RefcountRecord rec;
  if (!tree.value().FindShared(agbno, len, &rec)) {
    char message[192];
    std::snprintf(message, sizeof(message),
                  "shared refcount record not found fsb=%llu len=%u ag=%u "
                  "block=%u",
                  static_cast<unsigned long long>(fsb), len, agno, agbno);
    ::xfstest::Fail(__FILE__, __LINE__, message);
  }
  CHECK_EQ(rec.key.startblock, agbno);
  CHECK(rec.blockcount >= len);
  if (rec.refcount != expected_refcount) {
    char message[192];
    std::snprintf(message, sizeof(message),
                  "fsb=%llu len=%u ag=%u block=%u: refcount %u != %u "
                  "(record start=%u len=%u)",
                  static_cast<unsigned long long>(fsb), len, agno, agbno,
                  rec.refcount, expected_refcount, rec.key.startblock,
                  rec.blockcount);
    ::xfstest::Fail(__FILE__, __LINE__, message);
  }
  return rec;
}

void ExpectNoSharedRecord(BlockDevice& device, const XfsGeometry& geo,
                          uint64_t fsb, uint32_t len) {
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo.agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo.agblklog) - 1));
  auto tree = LoadRefcountTree(device, geo, agno);
  CHECK(tree.ok());
  CHECK(!tree.value().OverlapsShared(agbno, len));
}

void InitRefcountLeaf(const XfsGeometry& geo, uint32_t agno, uint32_t agbno,
                      const std::vector<RefcountRecord>& records,
                      std::vector<uint8_t>* out) {
  out->assign(geo.block_size, 0);
  PutBE32(out->data(), bt::kMagic, ondisk::kRefcountCrcMagic);
  PutBE16(out->data(), bt::kLevel, 0);
  PutBE16(out->data(), bt::kNumrecs,
          static_cast<uint16_t>(records.size()));
  PutBE32(out->data(), bt::kSLeftsib, kNullAgBlock);
  PutBE32(out->data(), bt::kSRightSib, kNullAgBlock);
  PutBE64(out->data(), bt::kSBlkno,
          geo.FsbToByte(geo.AgFsb(agno, agbno)) / 512);
  PutBE64(out->data(), bt::kSLsn, 0);
  std::memcpy(out->data() + bt::kSUuid, geo.meta_uuid.data(), 16);
  PutBE32(out->data(), bt::kSOwner, agno);
  for (size_t i = 0; i < records.size(); ++i) {
    uint8_t* p = out->data() + bt::kSHeaderLenV5 + i * bt::kRefcountRecSize;
    PutBE32(p, 0, records[i].key.Encoded());
    PutBE32(p, 4, records[i].blockcount);
    PutBE32(p, 8, records[i].refcount);
  }
  UpdateXfsChecksum(out->data(), out->size(), bt::kSCrc);
}

void WriteRefcountLeaf(Image& image, uint32_t agno,
                       const std::vector<RefcountRecord>& records) {
  auto agf = ReadAgf(*image.device, image.geo, agno);
  CHECK(agf.ok());
  CHECK_EQ(agf.value().refcount_level, 1u);
  std::vector<uint8_t> block;
  InitRefcountLeaf(image.geo, agno, agf.value().refcount_root, records,
                   &block);
  const uint64_t offset = image.geo.FsbToByte(
      image.geo.AgFsb(agno, agf.value().refcount_root));
  CHECK(image.device->Write(offset, block.data(), block.size()));
}

template <typename ImageT>
void PatchRefcountAgf(ImageT& image, uint32_t agno, uint32_t root,
                      uint32_t level, uint32_t blocks) {
  const uint64_t offset =
      image.geo.AgByteOffset(agno) + AgfByteOffset(image.geo);
  std::vector<uint8_t> block(image.geo.sector_size, 0);
  CHECK(image.device->Read(offset, block.data(), block.size()));
  PutBE32(block.data(), ondisk::agf::kRefcountRoot, root);
  PutBE32(block.data(), ondisk::agf::kRefcountLevel, level);
  PutBE32(block.data(), ondisk::agf::kRefcountBlocks, blocks);
  UpdateXfsChecksum(block.data(), block.size(), ondisk::agf::kCrc);
  CHECK(image.device->Write(offset, block.data(), block.size()));
}

struct EditorImage {
  std::unique_ptr<xfstest::MemoryBlockDevice> device;
  XfsGeometry geo;
  uint32_t next_block = 5000;
  std::vector<uint32_t> freed;
};

EditorImage MakeEditorImage(uint32_t block_size = 1024) {
  EditorImage out;
  out.device = std::make_unique<xfstest::MemoryBlockDevice>(300ull << 20);
  mkfs::FormatOptions options;
  options.block_size = block_size;
  options.inode_size = 512;
  options.quiet = true;
  options.reflink = true;
  CHECK(mkfs::Format(*out.device, options).ok());
  auto sb = ReadSuperblock(*out.device, 0, true);
  CHECK(sb.ok());
  out.geo = MakeGeometry(sb.value());
  return out;
}

RefcountEditor::Hooks EditorHooks(EditorImage* image) {
  RefcountEditor::Hooks hooks;
  hooks.alloc_block = [image](uint32_t agno, uint32_t* agbno) -> Status {
    if (image->next_block >= image->geo.AgBlocksFor(agno)) {
      return XFS_ERR(XfsError::kNoSpace, "test tree block pool exhausted")
          .Metadata("refcountbt");
    }
    *agbno = image->next_block++;
    return OkStatus();
  };
  hooks.free_block = [image](uint32_t, uint32_t agbno) -> Status {
    image->freed.push_back(agbno);
    return OkStatus();
  };
  hooks.publish_block = [image](uint32_t agno, uint32_t agbno,
                                const uint8_t* data,
                                size_t len) -> Status {
    std::vector<uint8_t> block(data, data + len);
    UpdateXfsChecksum(block.data(), block.size(), bt::kSCrc);
    const uint64_t offset =
        image->geo.FsbToByte(image->geo.AgFsb(agno, agbno));
    if (!image->device->Write(offset, block.data(), block.size())) {
      return XFS_ERR(XfsError::kIoError, "test refcount publish failed")
          .Metadata("refcountbt");
    }
    return OkStatus();
  };
  return hooks;
}

std::unique_ptr<RefcountEditor> LoadEditor(EditorImage* image,
                                           std::vector<uint32_t>* freed) {
  auto agf = ReadAgf(*image->device, image->geo, 0);
  CHECK(agf.ok());
  RefcountEditor::Hooks hooks = EditorHooks(image);
  if (freed != nullptr) {
    hooks.free_extent = [freed](uint32_t, uint32_t startblock,
                                uint32_t blockcount) -> Status {
      freed->push_back(blockcount);
      (void)startblock;
      return OkStatus();
    };
  }
  auto editor = std::make_unique<RefcountEditor>(image->device.get(),
                                                 &image->geo, hooks);
  CHECK(editor
            ->Load(0, agf.value().refcount_root, agf.value().refcount_level,
                   agf.value().refcount_blocks)
            .ok());
  return editor;
}

void Republish(EditorImage* image, RefcountEditor* editor) {
  CHECK(editor->Verify(0).ok());
  CHECK(editor->Commit().ok());
  PatchRefcountAgf(*image, 0, editor->Root(0), editor->Level(0),
                   editor->Blocks(0));
}

RefcountRecord MustLookup(RefcountEditor& editor, uint32_t agno,
                          RefcountDomain domain, uint32_t start) {
  auto record =
      editor.LookupEq(agno, RefcountKey{domain, start});
  if (!record.ok()) {
    char message[160];
    std::snprintf(message, sizeof(message),
                  "refcount %s record at %u not found", ToString(domain),
                  start);
    ::xfstest::Fail(__FILE__, __LINE__, message);
  }
  return record.value();
}

RefcountRecord Shared(uint32_t start, uint32_t len, uint32_t refcount) {
  RefcountRecord rec;
  rec.key.domain = RefcountDomain::kShared;
  rec.key.startblock = start;
  rec.blockcount = len;
  rec.refcount = refcount;
  return rec;
}

RefcountRecord Cow(uint32_t start, uint32_t len) {
  RefcountRecord rec;
  rec.key.domain = RefcountDomain::kCow;
  rec.key.startblock = start;
  rec.blockcount = len;
  rec.refcount = 1;
  return rec;
}

std::vector<LogOp> CommitOps(uint32_t tid,
                             std::vector<std::vector<uint8_t>> regions) {
  std::vector<LogOp> ops;
  ops.push_back(LogOp(lf::kFlagStartTrans, {}));
  std::vector<uint8_t> header;
  AppendTransHeader(&header, tid, static_cast<uint32_t>(regions.size()));
  ops.push_back(LogOp(0, std::move(header)));
  for (auto& region : regions) {
    ops.push_back(LogOp(0, std::move(region)));
  }
  ops.push_back(LogOp(lf::kFlagCommitTrans, {}));
  return ops;
}

std::vector<uint8_t> MakeCui(uint64_t id, uint64_t startblock, uint32_t len,
                             uint32_t flags, uint32_t nextents = 1) {
  std::vector<uint8_t> region(lf::kIntentLogFormatSize +
                                  static_cast<size_t>(nextents) * 16,
                              0);
  StoreLE16(region.data(), lf::kItemCui);
  StoreLE16(region.data() + 2, 1);
  StoreLE32(region.data() + lf::kIntentNextentsOffset, nextents);
  StoreLE64(region.data() + lf::kIntentIdOffset, id);
  if (nextents != 0) {
    StoreLE64(region.data() + lf::kIntentLogFormatSize, startblock);
    StoreLE32(region.data() + lf::kIntentLogFormatSize + 8, len);
    StoreLE32(region.data() + lf::kIntentLogFormatSize + 12, flags);
  }
  return region;
}

std::vector<uint8_t> MakeBui(uint64_t id, uint64_t owner, uint64_t startblock,
                             uint64_t startoff, uint32_t len, uint32_t flags) {
  std::vector<uint8_t> region(lf::kIntentLogFormatSize + 32, 0);
  StoreLE16(region.data(), lf::kItemBui);
  StoreLE16(region.data() + 2, 1);
  StoreLE32(region.data() + lf::kIntentNextentsOffset, 1);
  StoreLE64(region.data() + lf::kIntentIdOffset, id);
  StoreLE64(region.data() + lf::kIntentLogFormatSize, owner);
  StoreLE64(region.data() + lf::kIntentLogFormatSize + 8, startblock);
  StoreLE64(region.data() + lf::kIntentLogFormatSize + 16, startoff);
  StoreLE32(region.data() + lf::kIntentLogFormatSize + 24, len);
  StoreLE32(region.data() + lf::kIntentLogFormatSize + 28, flags);
  return region;
}

std::vector<uint8_t> MakeBud(uint64_t id) {
  std::vector<uint8_t> region(16, 0);
  StoreLE16(region.data(), lf::kItemBud);
  StoreLE16(region.data() + 2, 1);
  StoreLE64(region.data() + lf::kIntentIdOffset, id);
  return region;
}

std::vector<uint8_t> MakeCud(uint64_t id) {
  std::vector<uint8_t> region(16, 0);
  StoreLE16(region.data(), lf::kItemCud);
  StoreLE16(region.data() + 2, 1);
  StoreLE64(region.data() + lf::kIntentIdOffset, id);
  return region;
}

}  // namespace

// ---------------------------------------------------------------------------
// Formatter and raw reader
// ---------------------------------------------------------------------------

TEST(rw6c_formatter_both_reflink_profiles) {
  for (bool rmapbt : {false, true}) {
    Image image = MakeImage(true, rmapbt);
    CHECK(image.geo.features.reflink);
    CHECK_EQ(image.geo.features.rmapbt, rmapbt);
    for (uint32_t agno = 0; agno < image.geo.agcount; ++agno) {
      auto agf = ReadAgf(*image.device, image.geo, agno);
      CHECK(agf.ok());
      CHECK_EQ(agf.value().refcount_level, 1u);
      CHECK_EQ(agf.value().refcount_blocks, 1u);
      auto tree = LoadRefcountTree(*image.device, image.geo, agno);
      if (!tree.ok()) {
        ::xfstest::Fail(__FILE__, __LINE__, tree.error().Format());
      }
      CHECK_EQ(tree.value().root, agf.value().refcount_root);
      CHECK_EQ(tree.value().level, 1u);
      CHECK_EQ(tree.value().blocks, 1u);
      CHECK(tree.value().records.empty());
      if (rmapbt) {
        auto rmap = LoadRmapTree(*image.device, image.geo, agno);
        CHECK(rmap.ok());
        const uint64_t encoded =
            image.geo.AgFsb(agno, agf.value().refcount_root);
        (void)encoded;
        auto owner = rmap.value().LookupEq(
            RmapKey{agf.value().refcount_root, kRmapOwnerRefc, 0, 0});
        CHECK(owner.ok());
      }
    }
  }

  Image none = MakeImage(false, false);
  CHECK(!none.geo.features.reflink);
  CHECK(!LoadRefcountTree(*none.device, none.geo, 0).ok());
}

TEST(rw6c_reader_validation_and_lookup_matrix) {
  Image image = MakeImage(true, false);
  std::vector<RefcountRecord> records = {
      Shared(100, 4, 2), Shared(200, 3, 3), Cow(300, 2)};
  WriteRefcountLeaf(image, 0, records);
  auto tree = LoadRefcountTree(*image.device, image.geo, 0);
  if (!tree.ok()) ::xfstest::Fail(__FILE__, __LINE__, tree.error().Format());
  CHECK_EQ(tree.value().records.size(), 3u);
  CHECK_EQ(tree.value().records[0].refcount, 2u);
  CHECK(tree.value().records[2].cow());
  CHECK(tree.value().OverlapsShared(101, 2));
  CHECK(!tree.value().OverlapsShared(104, 96));
  RefcountRecord part;
  CHECK(tree.value().FindShared(202, 4, &part));
  CHECK_EQ(part.key.startblock, 202u);
  CHECK_EQ(part.blockcount, 1u);
  CHECK_EQ(part.refcount, 3u);
  CHECK(tree.value().FindCow(299, 4, &part));
  CHECK_EQ(part.key.startblock, 300u);
  CHECK_EQ(part.blockcount, 2u);
  CHECK_EQ(tree.value().LookupEq(RefcountKey{RefcountDomain::kCow, 300})
               .value()
               .blockcount,
           2u);
  CHECK(!tree.value().LookupEq(RefcountKey{RefcountDomain::kCow, 301}).ok());
  CHECK_EQ(tree.value().Predecessor(RefcountKey{RefcountDomain::kShared, 300})
               .value()
               .key.startblock,
           200u);
  CHECK_EQ(tree.value().Successor(RefcountKey{RefcountDomain::kShared, 100})
               .value()
               .key.startblock,
           200u);

  auto agf = ReadAgf(*image.device, image.geo, 0);
  CHECK(agf.ok());
  const uint32_t root = agf.value().refcount_root;
  const uint64_t root_offset =
      image.geo.FsbToByte(image.geo.AgFsb(0, root));
  std::vector<uint8_t> pristine;
  InitRefcountLeaf(image.geo, 0, root, records, &pristine);

  auto install = [&](std::vector<uint8_t> block) {
    UpdateXfsChecksum(block.data(), block.size(), bt::kSCrc);
    CHECK(image.device->Write(root_offset, block.data(), block.size()));
  };
  auto corrupt_record = [&](size_t index, size_t field, uint32_t value) {
    std::vector<uint8_t> block = pristine;
    PutBE32(block.data(),
            bt::kSHeaderLenV5 + index * bt::kRefcountRecSize + field, value);
    install(std::move(block));
    CHECK(!LoadRefcountTree(*image.device, image.geo, 0).ok());
  };

  corrupt_record(0, 0, 0x80000000u);       // shared with the CoW bit
  corrupt_record(0, 4, 0);                 // zero length
  corrupt_record(0, 8, 1);                 // shared refcount below two
  corrupt_record(2, 8, 2);                 // CoW refcount above one

  {
    std::vector<uint8_t> block = pristine;
    PutBE32(block.data(),
            bt::kSHeaderLenV5 + 1 * bt::kRefcountRecSize, 300);  // unsorted
    install(std::move(block));
    CHECK(!LoadRefcountTree(*image.device, image.geo, 0).ok());
  }
  {
    std::vector<uint8_t> block = pristine;
    PutBE32(block.data(),
            bt::kSHeaderLenV5 + 1 * bt::kRefcountRecSize + 4, 200);
    install(std::move(block));
    CHECK(!LoadRefcountTree(*image.device, image.geo, 0).ok());
  }
  install(pristine);
  CHECK(LoadRefcountTree(*image.device, image.geo, 0).ok());
  PatchRefcountAgf(image, 0, root, 1, 2);
  CHECK(!LoadRefcountTree(*image.device, image.geo, 0).ok());
}

// ---------------------------------------------------------------------------
// Editor: logical adjustments and B+tree structure
// ---------------------------------------------------------------------------

TEST(rw6c_refcount_editor_adjustment_matrix) {
  EditorImage image = MakeEditorImage();
  std::vector<uint32_t> freed;
  auto editor = LoadEditor(&image, &freed);

  CHECK(editor->Increase(0, 100, 5).ok());
  auto first = editor->LookupEq(0, RefcountKey{RefcountDomain::kShared, 100});
  CHECK(first.ok());
  CHECK_EQ(first.value().blockcount, 5u);
  CHECK_EQ(first.value().refcount, 2u);

  CHECK(editor->Increase(0, 102, 4).ok());
  CHECK_EQ(MustLookup(*editor, 0, RefcountDomain::kShared, 100).blockcount,
           2u);
  CHECK_EQ(MustLookup(*editor, 0, RefcountDomain::kShared, 102).blockcount,
           3u);
  CHECK_EQ(MustLookup(*editor, 0, RefcountDomain::kShared, 102).refcount, 3u);
  CHECK_EQ(MustLookup(*editor, 0, RefcountDomain::kShared, 105).blockcount,
           1u);
  CHECK_EQ(MustLookup(*editor, 0, RefcountDomain::kShared, 105).refcount, 2u);

  CHECK(editor->Increase(0, 99, 1).ok());
  auto merged = MustLookup(*editor, 0, RefcountDomain::kShared, 99);
  CHECK_EQ(merged.blockcount, 3u);
  CHECK_EQ(merged.refcount, 2u);

  CHECK(editor->Decrease(0, 103, 2).ok());
  CHECK_EQ(MustLookup(*editor, 0, RefcountDomain::kShared, 102).refcount,
           3u);
  CHECK_EQ(MustLookup(*editor, 0, RefcountDomain::kShared, 103).refcount,
           2u);
  CHECK_EQ(MustLookup(*editor, 0, RefcountDomain::kShared, 103).blockcount,
           3u);

  // Decrease an implied refcount-1 hole: the range reaches zero and is freed.
  freed.clear();
  CHECK(editor->Decrease(0, 400, 3).ok());
  CHECK_EQ(freed.size(), 1u);
  CHECK_EQ(freed[0], 3u);

  CHECK(editor->Increase(0, 600, 1).ok());
  RefcountRecord pinned = Shared(600, 1, kRefcountMax);
  CHECK(editor
            ->UpdateExact(
                0, MustLookup(*editor, 0, RefcountDomain::kShared, 600),
                pinned)
            .ok());
  CHECK(editor->Increase(0, 600, 1).ok());
  CHECK_EQ(MustLookup(*editor, 0, RefcountDomain::kShared, 600).refcount,
           kRefcountMax);
  CHECK(editor->Decrease(0, 600, 1).ok());
  CHECK_EQ(MustLookup(*editor, 0, RefcountDomain::kShared, 600).refcount,
           kRefcountMax);

  // CoW is a separate domain with its own flag and refcount-one rule.
  CHECK(editor->CowAlloc(0, 700, 4).ok());
  RefcountRecord cow;
  CHECK(editor->FindCow(0, 702, 1, &cow));
  CHECK_EQ(cow.key.startblock, 702u);
  CHECK_EQ(cow.blockcount, 1u);
  CHECK(!editor->OverlapsShared(0, 702, 1));
  CHECK(!editor->Increase(0, 699, 2).ok());   // shared over CoW
  CHECK(!editor->CowAlloc(0, 701, 1).ok());   // CoW over CoW/shared
  CHECK(editor->CowFree(0, 700, 2).ok());     // split at the left boundary
  CHECK(!editor->FindCow(0, 700, 2, &cow));
  CHECK(editor->FindCow(0, 702, 2, &cow));
  CHECK_EQ(cow.blockcount, 2u);
  CHECK(!editor->CowFree(0, 700, 2).ok());    // already removed
  CHECK(editor->CowFree(0, 702, 2).ok());
  CHECK(!editor->FindCow(0, 700, 4, &cow));
  CHECK(!editor->Increase(0, 700, 0).ok());

  // Decrease to exactly one reference deletes the record without freeing.
  freed.clear();
  CHECK(editor->Decrease(0, 99, 3).ok());
  CHECK(freed.empty());
  CHECK(!editor->LookupEq(0, RefcountKey{RefcountDomain::kShared, 99}).ok());

  Republish(&image, editor.get());
  auto tree = LoadRefcountTree(*image.device, image.geo, 0);
  if (!tree.ok()) ::xfstest::Fail(__FILE__, __LINE__, tree.error().Format());
  CHECK(!tree.value().records.empty());
}

TEST(rw6c_refcount_editor_split_grow_merge_collapse) {
  EditorImage image = MakeEditorImage(1024);
  auto editor = LoadEditor(&image, nullptr);
  const uint32_t initial_level = editor->Level(0);
  uint32_t peak_level = initial_level;
  uint32_t peak_blocks = editor->Blocks(0);

  // At 1 KiB: max_leaf = 80, max_node = 121.  10000 records force multiple
  // internal splits and a third level through the real B+tree path.
  constexpr uint32_t kCount = 10000;
  for (uint32_t i = 0; i < kCount; ++i) {
    CHECK(editor->InsertExact(0, Shared(1000 + i * 2, 1, 2 + (i % 3))).ok());
    peak_level = std::max(peak_level, editor->Level(0));
    peak_blocks = std::max(peak_blocks, editor->Blocks(0));
  }
  std::printf(
      "rw6c refcountbt levels: initial=%u peak=%u blocks=%u split_leaf=%llu "
      "split_node=%llu root_grow=%llu\n",
      initial_level, peak_level, peak_blocks,
      static_cast<unsigned long long>(editor->stats().split_leaf),
      static_cast<unsigned long long>(editor->stats().split_node),
      static_cast<unsigned long long>(editor->stats().root_grow));
  CHECK(peak_level >= 3u);
  CHECK(editor->stats().split_leaf > 0);
  CHECK(editor->stats().split_node > 0);
  CHECK(editor->stats().root_grow > 0);
  Republish(&image, editor.get());

  auto peak = LoadRefcountTree(*image.device, image.geo, 0);
  CHECK(peak.ok());
  CHECK(peak.value().level >= 3u);
  CHECK_EQ(peak.value().records.size(), kCount);

  // Delete front-to-back, which repeatedly empties leaves and forces borrow,
  // merge and eventually root collapse.
  for (uint32_t i = 0; i + 70 < kCount; ++i) {
    CHECK(editor
              ->DeleteExact(0, RefcountKey{RefcountDomain::kShared,
                                           1000 + i * 2})
              .ok());
    if ((i % 1000) == 0) CHECK(editor->Verify(0).ok());
  }
  CHECK(editor->stats().merge_leaf > 0);
  CHECK(editor->stats().borrow_leaf > 0);
  CHECK(editor->stats().root_collapse > 0);
  CHECK_EQ(editor->Level(0), 1u);
  CHECK_EQ(editor->Records(0).size(), 70u);
  Republish(&image, editor.get());

  auto final_tree = LoadRefcountTree(*image.device, image.geo, 0);
  if (!final_tree.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, final_tree.error().Format());
  }
  CHECK_EQ(final_tree.value().level, 1u);
  CHECK_EQ(final_tree.value().records.size(), 70u);
  CHECK(!image.freed.empty());
}

// ---------------------------------------------------------------------------
// Native clone / CoW / shared lifetime
// ---------------------------------------------------------------------------

TEST(rw6c_clone_cow_truncate_lifecycle_both_rmap_profiles) {
  for (bool rmapbt : {false, true}) {
    Image image = MakeImage(true, rmapbt, 4096, true);
    XfsFilesystem& fs = *image.fs;
    auto src = fs.CreateRegularFile(fs.RootInode(), "src", 0644, 0, 0);
    auto dst = fs.CreateRegularFile(fs.RootInode(), "dst", 0644, 0, 0);
    CHECK(src.ok());
    CHECK(dst.ok());
    const std::vector<uint8_t> data = Pattern(4 * 4096);
    CHECK(fs.WriteFileData(src.value(), 0, data.data(), data.size()).ok());
    ExpectBytes(ReadAt(fs, src.value(), 0, data.size()), data);

    auto cloned = fs.ReflinkRange(src.value(), 0, dst.value(), 0, 0);
    CHECK(cloned.ok());
    CHECK_EQ(cloned.value(), data.size());
    ExpectBytes(ReadAt(fs, dst.value(), 0, data.size()), data);

    auto src_extents = fs.GetDataExtents(src.value());
    auto dst_extents = fs.GetDataExtents(dst.value());
    CHECK(src_extents.ok());
    CHECK(dst_extents.ok());
    CHECK_EQ(src_extents.value().size(), 1u);
    CHECK_EQ(dst_extents.value().size(), 1u);
    CHECK_EQ(src_extents.value()[0].startblock,
             dst_extents.value()[0].startblock);
    CHECK_EQ(src_extents.value()[0].blockcount, 4u);
    SharedRecord(fs.device(), fs.geometry(),
                 src_extents.value()[0].startblock, 4, 2);
    if (rmapbt) {
      const uint32_t agno = static_cast<uint32_t>(
          src_extents.value()[0].startblock >> fs.geometry().agblklog);
      auto rmap = LoadRmapTree(fs.device(), fs.geometry(), agno);
      CHECK(rmap.ok());
      auto owner = rmap.value().LookupEq(RmapKey{
          static_cast<uint32_t>(src_extents.value()[0].startblock &
                                ((1ULL << fs.geometry().agblklog) - 1)),
          dst.value(), 0, 0});
      CHECK(owner.ok());
    }

    // A CoW write isolates one destination block.  The source and the other
    // destination blocks must keep their old bytes and sharing.
    const std::vector<uint8_t> cow_data = Pattern(4096, 0xdeadbeef);
    CHECK(fs.WriteFileData(dst.value(), 4096, cow_data.data(),
                           cow_data.size())
              .ok());
    ExpectBytes(ReadAt(fs, dst.value(), 4096, 4096), cow_data);
    ExpectBytes(ReadAt(fs, src.value(), 0, data.size()), data);
    auto after = fs.GetDataExtents(dst.value());
    CHECK(after.ok());
    const XfsExtent* private_block = ExtentAt(after.value(), 1);
    CHECK(private_block != nullptr);
    CHECK(private_block->startblock !=
          src_extents.value()[0].startblock + 1);
    SharedRecord(fs.device(), fs.geometry(),
                 src_extents.value()[0].startblock, 1, 2);
    ExpectNoSharedRecord(fs.device(), fs.geometry(),
                         private_block->startblock, 1);

    // Unlinking the source drops its references but the destination's shared
    // mappings and its private CoW block must keep both data and extent state.
    auto unlinked = fs.Unlink(fs.RootInode(), "src");
    CHECK(unlinked.ok());
    std::vector<uint8_t> patched = data;
    std::memcpy(patched.data() + 4096, cow_data.data(), cow_data.size());
    ExpectBytes(ReadAt(fs, dst.value(), 0, data.size()), patched);
    if (rmapbt) {
      auto rmap = LoadRmapTree(
          fs.device(), fs.geometry(),
          static_cast<uint32_t>(src_extents.value()[0].startblock >>
                                fs.geometry().agblklog));
      CHECK(rmap.ok());
      CHECK(!rmap.value()
                 .LookupEq(RmapKey{
                     static_cast<uint32_t>(
                         src_extents.value()[0].startblock &
                         ((1ULL << fs.geometry().agblklog) - 1)),
                     src.value(), 0, 0})
                 .ok());
    }
    // Truncating the remaining destination releases the last shared
    // references.  There is no surviving owner for the former source blocks.
    auto truncated = fs.SetFileSize(dst.value(), 0);
    CHECK(truncated.ok());
    CHECK_EQ(truncated.value().size, 0u);
    ExpectNoSharedRecord(fs.device(), fs.geometry(),
                         src_extents.value()[0].startblock, 4);
    CHECK(fs.WriteCleanUnmountRecord().ok());
  }
}

TEST(rw6c_clone_holes_unwritten_and_partial_eof) {
  Image image = MakeImage(true, true, 4096, true);
  XfsFilesystem& fs = *image.fs;

  auto src = fs.CreateRegularFile(fs.RootInode(), "sparse-src", 0644, 0, 0);
  auto dst = fs.CreateRegularFile(fs.RootInode(), "sparse-dst", 0644, 0, 0);
  CHECK(src.ok());
  CHECK(dst.ok());
  const std::vector<uint8_t> middle = Pattern(2 * 4096, 0x1234);
  CHECK(fs.WriteFileData(src.value(), 3 * 4096, middle.data(),
                         middle.size())
            .ok());
  auto unwritten = fs.AllocateUnwritten(src.value(), 6 * 4096, 2 * 4096);
  CHECK(unwritten.ok());
  CHECK_EQ(unwritten.value().size, 8ull * 4096);

  CHECK(fs.ReflinkRange(src.value(), 0, dst.value(), 0, 0).ok());
  auto dst_extents = fs.GetDataExtents(dst.value());
  CHECK(dst_extents.ok());
  CHECK_EQ(dst_extents.value().size(), 1u);
  CHECK_EQ(dst_extents.value()[0].startoff, 3u);
  CHECK_EQ(dst_extents.value()[0].blockcount, 2u);
  CHECK(!dst_extents.value()[0].unwritten);
  ExpectBytes(ReadAt(fs, dst.value(), 0, 3 * 4096),
              std::vector<uint8_t>(3 * 4096, 0));
  ExpectBytes(ReadAt(fs, dst.value(), 3 * 4096, middle.size()), middle);
  ExpectBytes(ReadAt(fs, dst.value(), 5 * 4096, 3 * 4096),
              std::vector<uint8_t>(3 * 4096, 0));

  auto partial = fs.CreateRegularFile(fs.RootInode(), "partial-src", 0644, 0,
                                      0);
  auto partial_dst = fs.CreateRegularFile(fs.RootInode(), "partial-dst", 0644,
                                          0, 0);
  auto grown = fs.CreateRegularFile(fs.RootInode(), "grown-dst", 0644, 0, 0);
  CHECK(partial.ok());
  CHECK(partial_dst.ok());
  CHECK(grown.ok());
  const size_t partial_size = 4 * 4096 + 1234;
  const std::vector<uint8_t> partial_data = Pattern(partial_size, 0x7777);
  CHECK(fs.WriteFileData(partial.value(), 0, partial_data.data(),
                         partial_data.size())
            .ok());
  CHECK(fs.ReflinkRange(partial.value(), 0, partial_dst.value(), 0, 0).ok());
  ExpectBytes(ReadAt(fs, partial_dst.value(), 0, partial_size), partial_data);
  auto p_ext = fs.GetDataExtents(partial.value());
  CHECK(p_ext.ok());
  CHECK_EQ(p_ext.value().size(), 1u);
  CHECK_EQ(p_ext.value()[0].blockcount, 5u);
  SharedRecord(fs.device(), fs.geometry(), p_ext.value()[0].startblock, 5, 2);

  // A partial final block may not be placed inside a larger destination.
  CHECK(fs.SetFileSize(grown.value(), 8 * 4096).ok());
  auto refused = fs.ReflinkRange(partial.value(), 0, grown.value(), 0, 0);
  CHECK(!refused.ok());
  CHECK(refused.error().code == XfsError::kInvalidArgument);

  // Growing the destination over a shared partial block must CoW-zero the
  // newly visible bytes and leave the source's shared block untouched.
  auto dest_grown = fs.SetFileSize(partial_dst.value(), 5 * 4096 + 100);
  CHECK(dest_grown.ok());
  CHECK_EQ(dest_grown.value().size, 5ull * 4096 + 100);
  ExpectBytes(ReadAt(fs, partial_dst.value(), 0, partial_size), partial_data);
  ExpectBytes(ReadAt(fs, partial_dst.value(), partial_size, 4096 + 100 -
                                                            1234),
              std::vector<uint8_t>(4096 + 100 - 1234, 0));
  ExpectBytes(ReadAt(fs, partial.value(), 0, partial_size), partial_data);
  SharedRecord(fs.device(), fs.geometry(), p_ext.value()[0].startblock, 4, 2);
  ExpectNoSharedRecord(fs.device(), fs.geometry(),
                       p_ext.value()[0].startblock + 4, 1);

  // A pure-hole source keeps a local fork.  It is still a legal all-hole
  // clone, and the destination's old extents must be released.
  auto all_hole = fs.CreateRegularFile(fs.RootInode(), "all-hole", 0644, 0, 0);
  auto hole_dst = fs.CreateRegularFile(fs.RootInode(), "hole-dst", 0644, 0, 0);
  CHECK(all_hole.ok() && hole_dst.ok());
  CHECK(fs.SetFileSize(all_hole.value(), 3 * 4096).ok());
  const std::vector<uint8_t> old_data = Pattern(2 * 4096, 0x5151);
  CHECK(fs.WriteFileData(hole_dst.value(), 0, old_data.data(),
                         old_data.size())
            .ok());
  CHECK(fs.ReflinkRange(all_hole.value(), 0, hole_dst.value(), 0, 0).ok());
  auto hole_extents = fs.GetDataExtents(hole_dst.value());
  CHECK(hole_extents.ok());
  CHECK(hole_extents.value().empty());
  ExpectBytes(ReadAt(fs, hole_dst.value(), 0, 3 * 4096),
              std::vector<uint8_t>(3 * 4096, 0));
  CHECK(fs.WriteCleanUnmountRecord().ok());
}

TEST(rw6c_three_way_clone_and_shared_release) {
  Image image = MakeImage(true, true, 4096, true);
  XfsFilesystem& fs = *image.fs;
  auto src = fs.CreateRegularFile(fs.RootInode(), "origin", 0644, 0, 0);
  auto a = fs.CreateRegularFile(fs.RootInode(), "clone-a", 0644, 0, 0);
  auto b = fs.CreateRegularFile(fs.RootInode(), "clone-b", 0644, 0, 0);
  CHECK(src.ok() && a.ok() && b.ok());
  const std::vector<uint8_t> data = Pattern(3 * 4096, 0xabc);
  CHECK(fs.WriteFileData(src.value(), 0, data.data(), data.size()).ok());
  auto ext = fs.GetDataExtents(src.value());
  CHECK(ext.ok());
  const uint64_t fsb = ext.value()[0].startblock;
  CHECK(fs.ReflinkRange(src.value(), 0, a.value(), 0, 0).ok());
  CHECK(fs.ReflinkRange(src.value(), 0, b.value(), 0, 0).ok());
  SharedRecord(fs.device(), fs.geometry(), fsb, 3, 3);

  const std::vector<uint8_t> patch = Pattern(4096, 0xc0ffee);
  CHECK(fs.WriteFileData(a.value(), 4096, patch.data(), patch.size()).ok());
  SharedRecord(fs.device(), fs.geometry(), fsb, 1, 3);
  SharedRecord(fs.device(), fs.geometry(), fsb + 1, 1, 2);
  SharedRecord(fs.device(), fs.geometry(), fsb + 2, 1, 3);
  auto a_extents = fs.GetDataExtents(a.value());
  CHECK(a_extents.ok());
  const XfsExtent* a_private = ExtentAt(a_extents.value(), 1);
  CHECK(a_private != nullptr);
  CHECK(a_private->startblock != fsb + 1);
  ExpectNoSharedRecord(fs.device(), fs.geometry(), a_private->startblock, 1);

  CHECK(fs.SetFileSize(b.value(), 0).ok());
  ExpectBytes(ReadAt(fs, src.value(), 0, data.size()), data);
  SharedRecord(fs.device(), fs.geometry(), fsb, 1, 2);
  ExpectNoSharedRecord(fs.device(), fs.geometry(), fsb + 1, 1);
  SharedRecord(fs.device(), fs.geometry(), fsb + 2, 1, 2);

  CHECK(fs.Unlink(fs.RootInode(), "origin").ok());
  std::vector<uint8_t> a_expected = data;
  std::memcpy(a_expected.data() + 4096, patch.data(), patch.size());
  ExpectBytes(ReadAt(fs, a.value(), 0, data.size()), a_expected);
  ExpectNoSharedRecord(fs.device(), fs.geometry(), fsb, 3);
  CHECK(fs.Unlink(fs.RootInode(), "clone-a").ok());
  CHECK(fs.WriteCleanUnmountRecord().ok());
}

TEST(rw6c_bmbt_insert_shifts_before_splitting) {
  const auto run = [](uint32_t block_size, uint32_t extents) {
    Image image = MakeImage(true, false, block_size, true);
    XfsFilesystem& fs = *image.fs;
    auto file = fs.CreateRegularFile(fs.RootInode(), "shift.bin", 0644, 0, 0);
    CHECK(file.ok());
    // Appending one block every other block leaves a hole between mappings, so
    // every write inserts one single-block extent at the end of the file.
    // After the first leaf split, the last leaf fills again; the next insert
    // must move its first record into the left sibling instead of allocating a
    // third bmbt block (xfs_btree_make_block_unfull: rshift, lshift, split).
    std::vector<uint8_t> payload = Pattern(block_size, 0x51f7);
    for (uint32_t i = 0; i < extents; ++i) {
      const uint64_t offset =
          static_cast<uint64_t>(i) * 2 * block_size;
      CHECK(fs.WriteFileData(file.value(), offset, payload.data(),
                             payload.size())
                .ok());
    }
    auto inode = fs.OpenInode(file.value());
    CHECK(inode.ok());
    if (!inode.ok()) return;
    CHECK_EQ(inode.value().format, di::kFormatBtree);
    CHECK_EQ(inode.value().nextents, extents);
    // Data blocks plus exactly two leaf blocks.  A split-on-every-full-leaf
    // tree would have three leaf blocks here, at both 1 KiB and 4 KiB.
    CHECK_EQ(inode.value().nblocks,
             static_cast<uint64_t>(extents) + 2);
    auto data = fs.GetDataExtents(file.value());
    CHECK(data.ok());
    CHECK_EQ(data.value().size(), extents);
    CHECK(fs.WriteCleanUnmountRecord().ok());
  };
  run(1024, 100);
  run(4096, 380);
}

TEST(rw6c_many_shared_extents_grow_and_collapse_real_tree) {
  Image image = MakeImage(true, true, 1024, true);
  XfsFilesystem& fs = *image.fs;
  constexpr uint32_t kPairs = 100;
  std::vector<uint64_t> sources;
  std::vector<std::string> destination_names;
  std::vector<uint8_t> payload = Pattern(1024, 0x9911);
  for (uint32_t i = 0; i < kPairs; ++i) {
    char src_name[32];
    char gap_name[32];
    char dst_name[32];
    std::snprintf(src_name, sizeof(src_name), "s%03u", i);
    std::snprintf(gap_name, sizeof(gap_name), "g%03u", i);
    std::snprintf(dst_name, sizeof(dst_name), "d%03u", i);
    auto src = fs.CreateRegularFile(fs.RootInode(), src_name, 0644, 0, 0);
    auto gap = fs.CreateRegularFile(fs.RootInode(), gap_name, 0644, 0, 0);
    auto dst = fs.CreateRegularFile(fs.RootInode(), dst_name, 0644, 0, 0);
    CHECK(src.ok() && gap.ok() && dst.ok());
    CHECK(fs.WriteFileData(src.value(), 0, payload.data(), payload.size())
              .ok());
    // The gap file's private block keeps the next source extent physically
    // separate, so every clone adds its own refcount record.
    CHECK(fs.WriteFileData(gap.value(), 0, payload.data(), payload.size())
              .ok());
    CHECK(fs.ReflinkRange(src.value(), 0, dst.value(), 0, 0).ok());
    sources.push_back(src.value());
    destination_names.emplace_back(dst_name);
  }

  auto tree = LoadRefcountTree(fs.device(), fs.geometry(), 0);
  if (!tree.ok()) ::xfstest::Fail(__FILE__, __LINE__, tree.error().Format());
  std::printf("rw6c real tree: level=%u blocks=%u records=%zu\n",
              tree.value().level, tree.value().blocks,
              tree.value().records.size());
  CHECK(tree.value().level >= 2u);
  CHECK(tree.value().blocks > 1u);
  CHECK(tree.value().records.size() >= kPairs);
  auto agf = ReadAgf(fs.device(), fs.geometry(), 0);
  CHECK(agf.ok());
  CHECK_EQ(agf.value().refcount_level, tree.value().level);
  CHECK_EQ(agf.value().refcount_blocks, tree.value().blocks);
  auto rmap = LoadRmapTree(fs.device(), fs.geometry(), 0);
  CHECK(rmap.ok());

  // Drop every destination: each shared extent loses one owner, so its record
  // disappears and the tree merges/collapses back to an empty root leaf.
  for (uint32_t i = 0; i < kPairs; ++i) {
    CHECK(fs.Unlink(fs.RootInode(), destination_names[i]).ok());
  }
  for (int pass = 0; pass < 2; ++pass) {
    auto final_tree = LoadRefcountTree(fs.device(), fs.geometry(), 0);
    if (!final_tree.ok()) {
      ::xfstest::Fail(__FILE__, __LINE__, final_tree.error().Format());
    }
    CHECK(final_tree.value().level == 1u);
    CHECK(final_tree.value().blocks == 1u);
    CHECK(final_tree.value().records.empty());
    if (pass == 0) {
      // Source files still carry the original private mappings.
      for (uint32_t i = 0; i < kPairs; ++i) {
        ExpectBytes(ReadAt(fs, sources[i], 0, payload.size()), payload);
      }
    }
  }
  CHECK(fs.WriteCleanUnmountRecord().ok());
}

TEST(rw6c_ordinary_reflink_write_gate_is_open) {
  // The public qualification ledger is complete; a plain --rw-experimental
  // mount now accepts the reflink profile and can create/write a file.
  Image image = MakeImage(true, true, 4096, true);
  XfsFilesystem& fs = *image.fs;
  auto file = fs.CreateRegularFile(fs.RootInode(), "ordinary.bin", 0644, 0, 0);
  CHECK(file.ok());
  if (!file.ok()) return;
  std::vector<uint8_t> data = Pattern(4096, 0x6f70656e);
  CHECK(fs.WriteFileData(file.value(), 0, data.data(), data.size()).ok());
  CHECK(fs.WriteCleanUnmountRecord().ok());
}

// ---------------------------------------------------------------------------
// CoW staging recovery
// ---------------------------------------------------------------------------

TEST(rw6c_orphan_cow_staging_cleanup_on_mount) {
  for (bool rmapbt : {false, true}) {
    Image image = MakeImage(true, rmapbt, 4096, true);
    Allocation staging;
    CHECK(image.fs
              ->RunTransaction([&](XfsTransaction& txn) -> Status {
                BlockAllocator alloc(&txn, &image.fs->device(),
                                     &image.fs->geometry());
                Status s = alloc.AllocateCowStaging(0, 3, &staging);
                if (!s.ok()) return s;
                return alloc.Commit();
              })
              .ok());
    const uint32_t agno =
        static_cast<uint32_t>(staging.fsb >> image.geo.agblklog);
    const uint32_t agbno = static_cast<uint32_t>(
        staging.fsb & ((1ULL << image.geo.agblklog) - 1));
    {
      auto tree = LoadRefcountTree(image.fs->device(), image.geo, agno);
      CHECK(tree.ok());
      RefcountRecord cow;
      CHECK(tree.value().FindCow(agbno, 3, &cow));
      CHECK_EQ(cow.refcount, 1u);
    }
    if (rmapbt) {
      auto rmap = LoadRmapTree(image.fs->device(), image.geo, agno);
      CHECK(rmap.ok());
      CHECK(rmap.value()
                .LookupEq(RmapKey{agbno, kRmapOwnerCow, 0, 0})
                .ok());
    }
    auto staged_agf = ReadAgf(*image.device, image.geo, agno);
    CHECK(staged_agf.ok());
    CHECK(image.fs->WriteCleanUnmountRecord().ok());
    image.fs.reset();

    MountOptions mount_options;
    mount_options.read_write = true;
    auto remounted = XfsFilesystem::Mount(
        std::make_unique<xfstest::BorrowedBlockDevice>(image.device.get()),
        mount_options);
    if (!remounted.ok()) {
      ::xfstest::Fail(__FILE__, __LINE__, remounted.error().Format());
    }
    {
      auto tree = LoadRefcountTree(*image.device, image.geo, agno);
      CHECK(tree.ok());
      RefcountRecord cow;
      CHECK(!tree.value().FindCow(agbno, 3, &cow));
      CHECK_EQ(tree.value().blocks, 1u);
    }
    if (rmapbt) {
      auto rmap = LoadRmapTree(*image.device, image.geo, agno);
      CHECK(rmap.ok());
      CHECK(!rmap.value()
                 .LookupEq(RmapKey{agbno, kRmapOwnerCow, 0, 0})
                 .ok());
    }
    auto agf = ReadAgf(*image.device, image.geo, agno);
    CHECK(agf.ok());
    CHECK(agf.value().freeblks >= staged_agf.value().freeblks + 3);
    CHECK(remounted.value()->WriteCleanUnmountRecord().ok());
  }
}

TEST(rw6c_recovery_close_after_orphan_cow_cleanup) {
  Image image = MakeImage(true, true, 4096, true);
  Allocation staging;
  CHECK(image.fs
            ->RunTransaction([&](XfsTransaction& txn) -> Status {
              BlockAllocator alloc(&txn, &image.fs->device(),
                                   &image.fs->geometry());
              Status s = alloc.AllocateCowStaging(0, 2, &staging);
              if (!s.ok()) return s;
              return alloc.Commit();
            })
            .ok());
  // Deliberately crash-close: no clean-unmount record and no orphan cleanup.
  image.fs.reset();

  MountOptions recovery;
  recovery.read_write = true;
  recovery.recovery_only = true;
  recovery.clean_log_after_recovery = true;
  auto mounted = XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(image.device.get()),
      recovery);
  if (!mounted.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, mounted.error().Format());
  }
  CHECK(mounted.value()->recovery_report().has_value());
  CHECK(mounted.value()->FinishRecoveryClean().ok());
  mounted.value().reset();

  // A read-only mount classifies the journal as clean and no staging record
  // survives: the close happened after the CoW cleanup transaction.
  MountOptions readonly;
  auto reopened = XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(image.device.get()),
      readonly);
  if (!reopened.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, reopened.error().Format());
  }
  auto journal = reopened.value()->AnalyzeJournal();
  CHECK(journal.ok());
  CHECK(journal.value().state == LogState::kClean);
  const uint32_t agno =
      static_cast<uint32_t>(staging.fsb >> image.geo.agblklog);
  const uint32_t agbno = static_cast<uint32_t>(
      staging.fsb & ((1ULL << image.geo.agblklog) - 1));
  auto tree = LoadRefcountTree(*image.device, image.geo, agno);
  CHECK(tree.ok());
  RefcountRecord cow;
  CHECK(!tree.value().FindCow(agbno, 2, &cow));
}

// ---------------------------------------------------------------------------
// CUI/CUD parsing, lifetime and R3FC inventory
// ---------------------------------------------------------------------------

TEST(rw6c_bui_parser_and_bmap_completion) {
  Image image = MakeImage(true, true, 4096, true);
  const uint64_t root = image.fs->RootInode();
  auto src = image.fs->CreateRegularFile(root, "bui-src", 0644, 0, 0);
  auto dst = image.fs->CreateRegularFile(root, "bui-dst", 0644, 0, 0);
  CHECK(src.ok() && dst.ok());
  Allocation pool;
  CHECK(image.fs
            ->RunTransaction([&](XfsTransaction& txn) -> Status {
              BlockAllocator alloc(&txn, &image.fs->device(),
                                   &image.fs->geometry());
              Status s = alloc.AllocateOwned(
                  0, 8, RmapOwner::Data(src.value(), 0, false), &pool);
              if (!s.ok()) return s;
              return alloc.Commit();
            })
            .ok());
  CHECK(image.fs->WriteCleanUnmountRecord().ok());
  image.fs.reset();

  auto sb = ReadSuperblock(*image.device, 0, true);
  CHECK(sb.ok());
  const XfsGeometry geo = MakeGeometry(sb.value());
  auto append = [&](const std::vector<uint8_t>& item, uint32_t tid) -> bool {
    auto position = ReadLogPosition(*image.device, geo, sb.value());
    if (!position.ok()) {
      ::xfstest::Fail(__FILE__, __LINE__, position.error().Format());
    }
    LogRecordParams params;
    params.cycle = position.value().cycle;
    params.block = position.value().next_block;
    params.tid = tid;
    params.prev_block = position.value().prev_block;
    params.tail_lsn = MakeLsn(position.value().cycle,
                              position.value().next_block);
    params.log_blocks = position.value().log_blocks;
    params.record_size = position.value().record_size;
    params.uuid = geo.meta_uuid;
    auto record = BuildLogRecord(params, CommitOps(tid, {item}));
    if (!record.ok()) {
      ::xfstest::Fail(__FILE__, __LINE__, record.error().Format());
    }
    CHECK(image.device->Write(
        geo.FsbToByte(sb.value().logstart) +
            static_cast<uint64_t>(params.block) * 512,
        record.value().data(), record.value().size()));
    CHECK(image.device->Flush());
    return true;
  };
  auto recover = [&]() -> std::unique_ptr<XfsFilesystem> {
    MountOptions options;
    options.read_write = true;
    options.recovery_only = true;
    options.clean_log_after_recovery = true;
    auto mounted = XfsFilesystem::Mount(
        std::make_unique<xfstest::BorrowedBlockDevice>(image.device.get()),
        options);
    if (!mounted.ok()) {
      ::xfstest::Fail(__FILE__, __LINE__, mounted.error().Format());
    }
    CHECK(mounted.value()->FinishRecoveryClean().ok());
    return std::move(mounted.value());
  };

  const uint64_t bui_id = 0x6d0001;
  const std::vector<uint8_t> map_item =
      MakeBui(bui_id, dst.value(), pool.fsb, 0, 2, 1);
  auto parsed = ParseBmapIntent(map_item, geo);
  CHECK(parsed.ok());
  CHECK_EQ(parsed.value().extents[0].owner, dst.value());
  CHECK(parsed.value().extents[0].type() == BmapIntentType::kMap);
  CHECK(!ParseBmapIntent(
             MakeBui(1, dst.value(), pool.fsb, 0, 2,
                     kBmapIntentAttrFork | 1),
             geo)
             .ok());
  CHECK(!ParseBmapIntent(
             MakeBui(1, dst.value(), pool.fsb, 0, 2,
                     kBmapIntentRealtime | 1),
             geo)
             .ok());
  CHECK(!ParseBmapIntent(MakeBui(1, dst.value(), pool.fsb, 0, 2, 3), geo)
             .ok());
  CHECK(ParseBmapIntentDone(MakeBud(1)).ok());
  CHECK(!ParseBmapIntentDone(std::vector<uint8_t>(12, 0)).ok());

  CHECK(append(map_item, 0x6d01));
  {
    auto mounted = recover();
    CHECK(mounted->recovery_report().has_value());
    CHECK_EQ(mounted->recovery_report()->intents_completed, 1u);
    auto extents = mounted->GetDataExtents(dst.value());
    CHECK(extents.ok());
    CHECK_EQ(extents.value().size(), 1u);
    if (!extents.value().empty()) {
      CHECK_EQ(extents.value()[0].startblock, pool.fsb);
      CHECK_EQ(extents.value()[0].blockcount, 2u);
      CHECK(!extents.value()[0].unwritten);
    }
    const uint32_t agno = static_cast<uint32_t>(pool.fsb >> geo.agblklog);
    const uint32_t agbno = static_cast<uint32_t>(
        pool.fsb & ((1ULL << geo.agblklog) - 1));
    auto rmap = LoadRmapTree(*image.device, geo, agno);
    CHECK(rmap.ok());
    CHECK(rmap.value()
              .LookupEq(RmapKey{agbno, dst.value(), 0, 0})
              .ok());
    CHECK(rmap.value()
              .LookupEq(RmapKey{agbno, src.value(), 0, 0})
              .ok());
    mounted.reset();
  }

  const std::vector<uint8_t> unmap_item =
      MakeBui(bui_id + 1, dst.value(), pool.fsb, 0, 2, 2);
  CHECK(append(unmap_item, 0x6d02));
  {
    auto mounted = recover();
    CHECK(mounted->recovery_report().has_value());
    CHECK_EQ(mounted->recovery_report()->intents_completed, 1u);
    auto extents = mounted->GetDataExtents(dst.value());
    CHECK(extents.ok());
    CHECK(extents.value().empty());
    const uint32_t agno = static_cast<uint32_t>(pool.fsb >> geo.agblklog);
    const uint32_t agbno = static_cast<uint32_t>(
        pool.fsb & ((1ULL << geo.agblklog) - 1));
    auto rmap = LoadRmapTree(*image.device, geo, agno);
    CHECK(rmap.ok());
    CHECK(!rmap.value()
               .LookupEq(RmapKey{agbno, dst.value(), 0, 0})
               .ok());
    CHECK(rmap.value()
              .LookupEq(RmapKey{agbno, src.value(), 0, 0})
              .ok());
    mounted.reset();
  }
  CHECK(image.device->Flush());
}

TEST(rw6c_cui_completion_large_intent) {
  for (bool rmapbt : {false, true}) {
    Image image = MakeImage(true, rmapbt, 4096, true);
    // Reserve a private block pool so refcount tree growth cannot land inside
    // the ranges named by the synthetic intent.
    Allocation pool;
    CHECK(image.fs
              ->RunTransaction([&](XfsTransaction& txn) -> Status {
                BlockAllocator alloc(&txn, &image.fs->device(),
                                     &image.fs->geometry());
                Status s = rmapbt
                               ? alloc.AllocateFromAgOwned(
                                     0, 2048, 1,
                                     RmapOwner::Data(
                                         image.fs->superblock().rootino, 0,
                                         false),
                                     &pool)
                               : alloc.AllocateFromAg(0, 2048, 1, &pool);
                if (!s.ok()) return s;
                return alloc.Commit();
              })
              .ok());
    CHECK(image.fs->WriteCleanUnmountRecord().ok());
    image.fs.reset();

    auto sb = ReadSuperblock(*image.device, 0, true);
    CHECK(sb.ok());
    const XfsGeometry geo = MakeGeometry(sb.value());
    auto position = ReadLogPosition(*image.device, geo, sb.value());
    CHECK(position.ok());
    constexpr uint32_t kExtents = 1024;
    std::vector<uint8_t> cui(lf::kIntentLogFormatSize + kExtents * 16, 0);
    StoreLE16(cui.data(), lf::kItemCui);
    StoreLE16(cui.data() + 2, 1);
    StoreLE32(cui.data() + lf::kIntentNextentsOffset, kExtents);
    StoreLE64(cui.data() + lf::kIntentIdOffset, 0x6c0001);
    for (uint32_t i = 0; i < kExtents; ++i) {
      const uint64_t fsb = pool.fsb + i * 2;
      StoreLE64(cui.data() + lf::kIntentLogFormatSize + i * 16, fsb);
      StoreLE32(cui.data() + lf::kIntentLogFormatSize + i * 16 + 8, 1);
      StoreLE32(cui.data() + lf::kIntentLogFormatSize + i * 16 + 12, 1);
    }
    LogRecordParams params;
    params.cycle = position.value().cycle;
    params.block = position.value().next_block;
    params.tid = 0x6c0001;
    params.prev_block = position.value().prev_block;
    params.tail_lsn = MakeLsn(position.value().cycle,
                              position.value().next_block);
    params.log_blocks = position.value().log_blocks;
    params.record_size = position.value().record_size;
    params.uuid = geo.meta_uuid;
    auto record = BuildLogRecord(params, CommitOps(0x6c0001, {cui}));
    if (!record.ok()) {
      ::xfstest::Fail(__FILE__, __LINE__, record.error().Format());
    }
    CHECK(image.device->Write(
        geo.FsbToByte(sb.value().logstart) +
            static_cast<uint64_t>(params.block) * 512,
        record.value().data(), record.value().size()));
    CHECK(image.device->Flush());

    MountOptions recovery;
    recovery.read_write = true;
    recovery.recovery_only = true;
    recovery.clean_log_after_recovery = false;
    auto mounted = XfsFilesystem::Mount(
        std::make_unique<xfstest::BorrowedBlockDevice>(image.device.get()),
        recovery);
    if (!mounted.ok()) {
      ::xfstest::Fail(__FILE__, __LINE__, mounted.error().Format());
    }
    CHECK(mounted.value()->recovery_report().has_value());
    CHECK_EQ(mounted.value()->recovery_report()->intents_completed, 1u);
    auto tree = LoadRefcountTree(*image.device, geo, 0);
    if (!tree.ok()) ::xfstest::Fail(__FILE__, __LINE__, tree.error().Format());
    CHECK_EQ(tree.value().records.size(), kExtents);
    mounted.value().reset();

    auto after = ReadSuperblock(*image.device, 0, true);
    CHECK(after.ok());
    auto scan = ScanLog(*image.device, after.value(),
                        MakeGeometry(after.value()));
    CHECK(scan.ok());
    uint32_t cuds = 0;
    for (const RecoveredTransaction& trans : scan.value().transactions) {
      for (const RecoveredItem& item : trans.items) {
        if (item.type == lf::kItemCud) ++cuds;
      }
    }
    CHECK_EQ(cuds, 1u);
  }
}

TEST(rw6c_cui_cud_parser_matrix) {
  Image image = MakeImage(true, false);
  const uint64_t fsb = image.geo.AgFsb(0, 100);
  for (uint32_t op = 1; op <= 4; ++op) {
    auto parsed = ParseRefcountIntent(MakeCui(17, fsb, 4, op), image.geo);
    CHECK(parsed.ok());
    CHECK_EQ(parsed.value().id, 17u);
    CHECK_EQ(parsed.value().extents.size(), 1u);
    CHECK_EQ(static_cast<uint32_t>(parsed.value().extents[0].type()), op);
    CHECK_EQ(parsed.value().extents[0].startblock, fsb);
    CHECK_EQ(parsed.value().extents[0].len, 4u);
  }
  CHECK(!ParseRefcountIntent(MakeCui(1, fsb, 4, 5), image.geo).ok());
  CHECK(!ParseRefcountIntent(MakeCui(1, fsb, 4, 1 | (1u << 8)), image.geo)
             .ok());
  CHECK(!ParseRefcountIntent(MakeCui(1, fsb, 0, 1), image.geo).ok());
  CHECK(!ParseRefcountIntent(MakeCui(1, fsb, 4, 1, 0), image.geo).ok());
  CHECK(!ParseRefcountIntent(MakeCui(1, fsb, 4, 1, 2), image.geo).ok());
  const uint64_t cross_ag =
      image.geo.AgFsb(1, image.geo.AgBlocksFor(1) - 1);
  CHECK(!ParseRefcountIntent(MakeCui(1, cross_ag, 2, 1), image.geo).ok());
  auto short_region = MakeCui(1, fsb, 4, 1);
  short_region.resize(12);
  CHECK(!ParseRefcountIntent(short_region, image.geo).ok());
  auto wrong_type = MakeCui(1, fsb, 4, 1);
  StoreLE16(wrong_type.data(), lf::kItemCud);
  CHECK(!ParseRefcountIntent(wrong_type, image.geo).ok());
  auto wrong_count = MakeCui(1, fsb, 4, 1);
  StoreLE32(wrong_count.data() + lf::kIntentNextentsOffset, 2);
  CHECK(!ParseRefcountIntent(wrong_count, image.geo).ok());

  auto cud = ParseRefcountIntentDone(MakeCud(99));
  CHECK(cud.ok());
  CHECK_EQ(cud.value().id, 99u);
  auto cud_bad = MakeCud(99);
  cud_bad[0] = 0;
  CHECK(!ParseRefcountIntentDone(cud_bad).ok());
  auto cud_short = MakeCud(99);
  cud_short.resize(12);
  CHECK(!ParseRefcountIntentDone(cud_short).ok());
  CHECK(ClassifyLogItem(lf::kItemCui) == RecoveryItemClass::kIntent);
  CHECK(ClassifyLogItem(lf::kItemCud) == RecoveryItemClass::kIntentDone);
}

TEST(rw6c_cui_cud_lifetime_and_reuse) {
  Image image = MakeImage(true, false);
  LogScan scan;
  const uint64_t fsb = image.geo.AgFsb(0, 100);
  auto add = [&](uint16_t type, uint64_t id, uint32_t tid) {
    RecoveredTransaction trans;
    trans.tid = tid;
    trans.lsn = MakeLsn(1, tid);
    RecoveredItem item;
    item.type = type;
    item.tid = tid;
    if (type == lf::kItemCui) {
      item.regions.push_back(MakeCui(id, fsb, 2, 1));
    } else {
      item.regions.push_back(MakeCud(id));
    }
    trans.items.push_back(std::move(item));
    scan.transactions.push_back(std::move(trans));
  };
  add(lf::kItemCud, 1, 0);  // older than the tail: must not cancel a new CUI
  add(lf::kItemCui, 1, 1);
  add(lf::kItemCui, 2, 2);
  add(lf::kItemCud, 1, 3);
  add(lf::kItemCui, 1, 4);  // id reuse is a fresh intent
  add(lf::kItemCud, 2, 5);
  auto pending = CollectPendingIntents(scan);
  CHECK_EQ(pending.size(), 1u);
  if (!pending.empty()) {
    CHECK_EQ(pending[0].type, lf::kItemCui);
    CHECK_EQ(LoadLE64(pending[0].regions[0].data() + lf::kIntentIdOffset), 1u);
  }
}

// ---------------------------------------------------------------------------
// rmap overlap rules used by shared data
// ---------------------------------------------------------------------------

TEST(rw6c_rmap_allows_only_shared_written_overlap) {
  Image image = MakeImage(true, true, 4096, true);
  XfsFilesystem& fs = *image.fs;
  auto a = fs.CreateRegularFile(fs.RootInode(), "a", 0644, 0, 0);
  auto b = fs.CreateRegularFile(fs.RootInode(), "b", 0644, 0, 0);
  CHECK(a.ok() && b.ok());
  const std::vector<uint8_t> data = Pattern(2 * 4096);
  CHECK(fs.WriteFileData(a.value(), 0, data.data(), data.size()).ok());
  CHECK(fs.ReflinkRange(a.value(), 0, b.value(), 0, 0).ok());
  auto ext = fs.GetDataExtents(a.value());
  CHECK(ext.ok());
  const uint32_t agno =
      static_cast<uint32_t>(ext.value()[0].startblock >> image.geo.agblklog);
  const uint32_t agbno = static_cast<uint32_t>(
      ext.value()[0].startblock & ((1ULL << image.geo.agblklog) - 1));
  auto rmap = LoadRmapTree(fs.device(), image.geo, agno);
  CHECK(rmap.ok());
  CHECK(rmap.value()
            .LookupEq(RmapKey{agbno, a.value(), 0, 0})
            .ok());
  CHECK(rmap.value()
            .LookupEq(RmapKey{agbno, b.value(), 0, 0})
            .ok());

  // The reader must accept the overlapping pair; a metadata owner physically
  // inside a shared data record remains corruption.
  size_t overlapping = 0;
  for (const RmapRecord& rec : rmap.value().records) {
    if (rec.key.startblock < agbno + 2 && agbno < rec.EndBlock()) {
      ++overlapping;
    }
  }
  CHECK(overlapping >= 2);
  CHECK(fs.WriteCleanUnmountRecord().ok());
}
