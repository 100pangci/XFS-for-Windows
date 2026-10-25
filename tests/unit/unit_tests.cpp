// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <algorithm>
#include <cstring>

#include "test_util.h"
#include "xfs/crc32c.h"
#include "xfs/directory.h"
#include "xfs/endian.h"
#include "xfs/extent.h"
#include "xfs/inode.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"
#include "xfs/symlink.h"
#include "xfs/win_name_map.h"

using namespace xfs;

TEST(crc32c_known_vector) {
  const char* data = "123456789";
  CHECK_EQ(Crc32c(data, 9), 0xE3069283u);
  // Raw accumulator with ~0 seed gives the inverted value.
  CHECK_EQ(Crc32cUpdate(0xFFFFFFFFu, data, 9), 0x1CF96D7Cu);
}

TEST(crc32c_xfs_checksum_roundtrip) {
  std::vector<uint8_t> buf(4096, 0xA5);
  UpdateXfsChecksum(buf.data(), buf.size(), 224);
  CHECK(VerifyXfsChecksum(buf.data(), buf.size(), 224));
  buf[100] ^= 0x01;
  CHECK(!VerifyXfsChecksum(buf.data(), buf.size(), 224));
}

TEST(endian_roundtrip) {
  uint8_t buf[8] = {};
  StoreBE16(buf, 0x1234);
  CHECK_EQ(LoadBE16(buf), 0x1234);
  StoreBE32(buf, 0xDEADBEEF);
  CHECK_EQ(LoadBE32(buf), 0xDEADBEEFu);
  StoreBE64(buf, 0x0123456789ABCDEFULL);
  CHECK_EQ(LoadBE64(buf), 0x0123456789ABCDEFULL);
  StoreLE32(buf, 0xAABBCCDD);
  CHECK_EQ(LoadLE32(buf), 0xAABBCCDDu);
  CHECK_EQ((int)GetU8(buf, 0), 0xDD);
}

TEST(extent_record_roundtrip) {
  ondisk::BmbtRec rec;
  rec.unwritten = true;
  rec.startoff = (1ULL << 54) - 2;
  rec.startblock = (1ULL << 52) - 3;
  rec.blockcount = (1ULL << 21) - 1;
  uint64_t l0, l1;
  ondisk::EncodeBmbtRec(rec, &l0, &l1);
  ondisk::BmbtRec out = ondisk::DecodeBmbtRec(l0, l1);
  CHECK_EQ(out.unwritten, true);
  CHECK_EQ(out.startoff, rec.startoff);
  CHECK_EQ(out.startblock, rec.startblock);
  CHECK_EQ(out.blockcount, rec.blockcount);
}

TEST(eofs_hash_basics) {
  CHECK_EQ(XfsDirNameHash(std::string_view("")), 0u);
  CHECK_EQ(XfsDirNameHash(std::string_view("a")), 0x61u);
  // Known value from the XFS hash algorithm: "abcd" folds in one 4-byte group.
  uint32_t expected = (uint32_t('a') << 21) ^ (uint32_t('b') << 14) ^
                      (uint32_t('c') << 7) ^ uint32_t('d');
  CHECK_EQ(XfsDirNameHash(std::string_view("abcd")), expected);
}

TEST(win_name_roundtrip) {
  const char* names[] = {
      "hello.txt",
      "a:b",
      "q?star*star",
      "back\\slash",
      "CON",
      "con.txt",
      "LPT9",
      "trailing.",
      "trailing ",
      "100%.txt",
      "中文文件名.txt",
      "space name",
      "dollar$sign",
  };
  for (const char* name : names) {
    WindowsName win = XfsNameToWindows(name);
    CHECK(!win.too_long);
    auto decoded = WindowsNameToXfs(win.name);
    CHECK(decoded.has_value());
    CHECK_STR_EQ(*decoded, name);
  }
  // Distinct XFS names must never map to the same Windows name.
  WindowsName a = XfsNameToWindows("100%.txt");
  WindowsName b = XfsNameToWindows("100%25.txt");
  CHECK(a.name != b.name);
  CHECK(!WindowsNameToXfs(u"CON").has_value());
  CHECK(!WindowsNameToXfs(u"foo.").has_value());
  CHECK(!WindowsNameToXfs(u"foo<bar").has_value());
}

// ---------------------------------------------------------------------------
// Synthetic extent tests
// ---------------------------------------------------------------------------

namespace {

XfsGeometry MakeTestGeometry() {
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
  geo.inode_size = 512;
  geo.meta_uuid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  geo.uuid = geo.meta_uuid;
  geo.features.ftype = true;
  return geo;
}

void AddExtent(XfsInode* inode, const ondisk::BmbtRec& rec) {
  uint64_t l0, l1;
  ondisk::EncodeBmbtRec(rec, &l0, &l1);
  size_t off = inode->data_fork.size();
  inode->data_fork.resize(off + 16);
  StoreBE64(inode->data_fork.data() + off, l0);
  StoreBE64(inode->data_fork.data() + off + 8, l1);
  inode->nextents++;
}

}  // namespace

TEST(read_file_across_extents_holes_and_unwritten) {
  const uint64_t kBlock = 4096;
  auto dev = std::make_unique<xfstest::MemoryBlockDevice>(kBlock * 1024, 512);
  // Fill data blocks with recognizable patterns.
  std::memset(dev->bytes().data() + kBlock * 10, 'A', kBlock);
  std::memset(dev->bytes().data() + kBlock * 20, 0xCC, kBlock * 2);

  XfsGeometry geo = MakeTestGeometry();
  XfsInode inode;
  inode.ino = 64;
  inode.mode = 0x81A4;  // regular file
  inode.format = ondisk::inode::kFormatExtents;
  inode.size = kBlock * 4 + 5;
  AddExtent(&inode, {0, 10, 1, false});   // block 0 -> fsb 10
  AddExtent(&inode, {2, 20, 2, true});    // blocks 2-3 unwritten
  AddExtent(&inode, {4, 30, 1, false});   // block 4 -> fsb 30
  std::memset(dev->bytes().data() + kBlock * 30, 'B', kBlock);

  std::vector<uint8_t> out(inode.size);
  auto read = ReadFileData(*dev, geo, inode, 0, out.data(), out.size());
  CHECK(read.ok());
  CHECK_EQ(read.value(), out.size());
  for (size_t i = 0; i < kBlock; ++i) CHECK_EQ(out[i], 'A');
  for (size_t i = kBlock; i < kBlock * 2; ++i) CHECK_EQ(out[i], 0);  // hole
  for (size_t i = kBlock * 2; i < kBlock * 4; ++i) CHECK_EQ(out[i], 0);  // unwritten
  for (size_t i = kBlock * 4; i < kBlock * 4 + 5; ++i) CHECK_EQ(out[i], 'B');

  // Read past EOF returns fewer bytes.
  uint8_t tail[16];
  auto tail_read = ReadFileData(*dev, geo, inode, inode.size - 3, tail, 16);
  CHECK(tail_read.ok());
  CHECK_EQ(tail_read.value(), 3u);

  // Hole mapping semantics.
  auto mapped = MapFileOffset(*dev, geo, inode, kBlock + 7);
  CHECK(mapped.ok());
  CHECK(mapped.value().is_hole);
}

TEST(extent_addresses_in_non_power_of_two_allocation_groups) {
  XfsGeometry geo = MakeTestGeometry();
  geo.agcount = 4;
  geo.agblocks = 768;
  geo.agblklog = 10;
  geo.dblocks = 3 * geo.agblocks + 600;  // shorter final AG
  xfstest::MemoryBlockDevice dev(geo.dblocks * geo.block_size);
  const uint64_t fsb = geo.AgFsb(3, 599);
  CHECK(fsb > geo.dblocks);
  std::memset(dev.bytes().data() + geo.FsbToByte(fsb), 'Q', geo.block_size);

  XfsInode inode;
  inode.ino = 64;
  inode.mode = 0x81a4;
  inode.format = ondisk::inode::kFormatExtents;
  inode.size = geo.block_size;
  AddExtent(&inode, {0, fsb, 1, false});
  uint8_t data[8];
  auto read = ReadFileData(dev, geo, inode, 0, data, sizeof(data));
  CHECK(read.ok());
  CHECK_EQ(read.value(), sizeof(data));
  for (uint8_t byte : data) CHECK_EQ(byte, 'Q');

  auto rejects = [&](uint64_t start, uint32_t count) {
    XfsInode invalid = inode;
    invalid.data_fork.clear();
    invalid.nextents = 0;
    AddExtent(&invalid, {0, start, count, false});
    std::vector<XfsExtent> extents;
    auto status = LoadDataForkExtents(dev, geo, invalid, &extents);
    CHECK(!status.ok());
    CHECK(status.error().code == XfsError::kCorruptExtent);
  };
  rejects(fsb, 2);                   // extends beyond the last AG
  rejects(geo.AgFsb(3, 600), 1);      // first block past that AG
  rejects(geo.AgFsb(1, 800), 1);      // encoded padding, not an AG block
  rejects(geo.AgFsb(1, 767), 2);      // crosses an AG boundary
  rejects(geo.AgFsb(4, 0), 1);        // nonexistent AG
  rejects((uint64_t{1} << 42) | fsb, 1);  // AG number must not truncate to u32
}

TEST(read_file_via_bmbt_btree) {
  const uint64_t kBlock = 4096;
  auto dev = std::make_unique<xfstest::MemoryBlockDevice>(kBlock * 1024, 512);
  XfsGeometry geo = MakeTestGeometry();

  // Build a bmbt leaf block at fsb 100 with two extents.
  uint8_t* block = dev->bytes().data() + kBlock * 100;
  const uint64_t daddr = (100 * kBlock) / 512;
  PutBE32(block, ondisk::btree::kMagic, ondisk::kBmapCrcMagic);
  PutBE16(block, ondisk::btree::kLevel, 0);
  PutBE16(block, ondisk::btree::kNumrecs, 2);
  PutBE64(block, ondisk::btree::kLLeftsib, ~0ull);
  PutBE64(block, ondisk::btree::kLRightSib, ~0ull);
  PutBE64(block, ondisk::btree::kLBlkno, daddr);
  PutBE64(block, ondisk::btree::kLLsn, 0);
  std::memcpy(block + ondisk::btree::kLUuid, geo.meta_uuid.data(), 16);
  PutBE64(block, ondisk::btree::kLOwner, 64);
  uint8_t* recs = block + ondisk::btree::kLHeaderLenV5;
  uint64_t l0, l1;
  ondisk::EncodeBmbtRec({0, 200, 1, false}, &l0, &l1);
  StoreBE64(recs, l0);
  StoreBE64(recs + 8, l1);
  ondisk::EncodeBmbtRec({3, 300, 1, false}, &l0, &l1);
  StoreBE64(recs + 16, l0);
  StoreBE64(recs + 24, l1);
  UpdateXfsChecksum(block, kBlock, ondisk::btree::kLCrc);

  std::memset(dev->bytes().data() + kBlock * 200, 'X', kBlock);
  std::memset(dev->bytes().data() + kBlock * 300, 'Y', kBlock);

  XfsInode inode;
  inode.ino = 64;
  inode.mode = 0x81A4;
  inode.format = ondisk::inode::kFormatBtree;
  inode.size = kBlock * 4;
  inode.nextents = 2;
  inode.data_fork.assign(4 + 16, 0);
  PutBE16(inode.data_fork.data(), 0, 1);  // bmdr level
  PutBE16(inode.data_fork.data(), 2, 1);  // one child
  PutBE64(inode.data_fork.data(), 4, 0);  // key startoff 0
  PutBE64(inode.data_fork.data(), 4 + 8, 100);  // ptr fsb 100

  std::vector<uint8_t> out(inode.size);
  auto read = ReadFileData(*dev, geo, inode, 0, out.data(), out.size());
  CHECK(read.ok());
  for (size_t i = 0; i < kBlock; ++i) CHECK_EQ(out[i], 'X');
  for (size_t i = kBlock; i < kBlock * 3; ++i) CHECK_EQ(out[i], 0);
  for (size_t i = kBlock * 3; i < kBlock * 4; ++i) CHECK_EQ(out[i], 'Y');

  std::vector<XfsExtent> extents;
  CHECK(LoadDataForkExtents(*dev, geo, inode, &extents).ok());
  CHECK_EQ(extents.size(), 2u);
  CHECK_EQ(extents[1].startblock, 300u);
}

TEST(bmbt_embedded_root_walk_shares_siblings_and_cycle_state) {
  namespace bt = ondisk::btree;
  const XfsGeometry geo = MakeTestGeometry();
  xfstest::MemoryBlockDevice dev(geo.block_size * 1024ull);
  auto block = [&](uint64_t fsb) { return dev.bytes().data() + fsb * geo.block_size; };
  auto finish = [&](uint64_t fsb) {
    UpdateXfsChecksum(block(fsb), geo.block_size, bt::kLCrc);
  };
  const size_t ptrbase = bt::kLHeaderLenV5 +
                        ((geo.block_size - bt::kLHeaderLenV5) / 16) * 8;
  for (uint64_t fsb : {100ull, 101ull, 102ull, 103ull}) {
    uint8_t* b = block(fsb);
    const bool node = fsb < 102;
    const bool right = fsb == 101 || fsb == 103;
    PutBE32(b, bt::kMagic, ondisk::kBmapCrcMagic);
    PutBE16(b, bt::kLevel, node ? 1 : 0);
    PutBE16(b, bt::kNumrecs, 1);
    PutBE64(b, bt::kLLeftsib, right ? fsb - 1 : ~0ull);
    PutBE64(b, bt::kLRightSib, right ? ~0ull : fsb + 1);
    PutBE64(b, bt::kLBlkno, fsb * geo.block_size / 512);
    std::memcpy(b + bt::kLUuid, geo.meta_uuid.data(), 16);
    PutBE64(b, bt::kLOwner, 64);
    const uint64_t key = right ? 3 : 0;
    if (node) {
      PutBE64(b, bt::kLHeaderLenV5, key);
      PutBE64(b, ptrbase, fsb + 2);
    } else {
      uint64_t l0 = 0, l1 = 0;
      ondisk::EncodeBmbtRec({key, right ? 300ull : 200ull, 1, false}, &l0, &l1);
      PutBE64(b, bt::kLHeaderLenV5, l0);
      PutBE64(b, bt::kLHeaderLenV5 + 8, l1);
    }
    finish(fsb);
  }
  XfsInode inode;
  inode.ino = 64;
  inode.mode = kSIfreg | 0644;
  inode.format = ondisk::inode::kFormatBtree;
  inode.nextents = 2;
  inode.size = 4ull * geo.block_size;
  inode.data_fork.assign(4 + 2 * 16, 0);
  PutBE16(inode.data_fork.data(), 0, 2);
  PutBE16(inode.data_fork.data(), 2, 2);
  PutBE64(inode.data_fork.data(), 4, 0);
  PutBE64(inode.data_fork.data(), 12, 3);
  PutBE64(inode.data_fork.data(), 20, 100);
  PutBE64(inode.data_fork.data(), 28, 101);
  std::vector<XfsExtent> extents;
  CHECK(LoadDataForkExtents(dev, geo, inode, &extents).ok());
  CHECK_EQ(extents.size(), 2u);
  CHECK_EQ(extents.back().startblock, 300u);
  auto reject = [&](uint64_t fsb, size_t offset, uint64_t value) {
    const uint64_t old = GetBE64(block(fsb), offset);
    PutBE64(block(fsb), offset, value);
    finish(fsb);
    CHECK(!LoadDataForkExtents(dev, geo, inode, &extents).ok());
    PutBE64(block(fsb), offset, old);
    finish(fsb);
  };
  reject(100, bt::kLRightSib, ~0ull);  // forward link across embedded-root children
  reject(101, bt::kLLeftsib, ~0ull);   // backward link across root children
  reject(103, bt::kLLeftsib, ~0ull);   // leaf links also span child subtrees
  reject(103, bt::kLRightSib, 102);    // right edge must terminate
  PutBE64(inode.data_fork.data(), 28, 100); // repeated root child is a cycle
  CHECK(!LoadDataForkExtents(dev, geo, inode, &extents).ok());
}

// ---------------------------------------------------------------------------
// Synthetic shortform directory
// ---------------------------------------------------------------------------

TEST(shortform_directory_list_and_lookup) {
  XfsGeometry geo = MakeTestGeometry();
  auto dev = std::make_unique<xfstest::MemoryBlockDevice>(4096 * 64, 512);

  // Build the shortform content manually.
  std::vector<uint8_t> fork;
  fork.resize(6, 0);
  fork[0] = 2;      // count (no stored "." / "..")
  fork[1] = 0;      // i8count
  PutBE32(fork.data(), 2, 64);  // parent
  auto add = [&](const char* name, size_t len, uint32_t ino) {
    size_t off = fork.size();
    fork.resize(off + 3 + len + 1 + 4, 0);
    fork[off] = static_cast<uint8_t>(len);
    PutBE16(fork.data(), off + 1, 0);
    std::memcpy(fork.data() + off + 3, name, len);
    fork[off + 3 + len] = ondisk::dir::kFtRegFile;
    PutBE32(fork.data(), off + 3 + len + 1, ino);
  };
  add("hello.txt", 9, 65);
  {
    const char* unicode_name = reinterpret_cast<const char*>(u8"中文.txt");
    add(unicode_name, std::strlen(unicode_name), 66);
  }

  XfsInode inode;
  inode.ino = 64;
  inode.mode = 0x41ED;
  inode.format = ondisk::inode::kFormatLocal;
  inode.size = fork.size();
  inode.nlink = 2;
  inode.data_fork = fork;

  std::vector<std::string> names;
  Status s = ListDirectory(*dev, geo, inode, [&](const XfsDirEntry& e) {
    names.push_back(e.name);
    return OkStatus();
  });
  CHECK(s.ok());
  CHECK_EQ(names.size(), 4u);  // synthetic "." and ".." + 2 entries

  auto self = LookupName(*dev, geo, inode, ".");
  CHECK(self.ok());
  CHECK_EQ(self.value().ino, 64u);
  auto parent = LookupName(*dev, geo, inode, "..");
  CHECK(parent.ok());
  CHECK_EQ(parent.value().ino, 64u);

  auto found = LookupName(*dev, geo, inode, "hello.txt");
  CHECK(found.ok());
  CHECK_EQ(found.value().ino, 65u);
  auto found_cn = LookupName(*dev, geo, inode, "中文.txt");
  CHECK(found_cn.ok());
  CHECK_EQ(found_cn.value().ino, 66u);
  auto missing = LookupName(*dev, geo, inode, "nope");
  CHECK(!missing.ok());
  CHECK(missing.error().code == XfsError::kNotFound);
}

// ---------------------------------------------------------------------------
// Synthetic leaf-format directory (data block + LEAF1 index block)
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kBlockSize = 4096;
constexpr uint64_t kLeafOffsetBlocks = (1ULL << 35) / kBlockSize;  // 2^23

void WriteDir3DataBlock(uint8_t* block, uint64_t fsb, const XfsGeometry& geo,
                        uint64_t owner,
                        const std::vector<std::pair<std::string, uint64_t>>& entries) {
  std::memset(block, 0, kBlockSize);
  PutBE32(block, 0, ondisk::kDir3DataMagic);
  PutBE64(block, 8, (fsb * kBlockSize) / 512);
  PutBE64(block, 16, 0);
  std::memcpy(block + 24, geo.meta_uuid.data(), 16);
  PutBE64(block, 40, owner);
  uint32_t off = 64;
  uint16_t best_free = 0;
  for (const auto& [name, ino] : entries) {
    uint64_t inumber = ino;
    uint8_t namelen = static_cast<uint8_t>(name.size());
    size_t entsize = ondisk::dir::DataEntrySize(namelen, true);
    PutBE64(block, off, inumber);
    block[off + 8] = namelen;
    std::memcpy(block + off + 9, name.data(), namelen);
    block[off + 9 + namelen] = ondisk::dir::kFtRegFile;
    PutBE16(block, off + entsize - 2, static_cast<uint16_t>(off));
    off += static_cast<uint32_t>(entsize);
  }
  // One unused region covering the rest of the block.
  best_free = static_cast<uint16_t>(kBlockSize - off);
  PutBE16(block, off, 0xFFFF);
  PutBE16(block, off + 2, static_cast<uint16_t>(kBlockSize - off));
  PutBE16(block, kBlockSize - 2, static_cast<uint16_t>(off));
  PutBE16(block, 48, best_free);  // bestfree[0].offset is 0, we use len only
  // (bestfree[0] = {offset 0, length best_free}; our reader does not rely on
  // it, but keep it coherent: set offset to the free region start.)
  PutBE16(block, 48, static_cast<uint16_t>(off));
  PutBE16(block, 50, best_free);
  UpdateXfsChecksum(block, kBlockSize, 4);
}

uint32_t LeafAddress(uint32_t db, uint32_t off) {
  return (db << (12 - 3)) | (off >> 3);
}

void WriteLeaf1Block(uint8_t* block, uint64_t fsb, const XfsGeometry& geo,
                     uint64_t owner,
                     const std::vector<std::pair<std::string, uint32_t>>& entries,
                     uint32_t bestcount, uint16_t best) {
  std::memset(block, 0, kBlockSize);
  PutBE16(block, 8, ondisk::kDir3Leaf1Magic);
  PutBE32(block, 0, 0);   // forw
  PutBE32(block, 4, 0);   // back
  PutBE64(block, 16, (fsb * kBlockSize) / 512);
  std::memcpy(block + 32, geo.meta_uuid.data(), 16);
  PutBE64(block, 48, owner);
  PutBE16(block, 56, static_cast<uint16_t>(entries.size()));
  PutBE16(block, 58, 0);
  uint32_t off = 64;
  for (const auto& [name, address] : entries) {
    PutBE32(block, off, XfsDirNameHash(name));
    PutBE32(block, off + 4, address);
    off += 8;
  }
  PutBE32(block, kBlockSize - 4, bestcount);
  PutBE16(block, kBlockSize - 4 - 2 * bestcount, best);
  UpdateXfsChecksum(block, kBlockSize, 12);
}

void WriteLeafNBlock(
    uint8_t* block, uint64_t fsb, const XfsGeometry& geo, uint64_t owner,
    const std::vector<std::pair<std::string, uint32_t>>& entries) {
  std::memset(block, 0, kBlockSize);
  PutBE16(block, 8, ondisk::kDir3LeafnMagic);
  PutBE32(block, 0, 0);  // forw = NULL
  PutBE32(block, 4, 0);  // back = NULL
  PutBE64(block, 16, (fsb * kBlockSize) / 512);
  std::memcpy(block + 32, geo.meta_uuid.data(), 16);
  PutBE64(block, 48, owner);
  PutBE16(block, 56, static_cast<uint16_t>(entries.size()));
  PutBE16(block, 58, 0);
  uint32_t off = 64;
  for (const auto& [name, address] : entries) {
    PutBE32(block, off, XfsDirNameHash(name));
    PutBE32(block, off + 4, address);
    off += 8;
  }
  UpdateXfsChecksum(block, kBlockSize, 12);
}

void WriteNode3Block(uint8_t* block, uint64_t fsb, const XfsGeometry& geo,
                     uint64_t owner, uint16_t level,
                     const std::vector<std::pair<uint32_t, uint32_t>>& entries) {
  std::memset(block, 0, kBlockSize);
  PutBE16(block, 8, ondisk::kDaNode3Magic);
  PutBE32(block, 0, 0);  // forw = NULL
  PutBE32(block, 4, 0);  // back = NULL
  PutBE64(block, 16, (fsb * kBlockSize) / 512);
  std::memcpy(block + 32, geo.meta_uuid.data(), 16);
  PutBE64(block, 48, owner);
  PutBE16(block, 56, static_cast<uint16_t>(entries.size()));
  PutBE16(block, 58, level);
  uint32_t off = 64;
  for (const auto& [hash, before] : entries) {
    PutBE32(block, off, hash);
    PutBE32(block, off + 4, before);
    off += 8;
  }
  UpdateXfsChecksum(block, kBlockSize, 12);
}

// Single-block directory: XDB3 header + data entries + embedded leaf entries
// + block tail (count/stale).
void WriteBlockDirBlock(
    uint8_t* block, uint64_t fsb, const XfsGeometry& geo, uint64_t owner,
    const std::vector<std::pair<std::string, uint64_t>>& entries) {
  std::memset(block, 0, kBlockSize);
  PutBE32(block, 0, ondisk::kDir3BlockMagic);
  PutBE64(block, 8, (fsb * kBlockSize) / 512);
  std::memcpy(block + 24, geo.meta_uuid.data(), 16);
  PutBE64(block, 40, owner);
  // Data entries.
  uint32_t off = 64;
  std::vector<std::pair<std::string, uint32_t>> leaf_entries;
  for (const auto& [name, ino] : entries) {
    uint8_t namelen = static_cast<uint8_t>(name.size());
    size_t entsize = ondisk::dir::DataEntrySize(namelen, true);
    PutBE64(block, off, ino);
    block[off + 8] = namelen;
    std::memcpy(block + off + 9, name.data(), namelen);
    block[off + 9 + namelen] = ondisk::dir::kFtRegFile;
    PutBE16(block, off + entsize - 2, static_cast<uint16_t>(off));
    leaf_entries.push_back({name, LeafAddress(0, off)});
    off += static_cast<uint32_t>(entsize);
  }
  // The data area ends where the embedded leaf entries begin.
  uint32_t leaf_start =
      static_cast<uint32_t>(kBlockSize - 8 - leaf_entries.size() * 8);
  uint32_t data_end = off;
  uint32_t unused_len = leaf_start - data_end;
  if (unused_len >= 8) {
    PutBE16(block, data_end, 0xFFFF);
    PutBE16(block, data_end + 2, static_cast<uint16_t>(unused_len));
    PutBE16(block, data_end + unused_len - 2,
            static_cast<uint16_t>(data_end));
    PutBE16(block, 48, static_cast<uint16_t>(data_end));
    PutBE16(block, 50, static_cast<uint16_t>(unused_len));
  }
  // Leaf entries (sorted by hash) immediately before the block tail.
  std::sort(leaf_entries.begin(), leaf_entries.end(),
            [](const auto& a, const auto& b) {
              return XfsDirNameHash(a.first) < XfsDirNameHash(b.first);
            });
  uint32_t pos = leaf_start;
  for (const auto& [name, address] : leaf_entries) {
    PutBE32(block, pos, XfsDirNameHash(name));
    PutBE32(block, pos + 4, address);
    pos += 8;
  }
  PutBE32(block, kBlockSize - 8, static_cast<uint32_t>(leaf_entries.size()));
  PutBE32(block, kBlockSize - 4, 0);
  UpdateXfsChecksum(block, kBlockSize, 4);
}

}  // namespace

TEST(leaf_directory_list_and_lookup) {
  XfsGeometry geo = MakeTestGeometry();
  auto dev = std::make_unique<xfstest::MemoryBlockDevice>(kBlockSize * 1024, 512);

  std::vector<std::pair<std::string, uint64_t>> data_entries = {
      {"hello.txt", 100}, {"world", 101}};
  uint8_t* data_block = dev->bytes().data() + kBlockSize * 10;
  WriteDir3DataBlock(data_block, 10, geo, 64, data_entries);

  std::vector<std::pair<std::string, uint32_t>> leaf_entries = {
      {"hello.txt", LeafAddress(0, 64)},
      {"world", LeafAddress(0, 64 + ondisk::dir::DataEntrySize(9, true))}};
  // Leaf entries are sorted by name hash (format invariant).
  std::sort(leaf_entries.begin(), leaf_entries.end(),
            [](const auto& a, const auto& b) {
              return XfsDirNameHash(a.first) < XfsDirNameHash(b.first);
            });
  uint8_t* leaf = dev->bytes().data() + kBlockSize * 11;
  WriteLeaf1Block(leaf, 11, geo, 64, leaf_entries, 1, 3984);

  XfsInode inode;
  inode.ino = 64;
  inode.mode = 0x41ED;
  inode.format = ondisk::inode::kFormatExtents;
  inode.size = kBlockSize * kLeafOffsetBlocks + kBlockSize;
  inode.nlink = 2;
  inode.data_fork.resize(32);
  uint64_t l0, l1;
  ondisk::EncodeBmbtRec({0, 10, 1, false}, &l0, &l1);
  StoreBE64(inode.data_fork.data(), l0);
  StoreBE64(inode.data_fork.data() + 8, l1);
  ondisk::EncodeBmbtRec({kLeafOffsetBlocks, 11, 1, false}, &l0, &l1);
  StoreBE64(inode.data_fork.data() + 16, l0);
  StoreBE64(inode.data_fork.data() + 24, l1);
  inode.nextents = 2;

  auto format = DetectDirFormat(*dev, geo, inode);
  if (!format.ok()) {
    std::printf("detect leaf error: %s\n", format.error().Format().c_str());
  }
  CHECK(format.ok());
  CHECK(format.value() == XfsDirFormat::kLeaf);

  std::vector<std::string> names;
  Status s = ListDirectory(*dev, geo, inode, [&](const XfsDirEntry& e) {
    names.push_back(e.name);
    return OkStatus();
  });
  CHECK(s.ok());
  CHECK_EQ(names.size(), 2u);

  auto hello = LookupName(*dev, geo, inode, "hello.txt");
  CHECK(hello.ok());
  CHECK_EQ(hello.value().ino, 100u);
  auto world = LookupName(*dev, geo, inode, "world");
  CHECK(world.ok());
  CHECK_EQ(world.value().ino, 101u);
  auto missing = LookupName(*dev, geo, inode, "missing");
  CHECK(!missing.ok());
  CHECK(missing.error().code == XfsError::kNotFound);
}

TEST(block_directory_list_and_lookup) {
  XfsGeometry geo = MakeTestGeometry();
  auto dev = std::make_unique<xfstest::MemoryBlockDevice>(kBlockSize * 1024, 512);

  std::vector<std::pair<std::string, uint64_t>> entries = {
      {"a.txt", 100}, {"b.txt", 101}, {"c.txt", 102}};
  uint8_t* block = dev->bytes().data() + kBlockSize * 10;
  WriteBlockDirBlock(block, 10, geo, 64, entries);

  XfsInode inode;
  inode.ino = 64;
  inode.mode = 0x41ED;
  inode.format = ondisk::inode::kFormatExtents;
  inode.size = kBlockSize;
  inode.nlink = 2;
  inode.data_fork.resize(16);
  uint64_t l0, l1;
  ondisk::EncodeBmbtRec({0, 10, 1, false}, &l0, &l1);
  StoreBE64(inode.data_fork.data(), l0);
  StoreBE64(inode.data_fork.data() + 8, l1);
  inode.nextents = 1;

  auto format = DetectDirFormat(*dev, geo, inode);
  CHECK(format.ok());
  CHECK(format.value() == XfsDirFormat::kBlock);

  std::vector<std::string> names;
  Status s = ListDirectory(*dev, geo, inode, [&](const XfsDirEntry& e) {
    names.push_back(e.name);
    return OkStatus();
  });
  CHECK(s.ok());
  CHECK_EQ(names.size(), 3u);

  auto b = LookupName(*dev, geo, inode, "b.txt");
  CHECK(b.ok());
  CHECK_EQ(b.value().ino, 101u);
  auto missing = LookupName(*dev, geo, inode, "missing");
  CHECK(!missing.ok());
  CHECK(missing.error().code == XfsError::kNotFound);

  // A block tail with an impossible leaf count must be rejected.
  PutBE32(block, kBlockSize - 8, 0xFFFF);
  UpdateXfsChecksum(block, kBlockSize, 4);
  Status bad = ListDirectory(*dev, geo, inode, [&](const XfsDirEntry&) {
    return OkStatus();
  });
  CHECK(!bad.ok());
  CHECK(bad.error().code == XfsError::kCorruptDirectory);
}

TEST(node_directory_list_and_lookup) {  XfsGeometry geo = MakeTestGeometry();
  auto dev = std::make_unique<xfstest::MemoryBlockDevice>(kBlockSize * 1024, 512);

  std::vector<std::pair<std::string, uint64_t>> data_entries = {
      {"hello.txt", 100}, {"world", 101}};
  WriteDir3DataBlock(dev->bytes().data() + kBlockSize * 10, 10, geo, 64,
                     data_entries);

  std::vector<std::pair<std::string, uint32_t>> leaf_entries = {
      {"hello.txt", LeafAddress(0, 64)},
      {"world", LeafAddress(0, 64 + ondisk::dir::DataEntrySize(9, true))}};
  std::sort(leaf_entries.begin(), leaf_entries.end(),
            [](const auto& a, const auto& b) {
              return XfsDirNameHash(a.first) < XfsDirNameHash(b.first);
            });
  uint32_t last_hash = XfsDirNameHash(leaf_entries.back().first);
  uint32_t leaf_dablk = static_cast<uint32_t>(kLeafOffsetBlocks + 1);
  WriteLeafNBlock(dev->bytes().data() + kBlockSize * 12, 12, geo, 64,
                  leaf_entries);

  // Root node at the leaf-space offset, one child (the leafn block).
  WriteNode3Block(dev->bytes().data() + kBlockSize * 11, 11, geo, 64,
                  /*level=*/1, {{last_hash, leaf_dablk}});

  XfsInode inode;
  inode.ino = 64;
  inode.mode = 0x41ED;
  inode.format = ondisk::inode::kFormatExtents;
  inode.size = kBlockSize * kLeafOffsetBlocks + 2 * kBlockSize;
  inode.nlink = 2;
  inode.data_fork.resize(32);
  uint64_t l0, l1;
  ondisk::EncodeBmbtRec({0, 10, 1, false}, &l0, &l1);
  StoreBE64(inode.data_fork.data(), l0);
  StoreBE64(inode.data_fork.data() + 8, l1);
  // Node (fsb 11) and leaf (fsb 12) are contiguous in one extent.
  ondisk::EncodeBmbtRec({kLeafOffsetBlocks, 11, 2, false}, &l0, &l1);
  StoreBE64(inode.data_fork.data() + 16, l0);
  StoreBE64(inode.data_fork.data() + 24, l1);
  inode.nextents = 2;

  auto format = DetectDirFormat(*dev, geo, inode);
  if (!format.ok()) {
    std::printf("detect node error: %s\n", format.error().Format().c_str());
  }
  CHECK(format.ok());
  CHECK(format.value() == XfsDirFormat::kNode);

  std::vector<std::string> names;
  Status s = ListDirectory(*dev, geo, inode, [&](const XfsDirEntry& e) {
    names.push_back(e.name);
    return OkStatus();
  });
  CHECK(s.ok());
  CHECK_EQ(names.size(), 2u);

  auto hello = LookupName(*dev, geo, inode, "hello.txt");
  CHECK(hello.ok());
  CHECK_EQ(hello.value().ino, 100u);
  auto world = LookupName(*dev, geo, inode, "world");
  CHECK(world.ok());
  CHECK_EQ(world.value().ino, 101u);
  auto missing = LookupName(*dev, geo, inode, "missing");
  CHECK(!missing.ok());
  CHECK(missing.error().code == XfsError::kNotFound);
}

// ---------------------------------------------------------------------------
// Symlinks
// ---------------------------------------------------------------------------

TEST(local_symlink_target) {
  XfsGeometry geo = MakeTestGeometry();
  auto dev = std::make_unique<xfstest::MemoryBlockDevice>(kBlockSize * 64, 512);

  XfsInode inode;
  inode.ino = 64;
  inode.mode = 0xA1FF;  // S_IFLNK | 0777
  inode.format = ondisk::inode::kFormatLocal;
  std::string target = "../folder/hello.txt";
  inode.size = target.size();
  inode.data_fork.assign(target.begin(), target.end());

  auto result = ReadSymlinkTarget(*dev, geo, inode);
  CHECK(result.ok());
  CHECK_STR_EQ(result.value(), target);

  // A non-symlink is rejected.
  inode.mode = 0x81FF;
  auto not_link = ReadSymlinkTarget(*dev, geo, inode);
  CHECK(!not_link.ok());
  CHECK(not_link.error().code == XfsError::kNotSymlink);
}

TEST(remote_symlink_target) {
  XfsGeometry geo = MakeTestGeometry();
  auto dev = std::make_unique<xfstest::MemoryBlockDevice>(kBlockSize * 64, 512);

  std::string target = "../folder/hello.txt";
  uint8_t* block = dev->bytes().data() + kBlockSize * 10;
  std::memset(block, 0, kBlockSize);
  PutBE32(block, ondisk::symlink::kMagic, ondisk::kSymlinkMagic);
  // sl_offset is the offset of this chunk inside the target string, exactly
  // what xfs_symlink_hdr_set writes (0 for the first extent).
  PutBE32(block, ondisk::symlink::kOffset, 0);
  PutBE32(block, ondisk::symlink::kBytes, static_cast<uint32_t>(target.size()));
  PutBE64(block, ondisk::symlink::kOwner, 64);
  PutBE64(block, ondisk::symlink::kBlkno, (10 * kBlockSize) / 512);
  std::memcpy(block + ondisk::symlink::kUuid, geo.meta_uuid.data(), 16);
  std::memcpy(block + ondisk::symlink::kHeaderSize, target.data(),
              target.size());
  UpdateXfsChecksum(block, kBlockSize, ondisk::symlink::kCrc);

  XfsInode inode;
  inode.ino = 64;
  inode.mode = 0xA1FF;
  inode.format = ondisk::inode::kFormatExtents;
  inode.size = target.size();
  inode.data_fork.resize(16);
  uint64_t l0, l1;
  ondisk::EncodeBmbtRec({0, 10, 1, false}, &l0, &l1);
  StoreBE64(inode.data_fork.data(), l0);
  StoreBE64(inode.data_fork.data() + 8, l1);
  inode.nextents = 1;

  auto result = ReadSymlinkTarget(*dev, geo, inode);
  if (!result.ok()) {
    std::printf("remote symlink error: %s\n", result.error().Format().c_str());
  }
  CHECK(result.ok());
  CHECK_STR_EQ(result.value(), target);
}
