// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - tests for RW-4: atomic rename (same directory, cross
// directory, replace), hard links and native symlink creation, including link
// counts, ".." migration and the AGI unlinked lifecycle.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/ag.h"
#include "xfs/endian.h"
#include "xfs/extent.h"
#include "xfs/filesystem.h"
#include "xfs/ondisk.h"

using namespace xfs;

namespace {

constexpr uint64_t kRoot = 64;
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

void CheckConsistent(xfstest::FixtureImage& fixture) {
  auto fs = fixture.Mount(/*read_write=*/false);
  if (!fs.ok()) {
    std::printf("  check mount error: %s\n", fs.error().Format().c_str());
  }
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

// The directory format as written on disk: shortform, block, leaf or node.
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
  Status loaded = LoadDataForkExtents(fs.value()->device(), geo, inode.value(),
                                      &extents);
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

uint32_t NlinkIn(XfsFilesystem& fs, uint64_t ino) {
  auto inode = fs.OpenInode(ino);
  CHECK(inode.ok());
  if (!inode.ok()) return 0;
  return inode.value().nlink;
}

uint32_t NlinkOf(xfstest::FixtureImage& fixture, uint64_t ino) {
  auto fs = fixture.Mount(/*read_write=*/false);
  CHECK(fs.ok());
  if (!fs.ok()) return 0;
  return NlinkIn(*fs.value(), ino);
}

}  // namespace

// ---------------------------------------------------------------------------
// Same-directory rename
// ---------------------------------------------------------------------------

TEST(rw4_same_directory_rename_file) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t ino = 0;
  std::vector<uint8_t> data(4096, 0x11);
  const SuperblockCounters before = ReadSuperblockCounters(fixture);
  const uint32_t root_nlink_before = NlinkOf(fixture, kRoot);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto created = fs.value()->CreateRegularFile(kRoot, "a.bin", 0644, 0, 0);
    CHECK(created.ok());
    if (!created.ok()) return;
    ino = created.value();
    CHECK(fs.value()->WriteFileData(ino, 0, data.data(), data.size()).ok());
    auto renamed = fs.value()->Rename(kRoot, "a.bin", kRoot, "b.bin", true);
    if (!renamed.ok()) {
      std::printf("rename error: %s\n", renamed.error().Format().c_str());
    }
    CHECK(renamed.ok());
    if (!renamed.ok()) return;
    CHECK_EQ(renamed.value().moved_ino, ino);
    CHECK_EQ(renamed.value().replaced_ino, 0ull);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto old_name = fs.value()->LookupName(kRoot, "a.bin");
    CHECK(!old_name.ok());
    CHECK_EQ(old_name.error().code, XfsError::kNotFound);
    auto new_name = fs.value()->LookupName(kRoot, "b.bin");
    CHECK(new_name.ok());
    CHECK_EQ(new_name.value().ino, ino);
    std::vector<uint8_t> got(data.size(), 0);
    auto read = fs.value()->ReadFile(ino, 0, got.data(), got.size());
    CHECK(read.ok());
    CHECK(got == data);
    CHECK_EQ(NlinkOf(fixture, ino), 1u);
  }
  const SuperblockCounters after = ReadSuperblockCounters(fixture);
  // The created file still exists (the rename moved it, it did not duplicate
  // or free anything): one inode and one data block more than the fixture.
  CHECK_EQ(after.icount, before.icount);
  CHECK_EQ(after.ifree, before.ifree - 1);
  CHECK_EQ(after.fdblocks, before.fdblocks - 1);
  CHECK_EQ(NlinkOf(fixture, kRoot), root_nlink_before);
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Cross-directory rename
// ---------------------------------------------------------------------------

TEST(rw4_cross_directory_rename_file) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t ino = 0;
  uint64_t a = 0;
  uint64_t b = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(fs.value()->Mkdir(kRoot, "A", 0755, 0, 0).ok());
    CHECK(fs.value()->Mkdir(kRoot, "B", 0755, 0, 0).ok());
    a = fs.value()->LookupName(kRoot, "A").value().ino;
    b = fs.value()->LookupName(kRoot, "B").value().ino;
    auto created = fs.value()->CreateRegularFile(a, "file", 0644, 0, 0);
    CHECK(created.ok());
    if (!created.ok()) return;
    ino = created.value();
    std::vector<uint8_t> data(2048, 0x22);
    CHECK(fs.value()->WriteFileData(ino, 0, data.data(), data.size()).ok());
    auto renamed = fs.value()->Rename(a, "file", b, "file", false);
    if (!renamed.ok()) {
      std::printf("rename error: %s\n", renamed.error().Format().c_str());
    }
    CHECK(renamed.ok());
    if (!renamed.ok()) return;
    CHECK_EQ(renamed.value().moved_ino, ino);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(!fs.value()->LookupName(a, "file").ok());
    auto moved = fs.value()->LookupName(b, "file");
    CHECK(moved.ok());
    CHECK_EQ(moved.value().ino, ino);
    // Parent link counts are unchanged for a file move.
    CHECK_EQ(NlinkOf(fixture, a), 2u);
    CHECK_EQ(NlinkOf(fixture, b), 2u);
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw4_cross_directory_directory_rename_repoints_dotdot) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t parent = 0;
  uint64_t child = 0;
  uint64_t other = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(fs.value()->Mkdir(kRoot, "A", 0755, 0, 0).ok());
    CHECK(fs.value()->Mkdir(kRoot, "B", 0755, 0, 0).ok());
    parent = fs.value()->LookupName(kRoot, "A").value().ino;
    other = fs.value()->LookupName(kRoot, "B").value().ino;
    CHECK(fs.value()->Mkdir(parent, "child", 0755, 0, 0).ok());
    child = fs.value()->LookupName(parent, "child").value().ino;
    CHECK(fs.value()->CreateRegularFile(child, "inside", 0644, 0, 0).ok());
    auto renamed = fs.value()->Rename(parent, "child", other, "child", false);
    if (!renamed.ok()) {
      std::printf("rename error: %s\n", renamed.error().Format().c_str());
    }
    CHECK(renamed.ok());
    if (!renamed.ok()) return;
    CHECK_EQ(renamed.value().moved_ino, child);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(!fs.value()->LookupName(parent, "child").ok());
    auto moved = fs.value()->LookupName(other, "child");
    CHECK(moved.ok());
    CHECK_EQ(moved.value().ino, child);
    // The moved directory keeps its contents and its ".." was rewritten.
    CHECK(fs.value()->LookupName(child, "inside").ok());
    auto dotdot = fs.value()->LookupName(child, "..");
    CHECK(dotdot.ok());
    CHECK_EQ(dotdot.value().ino, other);
    // A is back to 2 links (only "." and its entry in the root); B gained one.
    CHECK_EQ(NlinkOf(fixture, parent), 2u);
    CHECK_EQ(NlinkOf(fixture, other), 3u);
    CHECK_EQ(NlinkOf(fixture, child), 2u);
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Replace
// ---------------------------------------------------------------------------

TEST(rw4_rename_replace_regular_file_unlinks_target) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t src_ino = 0;
  uint64_t dst_ino = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto src = fs.value()->CreateRegularFile(kRoot, "src", 0644, 0, 0);
    auto dst = fs.value()->CreateRegularFile(kRoot, "dst", 0644, 0, 0);
    CHECK(src.ok() && dst.ok());
    if (!src.ok() || !dst.ok()) return;
    src_ino = src.value();
    dst_ino = dst.value();
    std::vector<uint8_t> data(4096, 0x33);
    CHECK(fs.value()->WriteFileData(src_ino, 0, data.data(), data.size()).ok());
    auto renamed = fs.value()->Rename(kRoot, "src", kRoot, "dst", true);
    if (!renamed.ok()) {
      std::printf("rename error: %s\n", renamed.error().Format().c_str());
    }
    CHECK(renamed.ok());
    if (!renamed.ok()) return;
    CHECK_EQ(renamed.value().moved_ino, src_ino);
    CHECK_EQ(renamed.value().replaced_ino, dst_ino);
    CHECK_EQ(renamed.value().replaced_new_nlink, 0u);
    CHECK(renamed.value().replaced_zero_link);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(!fs.value()->LookupName(kRoot, "src").ok());
    auto dst = fs.value()->LookupName(kRoot, "dst");
    CHECK(dst.ok());
    CHECK_EQ(dst.value().ino, src_ino);
    // The replaced inode was freed (defer was false): it no longer opens.
    auto gone = fs.value()->OpenInode(dst_ino);
    CHECK(!gone.ok());
    CHECK_EQ(NlinkOf(fixture, kRoot), 2u);  // root has no subdirectories
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw4_rename_replace_deferred_free_lifecycle) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t dst_ino = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto src = fs.value()->CreateRegularFile(kRoot, "src", 0644, 0, 0);
    auto dst = fs.value()->CreateRegularFile(kRoot, "dst", 0644, 0, 0);
    CHECK(src.ok() && dst.ok());
    if (!src.ok() || !dst.ok()) return;
    dst_ino = dst.value();
    std::vector<uint8_t> data(8192, 0x44);
    CHECK(fs.value()->WriteFileData(dst_ino, 0, data.data(), data.size()).ok());
    auto renamed =
        fs.value()->Rename(kRoot, "src", kRoot, "dst", true,
                           /*defer_replaced_free=*/true);
    CHECK(renamed.ok());
    if (!renamed.ok()) return;
    CHECK(renamed.value().replaced_zero_link);
    // The zero-link inode is still openable (open-but-unlinked) and is the
    // head of its AGI unlinked bucket.
    auto open = fs.value()->OpenInode(dst_ino);
    CHECK(open.ok());
    if (!open.ok()) return;
    CHECK_EQ(open.value().nlink, 0u);
    CHECK(open.value().mode != 0);
    uint32_t head = kNullAgino;
    uint32_t expected_agino = 0;
    {
      const XfsGeometry& geo = fs.value()->geometry();
      auto agi = ReadAgi(fixture.storage(), geo, geo.InoAgno(dst_ino));
      CHECK(agi.ok());
      expected_agino = geo.InoAgino(dst_ino);
      head = agi.value().unlinked[expected_agino % ondisk::kAgiUnlinkedBuckets];
    }
    CHECK_EQ(head, expected_agino);
    // Reads through the unlinked inode still work (the old handle case).
    std::vector<uint8_t> got(8192, 0);
    auto read = fs.value()->ReadFile(dst_ino, 0, got.data(), got.size());
    CHECK(read.ok());
    CHECK(got == data);
    // The last handle closed: free it.
    auto freed = fs.value()->FreeUnlinkedInode(dst_ino);
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

TEST(rw4_rename_replace_empty_directory) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t src_dir = 0;
  uint64_t dst_dir = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(fs.value()->Mkdir(kRoot, "s", 0755, 0, 0).ok());
    CHECK(fs.value()->Mkdir(kRoot, "t", 0755, 0, 0).ok());
    src_dir = fs.value()->LookupName(kRoot, "s").value().ino;
    dst_dir = fs.value()->LookupName(kRoot, "t").value().ino;
    auto renamed = fs.value()->Rename(kRoot, "s", kRoot, "t", true);
    if (!renamed.ok()) {
      std::printf("rename error: %s\n", renamed.error().Format().c_str());
    }
    CHECK(renamed.ok());
    if (!renamed.ok()) return;
    CHECK_EQ(renamed.value().moved_ino, src_dir);
    CHECK_EQ(renamed.value().replaced_ino, dst_dir);
    CHECK(renamed.value().replaced_zero_link);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(!fs.value()->LookupName(kRoot, "s").ok());
    auto t = fs.value()->LookupName(kRoot, "t");
    CHECK(t.ok());
    CHECK_EQ(t.value().ino, src_dir);
    // Same-directory directory replace: two subdirectories became one, so the
    // root lost one ".." reference (4 -> 3 with only the fixture file above).
    CHECK_EQ(NlinkOf(fixture, kRoot), 3u);
    auto gone = fs.value()->OpenInode(dst_dir);
    CHECK(!gone.ok());
    auto dotdot = fs.value()->LookupName(src_dir, "..");
    CHECK(dotdot.ok());
    CHECK_EQ(dotdot.value().ino, kRoot);
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw4_rename_replace_target_with_other_hardlinks_only_drops_nlink) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t dst_ino = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto src = fs.value()->CreateRegularFile(kRoot, "src", 0644, 0, 0);
    auto dst = fs.value()->CreateRegularFile(kRoot, "dst", 0644, 0, 0);
    CHECK(src.ok() && dst.ok());
    if (!src.ok() || !dst.ok()) return;
    dst_ino = dst.value();
    CHECK(fs.value()->Link(dst_ino, kRoot, "dst2").ok());
    auto renamed = fs.value()->Rename(kRoot, "src", kRoot, "dst", true);
    CHECK(renamed.ok());
    if (!renamed.ok()) return;
    CHECK(!renamed.value().replaced_zero_link);
    CHECK_EQ(renamed.value().replaced_new_nlink, 1u);
    auto other = fs.value()->LookupName(kRoot, "dst2");
    CHECK(other.ok());
    CHECK_EQ(other.value().ino, dst_ino);
    // The surviving name still reads the old inode; the inode was not freed.
    auto open = fs.value()->OpenInode(dst_ino);
    CHECK(open.ok());
    if (open.ok()) CHECK_EQ(open.value().nlink, 1u);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Refusals and no-ops
// ---------------------------------------------------------------------------

TEST(rw4_rename_rejects_directory_cycle) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t a = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(fs.value()->Mkdir(kRoot, "A", 0755, 0, 0).ok());
    a = fs.value()->LookupName(kRoot, "A").value().ino;
    CHECK(fs.value()->Mkdir(a, "B", 0755, 0, 0).ok());
    const uint64_t b = fs.value()->LookupName(a, "B").value().ino;
    auto renamed = fs.value()->Rename(kRoot, "A", b, "A", false);
    CHECK(!renamed.ok());
    CHECK_EQ(renamed.error().code, XfsError::kWouldCycle);
    // Nothing moved.
    CHECK(fs.value()->LookupName(kRoot, "A").ok());
    CHECK(fs.value()->LookupName(a, "B").ok());
    CHECK(!fs.value()->LookupName(b, "A").ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw4_rename_rejects_non_empty_target_directory) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(fs.value()->Mkdir(kRoot, "s", 0755, 0, 0).ok());
    CHECK(fs.value()->Mkdir(kRoot, "t", 0755, 0, 0).ok());
    const uint64_t t = fs.value()->LookupName(kRoot, "t").value().ino;
    CHECK(fs.value()->CreateRegularFile(t, "child", 0644, 0, 0).ok());
    auto renamed = fs.value()->Rename(kRoot, "s", kRoot, "t", true);
    CHECK(!renamed.ok());
    CHECK_EQ(renamed.error().code, XfsError::kDirectoryNotEmpty);
    CHECK(fs.value()->LookupName(kRoot, "s").ok());
    CHECK(fs.value()->LookupName(t, "child").ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw4_rename_file_onto_directory_and_back_is_refused) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(fs.value()->CreateRegularFile(kRoot, "file", 0644, 0, 0).ok());
    CHECK(fs.value()->Mkdir(kRoot, "dir", 0755, 0, 0).ok());
    auto file_on_dir = fs.value()->Rename(kRoot, "file", kRoot, "dir", true);
    CHECK(!file_on_dir.ok());
    auto dir_on_file = fs.value()->Rename(kRoot, "dir", kRoot, "file", true);
    CHECK(!dir_on_file.ok());
    CHECK(fs.value()->LookupName(kRoot, "file").ok());
    CHECK(fs.value()->LookupName(kRoot, "dir").ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw4_rename_same_inode_and_same_path_are_noops) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t ino = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto created = fs.value()->CreateRegularFile(kRoot, "a", 0644, 0, 0);
    CHECK(created.ok());
    if (!created.ok()) return;
    ino = created.value();
    CHECK(fs.value()->Link(ino, kRoot, "b").ok());
    // rename(a, a): no-op.
    auto same = fs.value()->Rename(kRoot, "a", kRoot, "a", true);
    CHECK(same.ok());
    if (same.ok()) CHECK_EQ(same.value().moved_ino, ino);
    // rename(a, b) with a and b hard links to the same inode: no-op.
    auto hardlink = fs.value()->Rename(kRoot, "a", kRoot, "b", true);
    if (!hardlink.ok()) {
      std::printf("rename error: %s\n", hardlink.error().Format().c_str());
    }
    CHECK(hardlink.ok());
    CHECK(fs.value()->LookupName(kRoot, "a").ok());
    CHECK(fs.value()->LookupName(kRoot, "b").ok());
    CHECK_EQ(NlinkIn(*fs.value(), ino), 2u);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Hard links
// ---------------------------------------------------------------------------

TEST(rw4_link_and_unlink_lifecycle) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t ino = 0;
  std::vector<uint8_t> data(4096, 0x55);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto created = fs.value()->CreateRegularFile(kRoot, "a", 0644, 0, 0);
    CHECK(created.ok());
    if (!created.ok()) return;
    ino = created.value();
    CHECK(fs.value()->WriteFileData(ino, 0, data.data(), data.size()).ok());
    CHECK(fs.value()->Link(ino, kRoot, "b").ok());
    CHECK(fs.value()->Link(ino, kRoot, "c").ok());
    CHECK_EQ(NlinkIn(*fs.value(), ino), 3u);
    auto b = fs.value()->LookupName(kRoot, "b");
    CHECK(b.ok());
    CHECK_EQ(b.value().ino, ino);
    CHECK_EQ(b.value().ftype, ondisk::dir::kFtRegFile);
    // Unlink one name: the inode stays.
    CHECK(fs.value()->Unlink(kRoot, "a", false).ok());
    auto open = fs.value()->OpenInode(ino);
    CHECK(open.ok());
    if (open.ok()) CHECK_EQ(open.value().nlink, 2u);
    std::vector<uint8_t> got(data.size(), 0);
    auto read = fs.value()->ReadFile(ino, 0, got.data(), got.size());
    CHECK(read.ok());
    CHECK(got == data);
    // Unlink the second: still alive.
    CHECK(fs.value()->Unlink(kRoot, "b", false).ok());
    CHECK_EQ(NlinkIn(*fs.value(), ino), 1u);
    // Unlink the last: gone.
    CHECK(fs.value()->Unlink(kRoot, "c", false).ok());
    auto gone = fs.value()->OpenInode(ino);
    CHECK(!gone.ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw4_link_cross_directory_and_refusals) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t ino = 0;
  uint64_t dir = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto created = fs.value()->CreateRegularFile(kRoot, "file", 0644, 0, 0);
    CHECK(created.ok());
    if (!created.ok()) return;
    ino = created.value();
    CHECK(fs.value()->Mkdir(kRoot, "dir", 0755, 0, 0).ok());
    dir = fs.value()->LookupName(kRoot, "dir").value().ino;
    CHECK(fs.value()->Link(ino, dir, "linked").ok());
    auto linked = fs.value()->LookupName(dir, "linked");
    CHECK(linked.ok());
    CHECK_EQ(linked.value().ino, ino);
    CHECK_EQ(NlinkIn(*fs.value(), ino), 2u);

    // A second link at an existing name is refused.
    auto dup = fs.value()->Link(ino, dir, "linked");
    CHECK(!dup.ok());
    CHECK_EQ(dup.error().code, XfsError::kAlreadyExists);
    // Directories never get hard links.
    auto on_dir = fs.value()->Link(dir, kRoot, "dirlink");
    CHECK(!on_dir.ok());
    CHECK_EQ(on_dir.error().code, XfsError::kPermissionDenied);
    // The directory's link count is untouched by all of it.
    CHECK_EQ(NlinkIn(*fs.value(), dir), 2u);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Symlinks
// ---------------------------------------------------------------------------

TEST(rw4_symlink_inline_and_unlink) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t ino = 0;
  const std::string target = "../hello.txt";
  const SuperblockCounters before = ReadSuperblockCounters(fixture);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto created =
        fs.value()->CreateSymlink(kRoot, "link", target, 0777, 0, 0);
    if (!created.ok()) {
      std::printf("symlink error: %s\n", created.error().Format().c_str());
    }
    CHECK(created.ok());
    if (!created.ok()) return;
    ino = created.value();
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
    CHECK_EQ(inode.value().mode & kSIfmt, kSIflnk);
    CHECK_EQ(inode.value().format, ondisk::inode::kFormatLocal);
    CHECK_EQ(inode.value().size, target.size());
    CHECK_EQ(inode.value().nlink, 1u);
    CHECK_EQ(inode.value().nblocks, 0ull);
    CHECK_EQ(inode.value().nextents, 0u);
    auto entry = fs.value()->LookupName(kRoot, "link");
    CHECK(entry.ok());
    CHECK_EQ(entry.value().ino, ino);
    CHECK_EQ(entry.value().ftype, ondisk::dir::kFtSymlink);
    auto read = fs.value()->ReadSymlink(ino);
    CHECK(read.ok());
    CHECK_STR_EQ(read.value(), target);
    // The target is never resolved: it does not even exist as a path.
    CHECK(!fs.value()->LookupPath("/hello.txt").ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    CHECK(fs.value()->Unlink(kRoot, "link", false).ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(!fs.value()->LookupName(kRoot, "link").ok());
    auto gone = fs.value()->OpenInode(ino);
    CHECK(!gone.ok());
  }
  const SuperblockCounters after = ReadSuperblockCounters(fixture);
  CHECK_EQ(after.ifree, before.ifree);
  CHECK_EQ(after.fdblocks, before.fdblocks);
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw4_symlink_remote_and_unlink_frees_blocks) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  // A target longer than the inode literal area but below the XFS maximum.
  std::string target;
  for (int i = 0; i < 800; ++i) {
    target.push_back(static_cast<char>('a' + (i % 26)));
  }
  uint64_t ino = 0;
  const SuperblockCounters before = ReadSuperblockCounters(fixture);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    const uint64_t local_capacity =
        fs.value()->geometry().inode_size - ondisk::inode::kCoreSizeV3;
    CHECK(target.size() > local_capacity);
    auto created = fs.value()->CreateSymlink(kRoot, "rlink", target, 0777, 0, 0);
    if (!created.ok()) {
      std::printf("symlink error: %s\n", created.error().Format().c_str());
    }
    CHECK(created.ok());
    if (!created.ok()) return;
    ino = created.value();
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
    CHECK_EQ(inode.value().mode & kSIfmt, kSIflnk);
    CHECK_EQ(inode.value().format, ondisk::inode::kFormatExtents);
    CHECK_EQ(inode.value().size, target.size());
    CHECK_EQ(inode.value().nextents, 1u);
    CHECK_EQ(inode.value().nblocks, 1ull);
    auto read = fs.value()->ReadSymlink(ino);
    CHECK(read.ok());
    if (read.ok()) CHECK_STR_EQ(read.value(), target);
  }
  const SuperblockCounters after_create = ReadSuperblockCounters(fixture);
  CHECK_EQ(after_create.fdblocks + 1, before.fdblocks);
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    CHECK(fs.value()->Unlink(kRoot, "rlink", false).ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  const SuperblockCounters after_unlink = ReadSuperblockCounters(fixture);
  CHECK_EQ(after_unlink.fdblocks, before.fdblocks);
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(!fs.value()->LookupName(kRoot, "rlink").ok());
    auto gone = fs.value()->OpenInode(ino);
    CHECK(!gone.ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw4_symlink_target_limits) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    // Empty targets never exist on disk.
    auto empty = fs.value()->CreateSymlink(kRoot, "e", "", 0777, 0, 0);
    CHECK(!empty.ok());
    // 1023 bytes is the maximum (XFS_SYMLINK_MAXLEN - 1).
    std::string ok_target(1023, 'x');
    auto made = fs.value()->CreateSymlink(kRoot, "max", ok_target, 0777, 0, 0);
    if (!made.ok()) {
      std::printf("symlink error: %s\n", made.error().Format().c_str());
    }
    CHECK(made.ok());
    if (made.ok()) {
      auto read = fs.value()->ReadSymlink(made.value());
      CHECK(read.ok());
      if (read.ok()) CHECK_STR_EQ(read.value(), ok_target);
    }
    std::string too_long(1024, 'y');
    auto refused = fs.value()->CreateSymlink(kRoot, "long", too_long, 0777, 0, 0);
    CHECK(!refused.ok());
    // A NUL inside the target cannot be represented on disk.
    std::string with_nul("a\0b", 3);
    auto nul = fs.value()->CreateSymlink(kRoot, "nul", with_nul, 0777, 0, 0);
    CHECK(!nul.ok());
    CHECK(!fs.value()->LookupName(kRoot, "e").ok());
    CHECK(!fs.value()->LookupName(kRoot, "long").ok());
    CHECK(!fs.value()->LookupName(kRoot, "nul").ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw4_rename_symlink_never_follows_the_target) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t ino = 0;
  const std::string target = "does/not/exist";
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    auto created = fs.value()->CreateSymlink(kRoot, "l1", target, 0777, 0, 0);
    CHECK(created.ok());
    if (!created.ok()) return;
    ino = created.value();
    auto renamed = fs.value()->Rename(kRoot, "l1", kRoot, "l2", false);
    CHECK(renamed.ok());
    if (!renamed.ok()) return;
    CHECK_EQ(renamed.value().moved_ino, ino);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(!fs.value()->LookupName(kRoot, "l1").ok());
    auto entry = fs.value()->LookupName(kRoot, "l2");
    CHECK(entry.ok());
    CHECK_EQ(entry.value().ino, ino);
    auto read = fs.value()->ReadSymlink(ino);
    CHECK(read.ok());
    if (read.ok()) CHECK_STR_EQ(read.value(), target);
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// Rename/replace inside grown directories (the DirectoryEditor::Replace
// paths for the block, leaf and node formats)
// ---------------------------------------------------------------------------

TEST(rw4_rename_replace_in_leaf_directory) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  constexpr int kEntries = 200;
  uint64_t dir = 0;
  uint64_t first_ino = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(fs.value()->Mkdir(kRoot, "big", 0755, 0, 0).ok());
    dir = fs.value()->LookupName(kRoot, "big").value().ino;
    for (int i = 0; i < kEntries; ++i) {
      auto created =
          fs.value()->CreateRegularFile(dir, FixedName(i), 0644, 0, 0);
      CHECK(created.ok());
      if (!created.ok()) return;
      if (i == 0) first_ino = created.value();
    }
    auto renamed = fs.value()->Rename(dir, FixedName(0), dir, FixedName(1),
                                      /*replace_existing=*/true);
    if (!renamed.ok()) {
      std::printf("rename error: %s\n", renamed.error().Format().c_str());
    }
    CHECK(renamed.ok());
    if (!renamed.ok()) return;
    CHECK_EQ(renamed.value().moved_ino, first_ino);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  const std::string fmt = DirFormatName(fixture, dir);
  CHECK(fmt == "leaf" || fmt == "node");
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(!fs.value()->LookupName(dir, FixedName(0)).ok());
    auto moved = fs.value()->LookupName(dir, FixedName(1));
    CHECK(moved.ok());
    CHECK_EQ(moved.value().ino, first_ino);
    const std::vector<std::string> names = ListNames(*fs.value(), dir);
    CHECK_EQ(names.size(), static_cast<size_t>(kEntries - 1));
    for (int i = 0; i < kEntries; ++i) {
      if (i == 0) continue;
      CHECK(fs.value()->LookupName(dir, FixedName(i)).ok());
    }
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

TEST(rw4_rename_replace_in_node_directory) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  constexpr int kEntries = 640;
  uint64_t dir = 0;
  uint64_t moved_ino = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(fs.value()->Mkdir(kRoot, "huge", 0755, 0, 0).ok());
    dir = fs.value()->LookupName(kRoot, "huge").value().ino;
    for (int i = 0; i < kEntries; ++i) {
      auto created =
          fs.value()->CreateRegularFile(dir, FixedName(i), 0644, 0, 0);
      CHECK(created.ok());
      if (!created.ok()) return;
      if (i == 5) moved_ino = created.value();
    }
    auto renamed = fs.value()->Rename(dir, FixedName(5), dir, FixedName(6),
                                      /*replace_existing=*/true);
    if (!renamed.ok()) {
      std::printf("rename error: %s\n", renamed.error().Format().c_str());
    }
    CHECK(renamed.ok());
    if (!renamed.ok()) return;
    CHECK_EQ(renamed.value().moved_ino, moved_ino);
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CHECK(DirFormatName(fixture, dir) == std::string("node"));
  {
    auto fs = fixture.Mount(/*read_write=*/false);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(!fs.value()->LookupName(dir, FixedName(5)).ok());
    auto moved = fs.value()->LookupName(dir, FixedName(6));
    CHECK(moved.ok());
    CHECK_EQ(moved.value().ino, moved_ino);
    const std::vector<std::string> names = ListNames(*fs.value(), dir);
    CHECK_EQ(names.size(), static_cast<size_t>(kEntries - 1));
  }
  CheckConsistent(fixture);
  fixture.Reset();
}

// ---------------------------------------------------------------------------
// rmdir of a nested directory repoints the *child's* ".." at the root
// (xfs_dir_remove_child). RW-3 pointed the parent's, which corrupted the
// parent's parent link; this pins the corrected behaviour.
// ---------------------------------------------------------------------------

TEST(rw4_rmdir_of_a_nested_directory_repoints_the_child) {
  auto& fixture = xfstest::FixtureImage::Instance();
  fixture.Reset();
  uint64_t a = 0;
  uint64_t b = 0;
  {
    auto fs = fixture.Mount(/*read_write=*/true);
    CHECK(fs.ok());
    if (!fs.ok()) return;
    CHECK(fs.value()->Mkdir(kRoot, "A", 0755, 0, 0).ok());
    a = fs.value()->LookupName(kRoot, "A").value().ino;
    CHECK(fs.value()->Mkdir(a, "B", 0755, 0, 0).ok());
    b = fs.value()->LookupName(a, "B").value().ino;
    auto removed = fs.value()->RemoveEmptyDirectory(a, "B",
                                                    /*defer_inode_free=*/true);
    if (!removed.ok()) {
      std::printf("rmdir error: %s\n", removed.error().Format().c_str());
    }
    CHECK(removed.ok());
    // The child directory's ".." now points at the root, not at A.
    auto dotdot = fs.value()->LookupName(b, "..");
    CHECK(dotdot.ok());
    CHECK_EQ(dotdot.value().ino, kRoot);
    // A's own ".." is untouched.
    auto a_parent = fs.value()->LookupName(a, "..");
    CHECK(a_parent.ok());
    CHECK_EQ(a_parent.value().ino, kRoot);
    CHECK(fs.value()->FreeUnlinkedInode(b).ok());
    CHECK(fs.value()->WriteCleanUnmountRecord().ok());
    CHECK(fs.value()->Flush().ok());
  }
  CheckConsistent(fixture);
  fixture.Reset();
}
