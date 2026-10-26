// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// RW-6E: parent pointers and modern default-profile qualification.
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
#include "xfs/log_checker.h"
#include "xfs/log_format.h"
#include "xfs/log_record.h"
#include "xfs/ondisk.h"
#include "xfs/parent.h"
#include "xfs/superblock.h"
#include "xfs/write.h"
#include "xfs/xattr.h"
#include "xfs/xattr_intent.h"

using namespace xfs;

namespace {

struct Image {
  std::unique_ptr<xfstest::MemoryBlockDevice> device;
  XfsGeometry geo;
  std::unique_ptr<XfsFilesystem> fs;
};

Image MakeImage(bool parent, uint64_t size = 400ull << 20,
                uint32_t block_size = 4096) {
  Image image;
  image.device = std::make_unique<xfstest::MemoryBlockDevice>(size);
  mkfs::FormatOptions options;
  options.block_size = block_size;
  options.inode_size = 512;
  options.quiet = true;
  options.finobt = true;
  options.inobtcount = true;
  options.sparse_inodes = true;
  options.bigtime = true;
  options.nrext64 = true;
  options.rmapbt = true;
  options.reflink = true;
  options.parent_pointers = parent;
  auto formatted = mkfs::Format(*image.device, options);
  if (!formatted.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, formatted.error().Format());
  }
  auto sb = ReadSuperblock(*image.device, 0, true);
  CHECK(sb.ok());
  image.geo = MakeGeometry(sb.value());
  MountOptions mount_options;
  mount_options.read_write = true;
  mount_options.allow_parent_qualification = parent;
  auto borrowed =
      std::make_unique<xfstest::BorrowedBlockDevice>(image.device.get());
  auto mounted = XfsFilesystem::Mount(std::move(borrowed), mount_options);
  if (!mounted.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, mounted.error().Format());
  }
  image.fs = std::move(mounted.value());
  return image;
}

std::vector<ParentPointer> Parents(Image& image, uint64_t ino) {
  auto inode = image.fs->OpenInode(ino);
  CHECK(inode.ok());
  auto parents = ListParents(*image.device, image.geo, inode.value());
  if (!parents.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, parents.error().Format());
  }
  auto value = parents.value();
  std::sort(value.begin(), value.end(),
            [](const ParentPointer& a, const ParentPointer& b) {
              if (a.parent_ino != b.parent_ino) return a.parent_ino < b.parent_ino;
              if (a.parent_gen != b.parent_gen) return a.parent_gen < b.parent_gen;
              return a.name < b.name;
            });
  return value;
}

void Finish(Image& image) {
  CHECK(image.fs->WriteCleanUnmountRecord().ok());
  CheckReport report = image.fs->CheckConsistency(CheckOptions{});
  if (report.Overall() != CheckStatus::kOk) {
    std::string detail = report.Summary();
    for (const CheckItem& item : report.items) {
      if (item.status != CheckStatus::kOk) {
        detail += "\n" + item.name + ": " + item.message;
      }
    }
    ::xfstest::Fail(__FILE__, __LINE__, detail);
  }
  auto parents = image.fs->VerifyParentPointers();
  if (!parents.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, parents.error().Format());
  }
  if (!parents.value().clean()) {
    ::xfstest::Fail(__FILE__, __LINE__,
                    "parent audit failed: " + parents.value().Summary());
  }
  CHECK(image.device->Flush());
}

uint64_t Create(Image& image, uint64_t parent, const std::string& name) {
  auto ino = image.fs->CreateRegularFile(parent, name, 0644, 0, 0);
  if (!ino.ok()) {
    ::xfstest::Fail(__FILE__, __LINE__, ino.error().Format());
  }
  return ino.value();
}

uint64_t Mkdir(Image& image, uint64_t parent, const std::string& name) {
  Status s = image.fs->Mkdir(parent, name, 0755, 0, 0);
  if (!s.ok()) ::xfstest::Fail(__FILE__, __LINE__, s.error().Format());
  auto entry = image.fs->LookupName(parent, name);
  CHECK(entry.ok());
  return entry.value().ino;
}

void Link(Image& image, uint64_t ino, uint64_t parent, const std::string& name) {
  Status s = image.fs->Link(ino, parent, name);
  if (!s.ok()) ::xfstest::Fail(__FILE__, __LINE__, s.error().Format());
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

std::vector<LogOp> CommitOps(uint32_t tid,
                             std::vector<std::vector<uint8_t>> regions) {
  std::vector<LogOp> ops;
  ops.push_back(LogOp(logfmt::kFlagStartTrans, {}));
  std::vector<uint8_t> trans_header;
  AppendTransHeader(&trans_header, tid,
                    static_cast<uint32_t>(regions.size()));
  ops.push_back(LogOp(0, std::move(trans_header)));
  for (auto& region : regions) ops.push_back(LogOp(0, std::move(region)));
  ops.push_back(LogOp(logfmt::kFlagCommitTrans, {}));
  return ops;
}

std::vector<uint8_t> PaddedRegion(const std::vector<uint8_t>& bytes) {
  std::vector<uint8_t> region((bytes.size() + 3) & ~size_t{3}, 0);
  std::memcpy(region.data(), bytes.data(), bytes.size());
  return region;
}

}  // namespace

TEST(rw6e_parent_typed_model_and_hash) {
  XfsGeometry geo;
  geo.agcount = 4;
  geo.agblocks = 1000;
  geo.dblocks = 4000;
  geo.block_log = 12;
  geo.agblklog = 10;
  geo.inopblog = 3;
  geo.inode_log = 13;
  geo.agino_log = geo.agblklog + geo.inopblog;
  geo.agno_log = 2;
  geo.inode_size = 512;

  ParentPointer pointer{64, 0x12345678, "name"};
  XattrEntry entry = MakeParentPointerEntry(pointer);
  CHECK_EQ(entry.flags, kAttrParent);
  CHECK_EQ(entry.hash, ParentPointerHash("name", 64));
  CHECK_EQ(entry.value.size(), kParentRecSize);
  CHECK_EQ(GetBE64(entry.value.data(), 0), pointer.parent_ino);
  CHECK_EQ(GetBE32(entry.value.data(), kParentGenOffset), pointer.parent_gen);

  auto decoded = DecodeParentPointer(entry, geo, 67);
  CHECK(decoded.ok());
  CHECK(decoded.value() == pointer);
  CHECK_EQ(ParentPointerHash("same", 0x0000000100000002ULL),
           XfsDirNameHash("same") ^ 1u ^ 2u);

  CHECK(ValidateParentPointerName("normal").ok());
  CHECK(!ValidateParentPointerName("").ok());
  CHECK(!ValidateParentPointerName(".").ok());
  CHECK(!ValidateParentPointerName("..").ok());
  CHECK(!ValidateParentPointerName("a/b").ok());

  XattrEntry bad_hash = entry;
  bad_hash.hash ^= 1;
  CHECK(!DecodeParentPointer(bad_hash, geo, 67).ok());

  XattrEntry bad_len = entry;
  bad_len.value.resize(8);
  CHECK(!DecodeParentPointer(bad_len, geo, 67).ok());

  XattrEntry self = MakeParentPointerEntry({67, 1, "self"});
  CHECK(!DecodeParentPointer(self, geo, 67).ok());

  ParentPointer high{geo.AginoToIno(2, 16), 7, "high"};
  XattrEntry high_entry = MakeParentPointerEntry(high);
  CHECK(DecodeParentPointer(high_entry, geo, 67).ok());
  // The hash mixes both halves of the inode number.
  CHECK_EQ(ParentPointerHash("high", 0x0000000100000002ULL),
           XfsDirNameHash("high") ^ 1u ^ 2u);
}

TEST(rw6e_create_mkdir_symlink_have_parents) {
  Image image = MakeImage(true);
  const uint64_t root = image.fs->RootInode();
  CHECK(Parents(image, root).empty());

  const uint64_t file = Create(image, root, "file");
  auto p = Parents(image, file);
  CHECK_EQ(p.size(), 1u);
  CHECK_STR_EQ(p[0].name, "file");
  CHECK_EQ(p[0].parent_ino, root);

  // The private namespace is not a user xattr mutation path.
  auto private_attrs = image.fs->ListXattrs(file, true);
  CHECK(private_attrs.ok());
  CHECK(private_attrs.value().size() == 1);
  CHECK(private_attrs.value()[0].Namespace().IsPrivate());
  CHECK(!image.fs->SetXattr(file, "parent.evil", "x", 1,
                            XfsFilesystem::XattrSetMode::kUpsert)
             .ok());
  CHECK(!image.fs->RemoveXattr(file, "parent.file").ok());
  auto raw_parent = image.fs->GetXattr(file, "parent.file", true);
  CHECK(raw_parent.ok());
  CHECK_EQ(raw_parent.value().value.size(), kParentRecSize);
  CHECK(!image.fs->GetXattr(file, "parent.file", false).ok());
  CHECK_EQ(Parents(image, file).size(), 1u);

  const uint64_t dir = Mkdir(image, root, "dir");
  p = Parents(image, dir);
  CHECK_EQ(p.size(), 1u);
  CHECK_STR_EQ(p[0].name, "dir");
  CHECK_EQ(p[0].parent_ino, root);

  auto link = image.fs->CreateSymlink(root, "sym", "target", 0777, 0, 0);
  CHECK(link.ok());
  p = Parents(image, link.value());
  CHECK_EQ(p.size(), 1u);
  CHECK_STR_EQ(p[0].name, "sym");

  Finish(image);
}

TEST(rw6e_hardlink_unlink_rename_and_replace_lifecycle) {
  Image image = MakeImage(true);
  const uint64_t root = image.fs->RootInode();
  const uint64_t d1 = Mkdir(image, root, "d1");
  const uint64_t d2 = Mkdir(image, root, "d2");
  const uint64_t file = Create(image, root, "file");

  Link(image, file, d1, "same");
  Link(image, file, d2, "same");
  Link(image, file, root, "other");
  CHECK_EQ(image.fs->GetFileInfo(file).value().nlink, 4u);
  auto p = Parents(image, file);
  CHECK_EQ(p.size(), 4u);
  // Two identical names under different parents are distinct keys, not a
  // duplicate and not a replace.
  CHECK(std::count_if(p.begin(), p.end(), [](const ParentPointer& x) {
          return x.name == "same";
        }) == 2);

  // Same-directory rename: one exact record is replaced.
  auto renamed = image.fs->Rename(d1, "same", d1, "renamed", false);
  CHECK(renamed.ok());
  p = Parents(image, file);
  CHECK_EQ(p.size(), 4u);
  CHECK(std::none_of(p.begin(), p.end(), [&](const ParentPointer& x) {
    return x.parent_ino == d1 && x.name == "same";
  }));

  // Cross-directory rename: parent and name both change.
  renamed = image.fs->Rename(d1, "renamed", d2, "moved", false);
  CHECK(renamed.ok());
  p = Parents(image, file);
  CHECK(std::any_of(p.begin(), p.end(), [&](const ParentPointer& x) {
    return x.parent_ino == d2 && x.name == "moved";
  }));

  // Unlink one hard link: only that exact pointer disappears.
  CHECK(image.fs->Unlink(d2, "same").ok());
  p = Parents(image, file);
  CHECK_EQ(p.size(), 3u);

  // Rename replace: the target loses its own pointer and nlink, the moved
  // inode gains the target name.
  const uint64_t target = Create(image, d2, "victim");
  renamed = image.fs->Rename(d2, "moved", d2, "victim", true);
  CHECK(renamed.ok());
  CHECK_EQ(renamed.value().replaced_ino, target);
  CHECK(!image.fs->OpenInode(target).ok());
  p = Parents(image, file);
  CHECK(std::any_of(p.begin(), p.end(), [&](const ParentPointer& x) {
    return x.parent_ino == d2 && x.name == "victim";
  }));
  CHECK(std::none_of(p.begin(), p.end(), [&](const ParentPointer& x) {
    return x.name == "moved";
  }));

  Finish(image);
}

TEST(rw6e_directory_move_updates_dotdot_and_parent) {
  Image image = MakeImage(true);
  const uint64_t root = image.fs->RootInode();
  const uint64_t a = Mkdir(image, root, "a");
  const uint64_t b = Mkdir(image, root, "b");
  const uint64_t child = Mkdir(image, a, "child");
  Create(image, child, "inside");

  auto moved = image.fs->Rename(a, "child", b, "child2", false);
  CHECK(moved.ok());
  auto p = Parents(image, child);
  CHECK_EQ(p.size(), 1u);
  CHECK_EQ(p[0].parent_ino, b);
  CHECK_STR_EQ(p[0].name, "child2");
  auto dotdot = image.fs->LookupName(child, "..");
  CHECK(dotdot.ok());
  CHECK_EQ(dotdot.value().ino, b);
  CHECK_EQ(image.fs->GetFileInfo(a).value().nlink, 2u);
  CHECK_EQ(image.fs->GetFileInfo(b).value().nlink, 3u);
  Finish(image);

  // Replace an empty target directory: both PPs are updated and the old
  // target's ".." is no longer part of the namespace.
  const uint64_t c = Mkdir(image, root, "c");
  const uint64_t target = Mkdir(image, c, "target");
  moved = image.fs->Rename(b, "child2", c, "target", true);
  CHECK(moved.ok());
  p = Parents(image, child);
  CHECK_EQ(p.size(), 1u);
  CHECK_EQ(p[0].parent_ino, c);
  CHECK_STR_EQ(p[0].name, "target");
  CHECK(!image.fs->OpenInode(target).ok());
  Finish(image);
}

TEST(rw6e_same_inode_rename_is_a_noop) {
  Image image = MakeImage(true);
  const uint64_t root = image.fs->RootInode();
  const uint64_t file = Create(image, root, "a");
  Link(image, file, root, "b");
  const auto before = Parents(image, file);
  auto renamed = image.fs->Rename(root, "a", root, "b", true);
  CHECK(renamed.ok());
  const auto after = Parents(image, file);
  CHECK(before == after);
  CHECK_EQ(image.fs->GetFileInfo(file).value().nlink, 2u);
  Finish(image);
}

TEST(rw6e_parent_tree_growth_and_shrink) {
  // 600 hard links force shortform -> leaf -> node; unlinking them must walk
  // back through the same representations.  The exact transition points are
  // format-dependent, so the test asserts the shape is legal at each stage.
  Image image = MakeImage(true, 400ull << 20, 1024);
  const uint64_t root = image.fs->RootInode();
  const uint64_t file = Create(image, root, "bulk");
  std::vector<std::string> names;
  for (int i = 0; i < 600; ++i) {
    char name[16];
    std::snprintf(name, sizeof(name), "l%04d", i);
    names.emplace_back(name);
    Link(image, file, root, names.back());
  }
  auto inode = image.fs->OpenInode(file);
  CHECK(inode.ok());
  auto fork = ReadXattrFork(*image.device, image.geo, inode.value());
  CHECK(fork.ok());
  CHECK(fork.value().format == XattrForkFormat::kNode);
  // 600 parent pointers need more DA blocks than fit in the 512-byte inode
  // literal area, so the attr fork extent map must have reached the BMBT.
  CHECK(inode.value().aformat == ondisk::inode::kFormatBtree);
  CHECK_EQ(image.fs->GetFileInfo(file).value().nlink, 601u);
  CHECK(image.fs->VerifyParentPointers().value().clean());

  for (size_t i = 600; i > 0; --i) {
    CHECK(image.fs->Unlink(root, names[i - 1]).ok());
    if (i == 500 || i == 100 || i == 20 || i == 2) {
      auto fresh = image.fs->OpenInode(file);
      CHECK(fresh.ok());
      auto f = ReadXattrFork(*image.device, image.geo, fresh.value());
      CHECK(f.ok());
      CHECK(f.value().format == XattrForkFormat::kShortform ||
            f.value().format == XattrForkFormat::kLeaf ||
            f.value().format == XattrForkFormat::kNode);
      CHECK(image.fs->VerifyParentPointers().value().clean());
    }
  }
  CHECK_EQ(image.fs->GetFileInfo(file).value().nlink, 1u);
  auto one = Parents(image, file);
  CHECK_EQ(one.size(), 1u);
  CHECK_STR_EQ(one[0].name, "bulk");
  Finish(image);
}

TEST(rw6e_parent_and_ordinary_xattrs_coexist) {
  Image image = MakeImage(true);
  const uint64_t root = image.fs->RootInode();
  const uint64_t d1 = Mkdir(image, root, "d1");
  const uint64_t d2 = Mkdir(image, root, "d2");
  const uint64_t file = Create(image, d1, "f");

  std::vector<uint8_t> remote(3000);
  for (size_t i = 0; i < remote.size(); ++i) {
    remote[i] = static_cast<uint8_t>(i * 31 + 7);
  }
  CHECK(image.fs->SetXattr(file, "user.small", "x", 1,
                           XfsFilesystem::XattrSetMode::kUpsert)
            .ok());
  CHECK(image.fs->SetXattr(file, "user.remote", remote.data(), remote.size(),
                           XfsFilesystem::XattrSetMode::kUpsert)
            .ok());
  CHECK(image.fs->SetXattr(file, "trusted.trusted", "t", 1,
                           XfsFilesystem::XattrSetMode::kUpsert)
            .ok());
  // Same name and hash in another namespace is a different attribute.
  CHECK(image.fs->SetXattr(file, "trusted.small", "ts", 2,
                           XfsFilesystem::XattrSetMode::kUpsert)
            .ok());
  Link(image, file, d2, "alias");
  CHECK(image.fs->Rename(d1, "f", d2, "renamed", false).ok());
  auto parents = Parents(image, file);
  CHECK_EQ(parents.size(), 2u);
  auto stored = image.fs->GetXattr(file, "user.remote");
  CHECK(stored.ok());
  CHECK(stored.value().value == remote);
  auto small = image.fs->GetXattr(file, "user.small");
  CHECK(small.ok());
  CHECK_STR_EQ(std::string(small.value().value.begin(),
                           small.value().value.end()),
               "x");

  // Ordinary xattr removal must not disturb either parent pointer.
  CHECK(image.fs->RemoveXattr(file, "user.small").ok());
  parents = Parents(image, file);
  CHECK_EQ(parents.size(), 2u);
  Finish(image);
}

TEST(rw6e_parent_intent_parser_set_remove_replace) {
  Image image = MakeImage(true, 400ull << 20);
  const XfsGeometry& geo = image.geo;
  const uint64_t root = image.fs->RootInode();
  const uint64_t file = Create(image, root, "f");
  auto file_inode = image.fs->OpenInode(file);
  CHECK(file_inode.ok());
  const uint32_t gen = file_inode.value().gen;

  auto make_format = [&](uint32_t op, uint16_t old_len, uint16_t new_len) {
    std::vector<uint8_t> format(40, 0);
    PutLE16At(format.data(), 0, 0x1246);
    PutLE32At(format.data(), 4, gen);
    PutLE64At(format.data(), 8, 0x1234);
    PutLE64At(format.data(), 16, file);
    PutLE32At(format.data(), 24, op);
    if (op == static_cast<uint32_t>(XattrIntentOp::kParentReplace)) {
      PutLE16At(format.data(), 28, old_len);
      PutLE16At(format.data(), 30, new_len);
    } else {
      PutLE32At(format.data(), 28, old_len);
    }
    PutLE32At(format.data(), 32, kParentRecSize);
    PutLE32At(format.data(), 36, kAttrParent);
    return format;
  };
  auto make_region = [](const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> region((bytes.size() + 3) & ~size_t{3}, 0);
    std::memcpy(region.data(), bytes.data(), bytes.size());
    return region;
  };
  ParentPointer old_pointer{root, image.fs->OpenInode(root).value().gen, "f"};
  ParentPointer new_pointer{root, old_pointer.parent_gen, "g"};
  std::vector<uint8_t> old_value = EncodeParentPointerValue(old_pointer);
  std::vector<uint8_t> new_value = EncodeParentPointerValue(new_pointer);

  {
    auto format = make_format(
        static_cast<uint32_t>(XattrIntentOp::kParentSet), 1, 0);
    PutLE16At(format.data(), 2, 3);
    std::vector<std::vector<uint8_t>> regions;
    regions.push_back(std::move(format));
    regions.push_back(make_region({'f'}));
    regions.push_back(make_region(old_value));
    auto parsed = ParseXattrIntent(regions, geo);
    CHECK(parsed.ok());
    CHECK(parsed.value().op == XattrIntentOp::kParentSet);
    CHECK_EQ(parsed.value().igen, gen);
    CHECK_STR_EQ(parsed.value().name, "f");
    CHECK(parsed.value().value == old_value);
  }
  {
    auto format = make_format(
        static_cast<uint32_t>(XattrIntentOp::kParentReplace), 1, 1);
    PutLE16At(format.data(), 2, 5);
    std::vector<std::vector<uint8_t>> regions;
    regions.push_back(std::move(format));
    regions.push_back(make_region({'f'}));
    regions.push_back(make_region({'g'}));
    regions.push_back(make_region(old_value));
    regions.push_back(make_region(new_value));
    auto parsed = ParseXattrIntent(regions, geo);
    CHECK(parsed.ok());
    CHECK(parsed.value().op == XattrIntentOp::kParentReplace);
    CHECK_STR_EQ(parsed.value().old_name, "f");
    CHECK_STR_EQ(parsed.value().new_name, "g");
    CHECK(parsed.value().value == old_value);
    CHECK(parsed.value().new_value == new_value);
  }
  {
    auto format = make_format(
        static_cast<uint32_t>(XattrIntentOp::kParentRemove), 1, 0);
    PutLE16At(format.data(), 2, 3);
    std::vector<std::vector<uint8_t>> regions;
    regions.push_back(std::move(format));
    regions.push_back(make_region({'f'}));
    regions.push_back(make_region(old_value));
    auto parsed = ParseXattrIntent(regions, geo);
    CHECK(parsed.ok());
    CHECK(parsed.value().op == XattrIntentOp::kParentRemove);
  }
  // Generated by recovery: a valid parent log item must survive the ATTRI
  // region parser and carry the child generation, not the parent's.
  Finish(image);
}

TEST(rw6e_check_detects_missing_and_stale_parent_records) {
  {
    Image image = MakeImage(true);
    const uint64_t root = image.fs->RootInode();
    const uint64_t file = Create(image, root, "missing");
    auto raw = std::make_unique<std::vector<uint8_t>>(image.geo.inode_size);
    const uint64_t offset = image.geo.InoToByteOffset(file);
    CHECK(image.device->Read(offset, raw->data(), raw->size()));
    // Drop the attr fork entirely, as if a buggy writer forgot the reverse
    // record.  The namespace still contains the dirent.
    (*raw)[ondisk::inode::kForkoff] = 0;
    (*raw)[ondisk::inode::kAformat] = ondisk::inode::kFormatExtents;
    SetInodeAttrExtentCount(raw->data(), 0);
    UpdateXfsChecksum(raw->data(), raw->size(), ondisk::inode::kCrc);
    CHECK(image.device->Write(offset, raw->data(), raw->size()));
    auto audit = image.fs->VerifyParentPointers();
    CHECK(audit.ok());
    CHECK(!audit.value().clean());
    CHECK(!audit.value().missing.empty());
    CheckReport report = image.fs->CheckConsistency(CheckOptions{});
    CHECK(report.Overall() == CheckStatus::kCorrupt);
  }
  {
    Image image = MakeImage(true);
    const uint64_t root = image.fs->RootInode();
    const uint64_t file = Create(image, root, "stale");
    std::vector<uint8_t> raw(image.geo.inode_size);
    const uint64_t offset = image.geo.InoToByteOffset(file);
    CHECK(image.device->Read(offset, raw.data(), raw.size()));
    const uint8_t forkoff = raw[ondisk::inode::kForkoff];
    CHECK(forkoff != 0);
    const size_t attr = ondisk::inode::kCoreSizeV3 +
                        static_cast<size_t>(forkoff) * 8;
    const size_t entry = attr + 4;  // xfs_attr_sf_hdr
    CHECK_EQ(raw[entry + 2], kAttrParent);
    const uint8_t namelen = raw[entry];
    const size_t value = entry + 3 + namelen;
    const uint32_t gen = GetBE32(raw.data() + value + kParentGenOffset);
    PutBE32(raw.data(), value + kParentGenOffset, gen + 1);
    UpdateXfsChecksum(raw.data(), raw.size(), ondisk::inode::kCrc);
    CHECK(image.device->Write(offset, raw.data(), raw.size()));
    auto audit = image.fs->VerifyParentPointers();
    CHECK(audit.ok());
    CHECK(!audit.value().clean());
    CHECK(!audit.value().wrong_parent_generation.empty() ||
          !audit.value().stale_generation.empty());
    CheckReport report = image.fs->CheckConsistency(CheckOptions{});
    CHECK(report.Overall() == CheckStatus::kCorrupt);
  }
}

TEST(rw6e_parent_generation_survives_inode_reuse) {
  Image image = MakeImage(true);
  const uint64_t root = image.fs->RootInode();
  const uint64_t p = Mkdir(image, root, "p");
  auto old_inode = image.fs->OpenInode(p);
  CHECK(old_inode.ok());
  const uint32_t old_gen = old_inode.value().gen;
  const uint64_t f = Create(image, p, "f");
  auto child_before = Parents(image, f);
  CHECK_EQ(child_before.size(), 1u);
  CHECK_EQ(child_before[0].parent_ino, p);
  CHECK_EQ(child_before[0].parent_gen, old_gen);

  CHECK(image.fs->Unlink(p, "f").ok());
  CHECK(image.fs->RemoveEmptyDirectory(root, "p").ok());
  // The freed inode number is reused for the next directory, with a new
  // generation.  The new child's pointer must use the new generation.
  const uint64_t p2 = Mkdir(image, root, "p2");
  auto new_inode = image.fs->OpenInode(p2);
  CHECK(new_inode.ok());
  const uint64_t g = Create(image, p2, "g");
  auto child_after = Parents(image, g);
  CHECK_EQ(child_after.size(), 1u);
  CHECK_EQ(child_after[0].parent_ino, p2);
  CHECK_EQ(child_after[0].parent_gen, new_inode.value().gen);
  Finish(image);
}

TEST(rw6e_recovery_completes_a_parent_pointer_intent) {
  Image image = MakeImage(true);
  const uint64_t root = image.fs->RootInode();
  const uint64_t file = Create(image, root, "f");
  auto inode = image.fs->OpenInode(file);
  CHECK(inode.ok());
  auto before = Parents(image, file);
  CHECK_EQ(before.size(), 1u);
  const uint32_t child_gen = inode.value().gen;
  CHECK(image.fs->WriteCleanUnmountRecord().ok());
  CHECK(image.device->Flush());

  auto sb = ReadSuperblock(*image.device, 0, true);
  CHECK(sb.ok());
  const XfsGeometry geo = MakeGeometry(sb.value());
  auto position = ReadLogPosition(*image.device, geo, sb.value());
  CHECK(position.ok());
  if (!position.ok()) return;

  // One committed ATTRI PPTR_REMOVE with no matching ATTRD: exactly the
  // surviving deferred state a crash after the namespace transaction leaves.
  std::vector<uint8_t> attri(40, 0);
  PutLE16At(attri.data(), 0, logfmt::kItemAttri);
  PutLE16At(attri.data(), 2, 3);
  PutLE32At(attri.data(), 4, child_gen);
  PutLE64At(attri.data(), 8, 0x600d);
  PutLE64At(attri.data(), 16, file);
  PutLE32At(attri.data(), 24,
            static_cast<uint32_t>(XattrIntentOp::kParentRemove));
  PutLE32At(attri.data(), 28, 1);
  PutLE32At(attri.data(), 32, kParentRecSize);
  PutLE32At(attri.data(), 36, kAttrParent);

  LogRecordParams params;
  params.cycle = position.value().cycle;
  params.block = position.value().next_block;
  params.tid = 0x600d;
  params.prev_block = position.value().prev_block;
  params.tail_lsn =
      MakeLsn(position.value().cycle, position.value().next_block);
  params.log_blocks = position.value().log_blocks;
  params.record_size = position.value().record_size;
  params.uuid = geo.meta_uuid;
  auto record = BuildLogRecord(
      params, CommitOps(0x600d,
                        {attri, PaddedRegion({'f'}),
                         PaddedRegion(before[0].name == "f"
                                          ? EncodeParentPointerValue(before[0])
                                          : std::vector<uint8_t>())}));
  CHECK(record.ok());
  if (!record.ok()) return;
  CHECK(image.device->Write(
      geo.FsbToByte(sb.value().logstart) +
          static_cast<uint64_t>(params.block) * 512,
      record.value().data(), record.value().size()));
  CHECK(image.device->Flush());

  MountOptions recovery;
  recovery.read_write = true;
  recovery.recovery_only = true;
  recovery.clean_log_after_recovery = true;
  recovery.allow_parent_qualification = true;
  image.fs.reset();
  auto mounted = XfsFilesystem::Mount(
      std::make_unique<xfstest::BorrowedBlockDevice>(image.device.get()),
      recovery);
  CHECK(mounted.ok());
  if (!mounted.ok()) return;
  CHECK(mounted.value()->recovery_report().has_value());
  auto recovered = mounted.value()->OpenInode(file);
  CHECK(recovered.ok());
  auto after = ListParents(*image.device, geo, recovered.value());
  CHECK(after.ok());
  CHECK(after.value().empty());
}

TEST(rw6e_policy_gate_and_formatter_profile) {
  Image image = MakeImage(true);
  auto sb = ReadSuperblock(*image.device, 0, true);
  CHECK(sb.ok());
  CHECK(sb.value().features.parent);
  CHECK(sb.value().features.exchrange);
  // Linux xfs_scrub treats an attr fork on a filesystem without ATTRBIT as
  // corruption ("attr block map: Repairs are required"), even though
  // xfs_repair accepts it.  The formatter must set the bit for its attr2
  // profile before any parent pointer or user xattr is written.
  CHECK((sb.value().versionnum & ondisk::kSbVersionAttrBit) != 0);
  XfsGeometry geo = MakeGeometry(sb.value());
  // Parent pointers and exchange-range are part of the ordinary writable
  // profile now; the legacy qualification flag remains a no-op.
  Status ordinary =
      CheckWritableProfile(sb.value(), geo, false, false, false, false, false);
  CHECK(ordinary.ok());
  Status qualification =
      CheckWritableProfile(sb.value(), geo, false, false, false, false, true);
  CHECK(qualification.ok());
  // A parent profile still cannot smuggle in metadir.
  auto metadir_sb = sb.value();
  metadir_sb.features_incompat |= ondisk::kIncompatMetadir;
  CHECK(!CheckWritableProfile(metadir_sb, geo).ok());
  auto summary = WritableProfileSummary(sb.value());
  CHECK(summary.find("parent-pointers") != std::string::npos);
  Finish(image);
}
