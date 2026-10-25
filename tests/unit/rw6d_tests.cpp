// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// RW-6D: native extended attributes.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "support/fixture_device.h"
#include "test_util.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/formatter.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"
#include "xfs/xattr.h"
#include "xfs/xattr_intent.h"

using namespace xfs;

namespace di = ondisk::inode;

namespace {

struct Image {
  std::unique_ptr<xfstest::MemoryBlockDevice> device;
  XfsGeometry geo;
  std::unique_ptr<XfsFilesystem> fs;
};

Image MakeImage(bool reflink, bool rmapbt, uint32_t block_size, bool mount,
               uint64_t size_bytes = 300ull << 20) {
  Image image;
  image.device = std::make_unique<xfstest::MemoryBlockDevice>(size_bytes);
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

std::vector<uint8_t> Pattern(size_t size, uint32_t seed) {
  std::vector<uint8_t> out(size);
  uint32_t x = seed;
  for (size_t i = 0; i < size; ++i) {
    x = x * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(x >> 24);
  }
  return out;
}

XattrFork ReadFork(Image& image, uint64_t ino) {
  auto inode = image.fs->OpenInode(ino);
  CHECK(inode.ok());
  auto fork = ReadXattrFork(*image.device, image.geo, inode.value());
  if (!fork.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, fork.error().Format());
  }
  return fork.value();
}

void Set(Image& image, uint64_t ino, const std::string& name,
         const std::vector<uint8_t>& value,
         XfsFilesystem::XattrSetMode mode =
             XfsFilesystem::XattrSetMode::kUpsert) {
  auto stored = image.fs->SetXattr(
      ino, name, value.empty() ? nullptr : value.data(), value.size(), mode);
  if (!stored.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, stored.error().Format());
  }
}

std::vector<uint8_t> Get(Image& image, uint64_t ino,
                         const std::string& name) {
  auto stored = image.fs->GetXattr(ino, name);
  if (!stored.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, stored.error().Format());
  }
  return stored.value().value;
}

void ExpectBytes(const std::vector<uint8_t>& actual,
                 const std::vector<uint8_t>& expected) {
  CHECK_EQ(actual.size(), expected.size());
  CHECK(std::memcmp(actual.data(), expected.data(), actual.size()) == 0);
}

void PutLE16At(void* base, size_t off, uint16_t value) {
  StoreLE16(static_cast<uint8_t*>(base) + off, value);
}

void PutLE32At(void* base, size_t off, uint32_t value) {
  StoreLE32(static_cast<uint8_t*>(base) + off, value);
}

void PutLE64At(void* base, size_t off, uint64_t value) {
  StoreLE64(static_cast<uint8_t*>(base) + off, value);
}

void Finish(Image& image) {
  CHECK(image.fs->WriteCleanUnmountRecord().ok());
  CheckReport report = image.fs->CheckConsistency(CheckOptions{});
  if (report.Overall() != CheckStatus::kOk) {
    std::string detail = report.Summary();
    for (const CheckItem& item : report.items) {
      if (item.status == CheckStatus::kOk) continue;
      detail += "\n" + std::string(ToString(item.status)) + " " + item.name +
                ": " + item.message;
    }
    ::xfstest::Fail(__FILE__, __LINE__, detail);
  }
  image.fs.reset();
}

void TestProfile(bool reflink, bool rmapbt) {
  Image image = MakeImage(reflink, rmapbt, 1024, true);
  auto file = image.fs->CreateRegularFile(image.fs->RootInode(), "x", 0644, 0,
                                          0);
  CHECK(file.ok());
  const uint64_t ino = file.value();

  // Shortform.
  Set(image, ino, "user.small", {'h', 'e', 'l', 'l', 'o'});
  XattrFork fork = ReadFork(image, ino);
  CHECK(fork.format == XattrForkFormat::kShortform);
  ExpectBytes(Get(image, ino, "user.small"), {'h', 'e', 'l', 'l', 'o'});
  {
    // The on-disk shortform entry is the unprefixed name, a 3-byte header and
    // no padding: 4-byte fork header + 3 + 5 + 5 = 17 bytes.
    auto raw = image.fs->OpenInode(ino);
    CHECK(raw.ok());
    const std::vector<uint8_t>& attr = raw.value().attr_fork;
    CHECK_EQ(GetBE16(attr.data(), 0), 17u);
    CHECK_EQ(attr[2], 1u);
    CHECK_EQ(attr[4], 5u);  // namelen
    CHECK_EQ(attr[5], 5u);  // valuelen
    CHECK_EQ(attr[6], 0u);  // namespace
    CHECK(std::memcmp(attr.data() + 7, "small", 5) == 0);
    CHECK(std::memcmp(attr.data() + 12, "hello", 5) == 0);
  }

  // The shortform value field is u8: 300 bytes must become a leaf.
  auto v300 = Pattern(300, 0x6d01);
  Set(image, ino, "user.v300", v300);
  fork = ReadFork(image, ino);
  CHECK(fork.format == XattrForkFormat::kLeaf);
  ExpectBytes(Get(image, ino, "user.v300"), v300);
  {
    // The single free region must be between the 8-byte entry table and the
    // first name/value byte.  Claiming the used name/value area as free makes
    // Linux insert over existing attributes.
    auto fsb = AttrLogicalToPhysical(fork, image.geo, 0);
    CHECK(fsb.ok());
    std::vector<uint8_t> block(image.geo.block_size);
    CHECK(image.device->Read(image.geo.FsbToByte(fsb.value()), block.data(),
                            block.size()));
    const uint32_t count = GetBE16(block.data(), 56);
    const uint32_t firstused = GetBE16(block.data(), 60);
    const uint32_t entries_end = 80 + count * 8;
    const uint32_t free_base = GetBE16(block.data(), 64);
    const uint32_t free_size = GetBE16(block.data(), 66);
    CHECK_EQ(free_base, entries_end);
    CHECK_EQ(free_base + free_size, firstused);
    CHECK(entries_end <= firstused);
  }

  // Remote value (512-byte blocks: the local entry limit is 384 bytes).
  auto remote = Pattern(3000, 0x6d02);
  Set(image, ino, "user.remote", remote);
  fork = ReadFork(image, ino);
  CHECK(fork.format == XattrForkFormat::kLeaf);
  CHECK(fork.remote_blocks >= 3);
  auto got_remote = image.fs->GetXattr(ino, "user.remote");
  CHECK(got_remote.ok());
  CHECK(got_remote.value().remote);
  ExpectBytes(got_remote.value().value, remote);

  // Replace remote -> remote (new allocation first, old freed).
  auto remote2 = Pattern(4100, 0x6d03);
  Set(image, ino, "user.remote", remote2, XfsFilesystem::XattrSetMode::kReplace);
  ExpectBytes(Get(image, ino, "user.remote"), remote2);
  // Remote -> local.
  Set(image, ino, "user.remote", {'x'});
  auto local = image.fs->GetXattr(ino, "user.remote");
  CHECK(local.ok());
  CHECK(!local.value().remote);
  ExpectBytes(local.value().value, {'x'});

  // Namespaces.
  Set(image, ino, "trusted.rootonly", {'r'});
  Set(image, ino, "security.selinux", {'s', 0, 'l'});
  Set(image, ino, "system.posix_acl_access", {'a', 'c', 'l'});
  auto list = image.fs->ListXattrs(ino);
  CHECK(list.ok());
  bool saw_trusted = false, saw_security = false, saw_acl = false;
  for (const XattrEntry& e : list.value()) {
    const std::string name = e.DisplayName();
    if (name == "trusted.rootonly") saw_trusted = true;
    if (name == "security.selinux") saw_security = true;
    if (name == "system.posix_acl_access") saw_acl = true;
  }
  CHECK(saw_trusted && saw_security && saw_acl);
  ExpectBytes(Get(image, ino, "system.posix_acl_access"), {'a', 'c', 'l'});

  // XATTR_CREATE / XATTR_REPLACE semantics.
  auto exists = image.fs->SetXattr(ino, "user.small", "z", 1,
                                   XfsFilesystem::XattrSetMode::kCreate);
  CHECK(!exists.ok());
  CHECK(exists.error().code == XfsError::kAlreadyExists);
  auto missing = image.fs->SetXattr(ino, "user.nope", "z", 1,
                                    XfsFilesystem::XattrSetMode::kReplace);
  CHECK(!missing.ok());
  CHECK(missing.error().code == XfsError::kNotFound);

  // Bounds.
  auto too_big = image.fs->SetXattr(
      ino, "user.toobig", std::string(65537, 'q').data(), 65537,
      XfsFilesystem::XattrSetMode::kUpsert);
  CHECK(!too_big.ok());
  CHECK(too_big.error().code == XfsError::kOutOfRange);

  // Remove and verify the value really disappears.
  CHECK(image.fs->RemoveXattr(ino, "user.remote").ok());
  auto gone = image.fs->GetXattr(ino, "user.remote");
  CHECK(!gone.ok());
  CHECK(gone.error().code == XfsError::kNotFound);

  Finish(image);
}

}  // namespace

TEST(rw6d_shortform_leaf_remote_and_namespaces) {
  TestProfile(false, false);
  TestProfile(false, true);
  TestProfile(true, false);
  TestProfile(true, true);
}

TEST(rw6d_node_tree_growth_and_shrink) {
  Image image = MakeImage(true, true, 1024, true);
  auto file = image.fs->CreateRegularFile(image.fs->RootInode(), "node", 0644,
                                          0, 0);
  CHECK(file.ok());
  const uint64_t ino = file.value();
  constexpr int kAttrs = 140;
  std::vector<std::vector<uint8_t>> values;
  values.reserve(kAttrs);
  for (int i = 0; i < kAttrs; ++i) {
    values.push_back(Pattern(600, 0x9000u + static_cast<uint32_t>(i)));
    char name[32];
    std::snprintf(name, sizeof(name), "user.n%03d", i);
    Set(image, ino, name, values.back());
  }
  XattrFork fork = ReadFork(image, ino);
  CHECK(fork.format == XattrForkFormat::kNode);
  CHECK(fork.root_level >= 2);  // must have exercised a multi-level DA tree
  CHECK(fork.leaf_count >= 2);
  CHECK(fork.nodes.size() >= 3);
  for (int i = 0; i < kAttrs; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "user.n%03d", i);
    ExpectBytes(Get(image, ino, name), values[static_cast<size_t>(i)]);
  }

  // Replace every 10th value with a remote one, then back to local.
  for (int i = 0; i < kAttrs; i += 10) {
    auto big = Pattern(2500, 0xa000u + static_cast<uint32_t>(i));
    char name[32];
    std::snprintf(name, sizeof(name), "user.n%03d", i);
    Set(image, ino, name, big, XfsFilesystem::XattrSetMode::kReplace);
    ExpectBytes(Get(image, ino, name), big);
  }
  fork = ReadFork(image, ino);
  CHECK(fork.format == XattrForkFormat::kNode);
  CHECK(fork.remote_blocks > 0);

  // Remove in reverse; the tree must merge/collapse and finally return to
  // shortform or no fork.
  for (int i = kAttrs - 1; i >= 0; --i) {
    char name[32];
    std::snprintf(name, sizeof(name), "user.n%03d", i);
    CHECK(image.fs->RemoveXattr(ino, name).ok());
  }
  fork = ReadFork(image, ino);
  CHECK(fork.format == XattrForkFormat::kShortform ||
        fork.format == XattrForkFormat::kNone ||
        fork.format == XattrForkFormat::kExtentsEmpty);
  CHECK(fork.entries.empty());
  Finish(image);
}

TEST(rw6d_data_fork_conversion_and_btree_attr_fork) {
  // A 512-byte inode with many data extents cannot keep forkoff 0 while an
  // attribute fork appears; the data fork must migrate to btree format.
  Image image = MakeImage(true, true, 1024, true);
  auto file = image.fs->CreateRegularFile(image.fs->RootInode(), "df", 0644, 0,
                                          0);
  CHECK(file.ok());
  const uint64_t ino = file.value();
  for (int i = 0; i < 15; ++i) {
    auto allocated = image.fs->AllocateUnwritten(
        ino, static_cast<uint64_t>(i) * 4096, 512);
    CHECK(allocated.ok());
  }
  auto before = image.fs->OpenInode(ino);
  CHECK(before.ok());
  CHECK(before.value().format == di::kFormatExtents);
  CHECK(before.value().nextents == 15);

  Set(image, ino, "user.force", Pattern(200, 0xd00d));
  auto after = image.fs->OpenInode(ino);
  CHECK(after.ok());
  CHECK(after.value().format == di::kFormatBtree);
  CHECK(after.value().HasAttrFork());
  ExpectBytes(Get(image, ino, "user.force"), Pattern(200, 0xd00d));

  CHECK(image.fs->RemoveXattr(ino, "user.force").ok());
  auto empty = image.fs->OpenInode(ino);
  CHECK(empty.ok());
  // Linux keeps an empty shortform attr fork when the data fork is a btree.
  CHECK(empty.value().forkoff != 0);
  CHECK(empty.value().aformat == di::kFormatLocal);
  CHECK(empty.value().anextents == 0);
  CHECK(empty.value().format == di::kFormatBtree);
  Finish(image);
}

TEST(rw6d_attr_fork_btree_and_unlink_lifecycle) {
  Image image = MakeImage(true, true, 1024, true);
  auto file = image.fs->CreateRegularFile(image.fs->RootInode(), "del", 0644,
                                          0, 0);
  CHECK(file.ok());
  const uint64_t ino = file.value();
  auto filler = image.fs->CreateRegularFile(image.fs->RootInode(), "filler",
                                            0644, 0, 0);
  CHECK(filler.ok());
  // Many leaves plus interleaved one-block allocations fragment the attr
  // fork's logical-to-physical map, forcing the extent list into a real BMBT.
  for (int i = 0; i < 80; ++i) {
    if (i >= 8) {
      auto allocated = image.fs->AllocateUnwritten(
          filler.value(), static_cast<uint64_t>(i) * 512, 512);
      CHECK(allocated.ok());
    }
    char name[32];
    std::snprintf(name, sizeof(name), "user.d%03d", i);
    Set(image, ino, name, Pattern(180, 0xe000u + static_cast<uint32_t>(i)));
  }
  auto inode = image.fs->OpenInode(ino);
  CHECK(inode.ok());
  CHECK(inode.value().aformat == di::kFormatBtree);
  auto fork = ReadFork(image, ino);
  CHECK(fork.format == XattrForkFormat::kNode);
  // Last link + attributes: the inode must be completely reclaimable.
  CHECK(image.fs->Unlink(image.fs->RootInode(), "del").ok());
  auto freed = image.fs->OpenInode(ino);
  CHECK(!freed.ok());
  Finish(image);
}

TEST(rw6d_hash_collision_and_ordering) {
  // FNV-style deterministic search for two names with the same XFS DA hash.
  std::map<uint32_t, std::string> seen;
  std::string first, second;
  for (uint32_t i = 0; i < 2000000; ++i) {
    std::string name = "c" + std::to_string(i);
    const uint32_t h = AttrHashName(name);
    auto it = seen.find(h);
    if (it != seen.end() && it->second != name) {
      first = it->second;
      second = name;
      break;
    }
    seen.emplace(h, name);
  }
  CHECK(!first.empty());
  CHECK(AttrHashName(first) == AttrHashName(second));

  Image image = MakeImage(false, false, 4096, true);
  auto file = image.fs->CreateRegularFile(image.fs->RootInode(), "coll", 0644,
                                          0, 0);
  CHECK(file.ok());
  const uint64_t ino = file.value();
  Set(image, ino, "user." + first, {'A'});
  Set(image, ino, "user." + second, {'B'});
  ExpectBytes(Get(image, ino, "user." + first), {'A'});
  ExpectBytes(Get(image, ino, "user." + second), {'B'});
  Set(image, ino, "user." + first, {'C'},
      XfsFilesystem::XattrSetMode::kReplace);
  ExpectBytes(Get(image, ino, "user." + first), {'C'});
  ExpectBytes(Get(image, ino, "user." + second), {'B'});
  CHECK(image.fs->RemoveXattr(ino, "user." + second).ok());
  ExpectBytes(Get(image, ino, "user." + first), {'C'});
  Finish(image);
}

TEST(rw6d_delete_many_attr_extents) {
  // More attribute extents than the inode-free transaction budget: the attr
  // free must roll across transactions, then free the inode.
  Image image = MakeImage(true, true, 1024, true);
  auto file = image.fs->CreateRegularFile(image.fs->RootInode(), "bigdel",
                                          0644, 0, 0);
  CHECK(file.ok());
  const uint64_t ino = file.value();
  auto filler = image.fs->CreateRegularFile(image.fs->RootInode(), "del-fill",
                                            0644, 0, 0);
  CHECK(filler.ok());
  for (int i = 0; i < 130; ++i) {
    if (i >= 4) {
      auto allocated = image.fs->AllocateUnwritten(
          filler.value(), static_cast<uint64_t>(i) * 512, 512);
      CHECK(allocated.ok());
    }
    char name[32];
    std::snprintf(name, sizeof(name), "user.del%03d", i);
    Set(image, ino, name, Pattern(600, 0xdd00u + i));
  }
  auto fork = ReadFork(image, ino);
  CHECK(fork.format == XattrForkFormat::kNode);
  CHECK(fork.extents.size() >= 48);
  CHECK(image.fs->Unlink(image.fs->RootInode(), "bigdel").ok());
  auto freed = image.fs->OpenInode(ino);
  CHECK(!freed.ok());
  Finish(image);
}

TEST(rw6d_format_transitions_reverse) {
  Image image = MakeImage(true, true, 1024, true);
  auto file = image.fs->CreateRegularFile(image.fs->RootInode(), "reverse",
                                          0644, 0, 0);
  CHECK(file.ok());
  const uint64_t ino = file.value();
  constexpr int kAttrs = 130;  // 130 leaves > 120 node entries: level 2
  std::vector<std::vector<uint8_t>> values;
  values.reserve(kAttrs);
  for (int i = 0; i < kAttrs; ++i) {
    values.push_back(Pattern(600, 0xc000u + static_cast<uint32_t>(i)));
    char name[32];
    std::snprintf(name, sizeof(name), "user.r%03d", i);
    Set(image, ino, name, values.back());
  }
  XattrFork fork = ReadFork(image, ino);
  CHECK(fork.format == XattrForkFormat::kNode);
  CHECK(fork.root_level >= 2);

  // Shrink back through the tree, a single leaf and no fork at all.
  for (int i = kAttrs - 1; i >= 8; --i) {
    char name[32];
    std::snprintf(name, sizeof(name), "user.r%03d", i);
    CHECK(image.fs->RemoveXattr(ino, name).ok());
  }
  fork = ReadFork(image, ino);
  CHECK(fork.entries.size() == 8);
  CHECK(fork.format == XattrForkFormat::kNode ||
        fork.format == XattrForkFormat::kLeaf);
  for (int i = 0; i < 8; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "user.r%03d", i);
    ExpectBytes(Get(image, ino, name), values[static_cast<size_t>(i)]);
  }
  for (int i = 7; i >= 1; --i) {
    char name[32];
    std::snprintf(name, sizeof(name), "user.r%03d", i);
    CHECK(image.fs->RemoveXattr(ino, name).ok());
  }
  fork = ReadFork(image, ino);
  CHECK(fork.entries.size() == 1);
  CHECK(fork.format == XattrForkFormat::kLeaf ||
        fork.format == XattrForkFormat::kShortform);
  CHECK(image.fs->RemoveXattr(ino, "user.r000").ok());
  fork = ReadFork(image, ino);
  CHECK(fork.entries.empty());
  CHECK(fork.format == XattrForkFormat::kNone ||
        fork.format == XattrForkFormat::kExtentsEmpty);

  Set(image, ino, "user.again", {'x'});
  fork = ReadFork(image, ino);
  CHECK(fork.format == XattrForkFormat::kShortform);
  CHECK(image.fs->RemoveXattr(ino, "user.again").ok());
  fork = ReadFork(image, ino);
  CHECK(fork.format == XattrForkFormat::kNone ||
        fork.format == XattrForkFormat::kExtentsEmpty);
  Finish(image);
}

TEST(rw6d_enospc_preserves_existing_attributes) {
  Image image = MakeImage(true, true, 1024, true);
  auto keep = image.fs->CreateRegularFile(image.fs->RootInode(), "keep", 0644,
                                          0, 0);
  CHECK(keep.ok());
  const uint64_t keep_ino = keep.value();
  Set(image, keep_ino, "user.keep", {'k', 'e', 'e', 'p'});
  auto old = Pattern(100, 0x5151);
  Set(image, keep_ino, "user.old", old);

  auto fill = image.fs->CreateRegularFile(image.fs->RootInode(), "fill", 0644,
                                          0, 0);
  CHECK(fill.ok());
  const uint64_t fill_ino = fill.value();
  uint64_t offset = 0;
  bool exhausted = false;
  for (uint64_t len = 1ull << 20; len >= 4096; len >>= 1) {
    while (offset + len < (270ull << 20)) {
      auto allocated = image.fs->AllocateUnwritten(fill_ino, offset, len);
      if (!allocated.ok()) {
        CHECK(allocated.error().code == XfsError::kNoSpace);
        break;
      }
      offset += len;
    }
    if (offset + len >= (270ull << 20)) continue;
    if (len == 4096) exhausted = true;
  }
  CHECK(exhausted);
  CHECK(offset > (150ull << 20));  // the filesystem really is full

  auto big = Pattern(60000, 0x6161);
  auto failed = image.fs->SetXattr(keep_ino, "user.big", big.data(),
                                   big.size(),
                                   XfsFilesystem::XattrSetMode::kUpsert);
  CHECK(!failed.ok());
  CHECK(failed.error().code == XfsError::kNoSpace);
  auto failed_replace = image.fs->SetXattr(
      keep_ino, "user.old", big.data(), big.size(),
      XfsFilesystem::XattrSetMode::kReplace);
  CHECK(!failed_replace.ok());
  CHECK(failed_replace.error().code == XfsError::kNoSpace);
  ExpectBytes(Get(image, keep_ino, "user.old"), old);
  ExpectBytes(Get(image, keep_ino, "user.keep"), {'k', 'e', 'e', 'p'});

  // A shortform set consumes only inode literal space and must still work.
  Set(image, keep_ino, "user.small", {'s'});
  ExpectBytes(Get(image, keep_ino, "user.small"), {'s'});
  CHECK(image.fs->RemoveXattr(keep_ino, "user.small").ok());

  // Freeing the filler must make the same remote set succeed.
  CHECK(image.fs->SetFileSize(fill_ino, 0).ok());
  auto stored = image.fs->SetXattr(keep_ino, "user.big", big.data(),
                                   big.size(),
                                   XfsFilesystem::XattrSetMode::kUpsert);
  CHECK(stored.ok());
  CHECK(stored.value().remote);
  ExpectBytes(Get(image, keep_ino, "user.big"), big);
  CHECK(image.fs->RemoveXattr(keep_ino, "user.big").ok());
  Finish(image);
}

TEST(rw6d_attr_fork_growth_moves_data_bmbt_root) {
  // A data-fork btree with more root records than fit at the default attr
  // fork offset.  Creating a leaf attribute shrinks the data fork; the writer
  // must move the bmdr root into a real bmbt block (xfs_btree_new_iroot)
  // instead of truncating it.
  Image image = MakeImage(false, true, 1024, true);
  auto file = image.fs->CreateRegularFile(image.fs->RootInode(), "rootgrow",
                                          0644, 0, 0);
  CHECK(file.ok());
  const uint64_t ino = file.value();
  constexpr int kExtents = 700;
  for (int i = 0; i < kExtents; ++i) {
    auto allocated = image.fs->AllocateUnwritten(
        ino, static_cast<uint64_t>(i) * 2048, 512);
    CHECK(allocated.ok());
  }
  auto before = image.fs->OpenInode(ino);
  CHECK(before.ok());
  CHECK(before.value().format == di::kFormatBtree);
  CHECK(before.value().forkoff == 0);
  const uint16_t old_level = GetBE16(before.value().data_fork.data(), 0);
  const uint16_t old_recs = GetBE16(before.value().data_fork.data(), 2);
  CHECK(old_recs > 11);  // more than fit in the 192-byte fork at forkoff 24

  auto value = Pattern(300, 0x7777);
  Set(image, ino, "user.force", value);
  auto after = image.fs->OpenInode(ino);
  CHECK(after.ok());
  CHECK(after.value().format == di::kFormatBtree);
  CHECK(after.value().HasAttrFork());
  CHECK_EQ(GetBE16(after.value().data_fork.data(), 0),
           static_cast<uint16_t>(old_level + 1));
  CHECK_EQ(GetBE16(after.value().data_fork.data(), 2), 1u);
  auto extents = image.fs->GetDataExtents(ino);
  CHECK(extents.ok());
  CHECK_EQ(extents.value().size(), static_cast<size_t>(kExtents));
  ExpectBytes(Get(image, ino, "user.force"), value);

  CHECK(image.fs->RemoveXattr(ino, "user.force").ok());
  auto grown = image.fs->OpenInode(ino);
  CHECK(grown.ok());
  CHECK(grown.value().format == di::kFormatBtree);
  auto extents_after = image.fs->GetDataExtents(ino);
  CHECK(extents_after.ok());
  CHECK_EQ(extents_after.value().size(), static_cast<size_t>(kExtents));
  Finish(image);
}

TEST(rw6d_attributes_follow_namespace_operations) {
  Image image = MakeImage(false, true, 1024, true);
  const uint64_t root = image.fs->RootInode();
  CHECK(image.fs->Mkdir(root, "d", 0755, 0, 0).ok());
  auto dir = image.fs->LookupPath("/d");
  CHECK(dir.ok());
  const uint64_t dir_ino = dir.value().ino;
  Set(image, dir_ino, "user.dir", {'d'});
  auto dirs = image.fs->ListXattrs(dir_ino);
  CHECK(dirs.ok());
  CHECK_EQ(dirs.value().size(), 1u);

  auto renamed = image.fs->Rename(root, "d", root, "e", false);
  CHECK(renamed.ok());
  ExpectBytes(Get(image, dir_ino, "user.dir"), {'d'});
  CHECK(image.fs->RemoveEmptyDirectory(root, "e").ok());

  auto link = image.fs->CreateSymlink(root, "s", "target", 0777, 0, 0);
  CHECK(link.ok());
  const uint64_t link_ino = link.value();
  Set(image, link_ino, "user.sym", {'s'});
  ExpectBytes(Get(image, link_ino, "user.sym"), {'s'});
  CHECK(image.fs->Unlink(root, "s").ok());

  auto file = image.fs->CreateRegularFile(root, "f", 0644, 0, 0);
  CHECK(file.ok());
  const uint64_t file_ino = file.value();
  Set(image, file_ino, "user.f", {'f'});
  CHECK(image.fs->Link(file_ino, root, "f2").ok());
  ExpectBytes(Get(image, file_ino, "user.f"), {'f'});
  CHECK(image.fs->Unlink(root, "f2").ok());
  ExpectBytes(Get(image, file_ino, "user.f"), {'f'});

  auto target = image.fs->CreateRegularFile(root, "t", 0644, 0, 0);
  CHECK(target.ok());
  const uint64_t target_ino = target.value();
  Set(image, target_ino, "user.t", {'t'});
  auto replaced = image.fs->Rename(root, "f", root, "t", true);
  CHECK(replaced.ok());
  CHECK(replaced.value().moved_ino == file_ino);
  ExpectBytes(Get(image, file_ino, "user.f"), {'f'});
  CHECK(image.fs->RemoveXattr(file_ino, "user.f").ok());
  CHECK(image.fs->Unlink(root, "t").ok());
  Finish(image);
}

TEST(rw6d_attr_intent_parser) {
  Image image = MakeImage(false, false, 4096, false);
  const XfsGeometry& geo = image.geo;
  const uint64_t ino = geo.rootino;
  auto build = [&](uint32_t op, uint32_t filter, const std::string& name,
                   const std::vector<uint8_t>& value, uint64_t id,
                   std::vector<std::vector<uint8_t>>* regions) {
    std::vector<uint8_t> format(40, 0);
    PutLE16At(format.data(), 0, 0x1246);
    PutLE16At(format.data(), 2,
              static_cast<uint16_t>(2 + (value.empty() ? 0 : 1)));
    PutLE32At(format.data(), 4, 0);
    PutLE64At(format.data(), 8, id);
    PutLE64At(format.data(), 16, ino);
    PutLE32At(format.data(), 24, op);
    PutLE32At(format.data(), 28, static_cast<uint32_t>(name.size()));
    PutLE32At(format.data(), 32, static_cast<uint32_t>(value.size()));
    PutLE32At(format.data(), 36, filter);
    regions->push_back(std::move(format));
    std::vector<uint8_t> name_region((name.size() + 3) & ~size_t{3}, 0);
    std::memcpy(name_region.data(), name.data(), name.size());
    regions->push_back(std::move(name_region));
    if (!value.empty()) {
      std::vector<uint8_t> value_region((value.size() + 3) & ~size_t{3}, 0);
      std::memcpy(value_region.data(), value.data(), value.size());
      regions->push_back(std::move(value_region));
    }
  };

  {
    std::vector<std::vector<uint8_t>> regions;
    build(1, 0, "user.intent", {'v', 'a', 'l'}, 0xabc, &regions);
    auto parsed = ParseXattrIntent(regions, geo);
    CHECK(parsed.ok());
    CHECK_EQ(parsed.value().id, 0xabcu);
    CHECK_EQ(parsed.value().ino, ino);
    CHECK(parsed.value().op == XattrIntentOp::kSet);
    CHECK_EQ(parsed.value().name, "user.intent");
    ExpectBytes(parsed.value().value, {'v', 'a', 'l'});
  }
  {
    std::vector<std::vector<uint8_t>> regions;
    build(2, kAttrRoot, "trusted.name", {}, 0xdef, &regions);
    auto parsed = ParseXattrIntent(regions, geo);
    CHECK(parsed.ok());
    CHECK(parsed.value().op == XattrIntentOp::kRemove);
    CHECK(parsed.value().value.empty());
    CHECK_EQ(parsed.value().attr_filter, kAttrRoot);
  }
  {
    std::vector<std::vector<uint8_t>> regions;
    build(3, kAttrSecure, "security.selinux", {'x'}, 0x111, &regions);
    auto parsed = ParseXattrIntent(regions, geo);
    CHECK(parsed.ok());
    CHECK(parsed.value().op == XattrIntentOp::kReplace);
  }
  {
    // Parent-pointer operations are parsed far enough to identify them and
    // then refused as outside RW-6D.
    std::vector<std::vector<uint8_t>> regions;
    build(4, kAttrParent, "parent.name", {'p'}, 0x222, &regions);
    auto parsed = ParseXattrIntent(regions, geo);
    CHECK(!parsed.ok());
    CHECK(parsed.error().code == XfsError::kUnsupportedFeature);
  }
  {
    std::vector<uint8_t> done(16, 0);
    PutLE16At(done.data(), 0, 0x1247);
    PutLE16At(done.data(), 2, 1);
    PutLE64At(done.data(), 8, 0x12345678);
    auto parsed = ParseXattrIntentDone(done);
    CHECK(parsed.ok());
    CHECK_EQ(parsed.value().id, 0x12345678u);
  }
  {
    std::vector<std::vector<uint8_t>> regions;
    build(1, 0, "user.intent", {}, 0xabc, &regions);
    regions[0][0] = 0;  // wrong item type
    CHECK(!ParseXattrIntent(regions, geo).ok());
  }
  {
    std::vector<std::vector<uint8_t>> regions;
    build(1, 0, "user.intent", {'v'}, 0xabc, &regions);
    regions.pop_back();  // drop the value region
    CHECK(!ParseXattrIntent(regions, geo).ok());
  }
  {
    std::vector<std::vector<uint8_t>> regions;
    build(1, kAttrRoot | kAttrSecure, "user.bad", {'v'}, 0xabc, &regions);
    CHECK(!ParseXattrIntent(regions, geo).ok());
  }
  {
    std::vector<std::vector<uint8_t>> regions;
    build(1, 0, "user.intent", {'v'}, 0xabc, &regions);
    PutLE64At(regions[0].data(), 16, ~0ULL);  // inode out of range
    CHECK(!ParseXattrIntent(regions, geo).ok());
  }
}

TEST(rw6d_data_mutations_preserve_attributes) {
  Image image = MakeImage(true, true, 1024, true);
  auto file = image.fs->CreateRegularFile(image.fs->RootInode(), "src", 0644,
                                          0, 0);
  CHECK(file.ok());
  const uint64_t src_ino = file.value();
  auto original = Pattern(8 * 1024, 0x7171);
  CHECK(image.fs->WriteFileData(src_ino, 0, original.data(),
                                original.size())
            .ok());
  auto remote = Pattern(3000, 0x7272);
  Set(image, src_ino, "user.remote", remote);
  Set(image, src_ino, "user.small", {'s'});

  auto clone = image.fs->CreateRegularFile(image.fs->RootInode(), "clone",
                                           0644, 0, 0);
  CHECK(clone.ok());
  auto cloned = image.fs->ReflinkRange(src_ino, 0, clone.value(), 0, 0);
  CHECK(cloned.ok());
  ExpectBytes(Get(image, src_ino, "user.remote"), remote);
  auto clone_attrs = image.fs->ListXattrs(clone.value());
  CHECK(clone_attrs.ok());
  CHECK(clone_attrs.value().empty());

  auto patch = Pattern(1024, 0x7373);
  CHECK(image.fs->WriteFileData(src_ino, 0, patch.data(), patch.size()).ok());
  ExpectBytes(Get(image, src_ino, "user.remote"), remote);
  ExpectBytes(Get(image, src_ino, "user.small"), {'s'});

  CHECK(image.fs->SetFileSize(src_ino, 2048).ok());
  ExpectBytes(Get(image, src_ino, "user.remote"), remote);
  CHECK(image.fs->SetFileSize(src_ino, 12000).ok());
  ExpectBytes(Get(image, src_ino, "user.remote"), remote);
  CHECK(image.fs->Unlink(image.fs->RootInode(), "clone").ok());
  Finish(image);
}
