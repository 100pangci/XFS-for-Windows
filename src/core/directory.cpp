// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/directory.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "xfs/endian.h"
#include "xfs/extent.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/verifier.h"

namespace xfs {
namespace {

namespace d = ondisk::dir;
namespace di = ondisk::inode;

constexpr uint32_t kNullDataPtr = 0;  // XFS_DIR2_NULL_DATAPTR
constexpr uint32_t kNameMaxLen = 255;

uint32_t Rol32(uint32_t v, int bits) {
  bits &= 31;
  return (v << bits) | (v >> (32 - bits));
}

// ---------------------------------------------------------------------------
// Block I/O helpers
// ---------------------------------------------------------------------------

class DirIo {
 public:
  DirIo(BlockDevice& dev, const XfsGeometry& geo, const XfsInode& inode)
      : dev_(dev), geo_(geo), inode_(inode), dir_blk_size_(geo.dir_block_size) {}

  uint32_t DirBlockSize() const { return dir_blk_size_; }
  uint32_t DirBlockLog() const { return geo_.block_log + geo_.dir_blk_log; }
  uint64_t LeafOffsetBytes() const { return d::kLeafOffset; }
  uint64_t FreeOffsetBytes() const { return d::kFreeOffset; }

  // Reads a directory block (dir_blk_size bytes) at a directory file byte
  // offset, verifies its V5 header for the given expected magic, and returns
  // the buffer plus the physical byte offset of its first fs block.
  Result<std::vector<uint8_t>> ReadDirBlock(uint64_t file_byte_offset,
                                            uint32_t expected_magic,
                                            const char* what) {
    uint64_t block_addr = 0;
    Status s = CheckMapped(file_byte_offset, &block_addr);
    if (!s.ok()) return s.error();
    std::vector<uint8_t> buf(dir_blk_size_);
    // Read through the extent mapping directly. Directory leaf/free blocks
    // live at huge logical offsets (32/64 GiB) beyond inode.size, which the
    // size-clamped ReadFileData would reject.
    s = ReadMappedRange(file_byte_offset, buf.data(), buf.size());
    if (!s.ok()) return s.error();

    // Identity: magic/crc/uuid/blkno/lsn/owner differ per block type.
    V5MetaCheck check;
    check.type = what;
    if (expected_magic == ondisk::kDir2BlockMagic ||
        expected_magic == ondisk::kDir3BlockMagic ||
        expected_magic == ondisk::kDir2DataMagic ||
        expected_magic == ondisk::kDir3DataMagic ||
        expected_magic == ondisk::kDir2FreeMagic ||
        expected_magic == ondisk::kDir3FreeMagic) {
      // xfs_dir3_blk_hdr layout.
      check.magic_offset = 0;
      check.crc_offset = d::kData3Crc;
      check.blkno_offset = 8;
      check.lsn_offset = 16;
      check.uuid_offset = 24;
      check.owner_offset = 40;
      check.owner_size = 8;
    } else {
      // xfs_da3_blkinfo layout (leaf / node blocks). The da magic is a
      // 16-bit field at offset 8.
      check.magic_offset = 8;
      check.magic_size = 2;
      check.crc_offset = d::kLeaf3Crc;
      check.blkno_offset = 16;
      check.lsn_offset = 24;
      check.uuid_offset = 32;
      check.owner_offset = 48;
      check.owner_size = 8;
    }
    check.magic = expected_magic;
    s = VerifyV5Metadata(buf.data(), buf.size(), check, block_addr / 512,
                         inode_.ino, geo_.meta_uuid, inode_.lsn);
    if (!s.ok()) return s.error();
    return buf;
  }

  // Verifies the byte range is backed by a mapped, written extent.
  Status CheckMapped(uint64_t file_byte_offset, uint64_t* dev_byte_offset) {
    auto m = MapFileOffset(dev_, geo_, inode_, file_byte_offset);
    if (!m.ok()) return m.error();
    const XfsFileMapping& mp = m.value();
    if (mp.is_hole || mp.is_unwritten) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     mp.is_hole ? "directory block is a hole"
                                : "directory block is unwritten")
          .Ino(inode_.ino)
          .Offset(file_byte_offset);
    }
    uint64_t block_mapped_bytes = mp.block_count * geo_.block_size;
    uint64_t extent_start_byte = mp.block_offset * geo_.block_size;
    uint64_t extent_end_byte = extent_start_byte + block_mapped_bytes;
    if (file_byte_offset + dir_blk_size_ > extent_end_byte) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "directory block spans an extent boundary")
          .Ino(inode_.ino)
          .Offset(file_byte_offset);
    }
    if (dev_byte_offset) {
      *dev_byte_offset = geo_.FsbToByte(mp.fs_block);
    }
    return OkStatus();
  }

  // Reads len bytes at a directory file offset through the extent mapping.
  // Unlike ReadFileData this does not clamp to inode.size, because directory
  // leaf/free blocks live at 32/64 GiB logical offsets.
  Status ReadMappedRange(uint64_t file_byte_offset, uint8_t* out, size_t len) {
    auto m = MapFileOffset(dev_, geo_, inode_, file_byte_offset);
    if (!m.ok()) return m.error();
    const XfsFileMapping& mp = m.value();
    if (mp.is_hole || mp.is_unwritten) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     mp.is_hole ? "directory block is a hole"
                                : "directory block is unwritten")
          .Ino(inode_.ino)
          .Offset(file_byte_offset);
    }
    uint64_t dev_off = geo_.FsbToByte(mp.fs_block) +
                       (file_byte_offset & (geo_.block_size - 1));
    if (!dev_.Read(dev_off, out, len)) {
      return XFS_ERR(XfsError::kIoError, "failed to read directory block")
          .Ino(inode_.ino)
          .Offset(dev_off);
    }
    return OkStatus();
  }

  const XfsInode& inode() const { return inode_; }
  bool HasFtype() const { return geo_.features.ftype; }

 private:
  BlockDevice& dev_;
  const XfsGeometry& geo_;
  const XfsInode& inode_;
  uint32_t dir_blk_size_;
};

// ---------------------------------------------------------------------------
// Entry-level parsing
// ---------------------------------------------------------------------------

// Validates a name found on disk.
Status ValidateName(const uint8_t* name, size_t len, uint64_t ino,
                    uint64_t dir_byte_offset) {
  if (len == 0 || len > kNameMaxLen) {
    return XFS_ERR(XfsError::kCorruptDirectory, "invalid name length")
        .Ino(ino)
        .Offset(dir_byte_offset)
        .Actual(len);
  }
  if (std::memchr(name, '\0', len) != nullptr ||
      std::memchr(name, '/', len) != nullptr) {
    return XFS_ERR(XfsError::kCorruptDirectory,
                   "directory name contains NUL or '/'")
        .Ino(ino)
        .Offset(dir_byte_offset);
  }
  return OkStatus();
}

// Walks the data-entry chain of a data/block directory block.
// `end` is the offset one past the last data entry.
Status ParseDataEntries(
    const uint8_t* block, uint32_t block_len, uint32_t header_size,
    uint32_t end, bool has_ftype, uint64_t dir_ino, uint64_t dir_byte_offset,
    const std::function<Status(const XfsDirEntry&)>& emit,
    const std::function<bool(uint32_t off, const XfsDirEntry&)>& filter) {
  if (header_size > block_len || end > block_len || end < header_size) {
    return XFS_ERR(XfsError::kCorruptDirectory,
                   "directory block header/end offsets out of range")
        .Ino(dir_ino)
        .Expected(block_len)
        .Actual(end);
  }
  uint32_t off = header_size;
  while (off < end) {
    if (off + 8 > end) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "truncated directory entry area")
          .Ino(dir_ino)
          .Offset(dir_byte_offset + off);
    }
    uint16_t freetag = GetBE16(block + off);
    if (freetag == d::kDataFreeTag) {
      uint16_t len = GetBE16(block + off + 2);
      if (len < 8 || (len & 7) != 0 || off + len > end) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "invalid free space record in directory block")
            .Ino(dir_ino)
            .Offset(dir_byte_offset + off)
            .Actual(len);
      }
      uint16_t tag = GetBE16(block + off + len - 2);
      if (tag != off) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "free space tag mismatch")
            .Ino(dir_ino)
            .Offset(dir_byte_offset + off)
            .Expected(off)
            .Actual(tag);
      }
      off += len;
      continue;
    }

    uint64_t inumber = GetBE64(block + off);
    uint8_t namelen = GetU8(block + off + 8);
    size_t entsize = d::DataEntrySize(namelen, has_ftype);
    if (off + entsize > end) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "directory entry extends past end of area")
          .Ino(dir_ino)
          .Offset(dir_byte_offset + off);
    }
    uint16_t tag = GetBE16(block + off + entsize - 2);
    if (tag != off) {
      return XFS_ERR(XfsError::kCorruptDirectory, "directory entry tag mismatch")
          .Ino(dir_ino)
          .Offset(dir_byte_offset + off)
          .Expected(off)
          .Actual(tag);
    }
    const uint8_t* name = block + off + 9;
    Status s = ValidateName(name, namelen, dir_ino, dir_byte_offset + off);
    if (!s.ok()) return s;

    XfsDirEntry entry;
    entry.name.assign(reinterpret_cast<const char*>(name), namelen);
    entry.ino = inumber;
    if (has_ftype) {
      uint8_t ftype = GetU8(block + off + 9 + namelen);
      if (ftype >= d::kFtMax) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "invalid dirent file type")
            .Ino(dir_ino)
            .Offset(dir_byte_offset + off)
            .Actual(ftype);
      }
      entry.ftype = ftype;
    }
    if (!filter || filter(off, entry)) {
      s = emit(entry);
      if (!s.ok()) return s;
    }
    off += static_cast<uint32_t>(entsize);
  }
  return OkStatus();
}

// Parses the shortform directory inside the inode literal area.
Status ParseShortform(const XfsInode& inode, bool has_ftype,
                      const std::function<Status(const XfsDirEntry&)>& emit) {
  const std::vector<uint8_t>& fork = inode.data_fork;
  if (fork.size() < 6) {
    return XFS_ERR(XfsError::kCorruptDirectory, "shortform dir fork too small")
        .Ino(inode.ino);
  }
  uint8_t count = fork[d::kSfHdrCount];
  uint8_t i8count = fork[d::kSfHdrI8Count];
  size_t hdr_size =
      (i8count == 0) ? 6 : d::kSfHdrSizeFixed;  // 10 with 8-byte inode
  if (fork.size() < hdr_size) {
    return XFS_ERR(XfsError::kCorruptDirectory, "shortform dir header too big")
        .Ino(inode.ino);
  }
  if (i8count > count + 1) {
    // The parent inode may itself require an 8-byte number, so i8count can
    // exceed the number of stored entries by one (empty root directories).
    return XFS_ERR(XfsError::kCorruptDirectory,
                   "shortform i8count exceeds entry count")
        .Ino(inode.ino)
        .Expected(count + 1)
        .Actual(i8count);
  }
  uint64_t parent = 0;
  if (i8count == 0) {
    parent = GetBE32(fork.data() + d::kSfHdrParent);
  } else {
    parent = GetBE64(fork.data() + d::kSfHdrParent);
  }

  // "." and ".." are implicit in shortform directories: "." is the inode
  // itself, ".." is the parent stored in the header. They are not entries.
  XfsDirEntry dot;
  dot.name = ".";
  dot.ino = inode.ino;
  dot.ftype = d::kFtDir;
  Status synthetic = emit(dot);
  if (!synthetic.ok()) return synthetic;
  XfsDirEntry dotdot;
  dotdot.name = "..";
  dotdot.ino = parent;
  dotdot.ftype = d::kFtDir;
  synthetic = emit(dotdot);
  if (!synthetic.ok()) return synthetic;

  size_t off = hdr_size;
  for (uint32_t i = 0; i < count; ++i) {
    if (off + d::kSfEntName > fork.size()) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "shortform entry truncated")
          .Ino(inode.ino)
          .Offset(off);
    }
    uint8_t namelen = fork[off + d::kSfEntNamelen];
    size_t entsize = d::kSfEntName + namelen + (has_ftype ? 1u : 0u) +
                     (i8count == 0 ? 4u : 8u);
    if (off + entsize > fork.size()) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "shortform entry extends past fork")
          .Ino(inode.ino)
          .Offset(off);
    }
    const uint8_t* name = fork.data() + off + d::kSfEntName;
    Status s = ValidateName(name, namelen, inode.ino, off);
    if (!s.ok()) return s;
    XfsDirEntry e;
    e.name.assign(reinterpret_cast<const char*>(name), namelen);
    size_t inum_off = off + d::kSfEntName + namelen + (has_ftype ? 1u : 0u);
    if (has_ftype) {
      uint8_t ftype = fork[off + d::kSfEntName + namelen];
      if (ftype >= d::kFtMax) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "invalid shortform dirent file type")
            .Ino(inode.ino)
            .Offset(off)
            .Actual(ftype);
      }
      e.ftype = ftype;
    }
    e.ino = (i8count == 0) ? GetBE32(fork.data() + inum_off)
                           : GetBE64(fork.data() + inum_off);
    if (e.ino == 0) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "shortform entry has zero inode number")
          .Ino(inode.ino)
          .Offset(off);
    }
    if (i8count > 0 && e.ino <= 0xffffffffu) {
      // i8count is hit on every entry when any inode number needs 8 bytes.
      // A 32-bit-sized number in an i8count directory is legal.
    }
    s = emit(e);
    if (!s.ok()) return s;
    off += entsize;
  }
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Leaf / node handling
// ---------------------------------------------------------------------------

struct LeafEntry {
  uint32_t hashval;
  uint32_t address;
};

struct LeafView {
  std::vector<uint8_t> buf;  // block image
  const LeafEntry* ents = nullptr;
  uint16_t count = 0;
  uint16_t stale = 0;
  uint32_t entries_offset = 0;
  // LEAF1 only:
  const uint8_t* bests = nullptr;
  uint32_t bestcount = 0;
  uint32_t tail_offset = 0;
  bool is_leaf1 = false;
  bool has_forw_back = false;
  uint32_t forw = 0;
  uint32_t back = 0;
};

Result<LeafView> ParseLeafBlock(std::vector<uint8_t>&& buf) {
  LeafView v;
  v.buf = std::move(buf);
  const uint8_t* p = v.buf.data();
  uint32_t len = static_cast<uint32_t>(v.buf.size());
  uint16_t magic = GetBE16(p, 8);
  v.forw = GetBE32(p, 0);
  v.back = GetBE32(p, 4);
  v.has_forw_back = true;
  if (magic == ondisk::kDir3Leaf1Magic || magic == ondisk::kDir2Leaf1Magic) {
    v.is_leaf1 = true;
  } else if (magic == ondisk::kDir3LeafnMagic ||
             magic == ondisk::kDir2LeafnMagic) {
    v.is_leaf1 = false;
  } else {
    return XFS_ERR(XfsError::kCorruptDirectory, "bad directory leaf magic")
        .Actual(magic);
  }
  v.count = GetBE16(p, d::kLeafHdrCount);
  v.stale = GetBE16(p, d::kLeafHdrStale);
  v.entries_offset = d::kLeaf3HeaderSize;
  if (v.entries_offset + uint64_t(v.count) * d::kLeafEntSize > len) {
    return XFS_ERR(XfsError::kCorruptDirectory,
                   "directory leaf entry count out of range")
        .Expected((len - v.entries_offset) / d::kLeafEntSize)
        .Actual(v.count);
  }
  v.ents = reinterpret_cast<const LeafEntry*>(p + v.entries_offset);
  if (v.is_leaf1) {
    if (len < 4) {
      return XFS_ERR(XfsError::kCorruptDirectory, "leaf1 block too small");
    }
    v.tail_offset = len - 4;
    v.bestcount = GetBE32(p, v.tail_offset);
    if (v.bestcount > v.tail_offset / 2) {
      return XFS_ERR(XfsError::kCorruptDirectory, "leaf1 bestcount too large")
          .Actual(v.bestcount);
    }
    v.bests = p + v.tail_offset - v.bestcount * 2;
    // bests may not overlap the entries.
    if (v.entries_offset + uint64_t(v.count) * d::kLeafEntSize >
        v.tail_offset - uint64_t(v.bestcount) * 2) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "leaf1 entries overlap bests array");
    }
  }
  return v;
}

// Binary search: first index with hashval >= hash.
uint32_t LeafLowerBound(const LeafView& leaf, uint32_t hash) {
  uint32_t lo = 0, hi = leaf.count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    uint32_t h = LoadBE32(&leaf.ents[mid].hashval);
    if (h < hash) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

uint32_t LeafLastHash(const LeafView& leaf) {
  if (leaf.count == 0) return 0;
  return LoadBE32(&leaf.ents[leaf.count - 1].hashval);
}

struct NodeView {
  uint16_t count = 0;
  uint16_t level = 0;
  std::vector<std::pair<uint32_t, uint32_t>> entries;  // (hashval, before)
};

Result<NodeView> ParseNodeBlock(const std::vector<uint8_t>& buf) {
  NodeView v;
  uint16_t magic = GetBE16(buf.data(), 8);
  if (magic != ondisk::kDaNode3Magic && magic != ondisk::kDaNodeMagic) {
    return XFS_ERR(XfsError::kCorruptDirectory, "bad da node magic")
        .Actual(magic);
  }
  v.count = GetBE16(buf.data(), 56);
  v.level = GetBE16(buf.data(), 58);
  if (v.level == 0 || v.level >= ondisk::kDaNodeMaxDepth) {
    return XFS_ERR(XfsError::kCorruptDirectory, "da node level out of range")
        .Actual(v.level);
  }
  size_t max_entries = (buf.size() - 64) / 8;
  if (v.count == 0 || v.count > max_entries) {
    return XFS_ERR(XfsError::kCorruptDirectory, "da node count out of range")
        .Expected(max_entries)
        .Actual(v.count);
  }
  v.entries.reserve(v.count);
  for (uint16_t i = 0; i < v.count; ++i) {
    uint32_t hash = GetBE32(buf.data(), 64 + i * 8);
    uint32_t before = GetBE32(buf.data(), 64 + i * 8 + 4);
    v.entries.emplace_back(hash, before);
  }
  return v;
}

}  // namespace

uint32_t XfsDirNameHash(const uint8_t* name, size_t len) {
  uint32_t hash = 0;
  size_t i = 0;
  for (; i + 4 <= len; i += 4) {
    hash = (static_cast<uint32_t>(name[i]) << 21) ^
           (static_cast<uint32_t>(name[i + 1]) << 14) ^
           (static_cast<uint32_t>(name[i + 2]) << 7) ^
           static_cast<uint32_t>(name[i + 3]) ^ Rol32(hash, 7 * 4);
  }
  size_t rem = len - i;
  switch (rem) {
    case 3:
      return (static_cast<uint32_t>(name[i]) << 14) ^
             (static_cast<uint32_t>(name[i + 1]) << 7) ^
             static_cast<uint32_t>(name[i + 2]) ^ Rol32(hash, 7 * 3);
    case 2:
      return (static_cast<uint32_t>(name[i]) << 7) ^
             static_cast<uint32_t>(name[i + 1]) ^ Rol32(hash, 7 * 2);
    case 1:
      return static_cast<uint32_t>(name[i]) ^ Rol32(hash, 7);
    default:
      return hash;
  }
}

Result<XfsDirFormat> DetectDirFormat(BlockDevice& dev, const XfsGeometry& geo,
                                     const XfsInode& inode) {
  if (!inode.IsDir()) {
    return XFS_ERR(XfsError::kNotDirectory, "inode is not a directory")
        .Ino(inode.ino);
  }
  if (inode.format == di::kFormatLocal) return XfsDirFormat::kShortform;
  if (inode.size == 0) {
    return XFS_ERR(XfsError::kCorruptDirectory, "directory has zero size")
        .Ino(inode.ino);
  }
  DirIo io(dev, geo, inode);
  auto block0 = io.ReadDirBlock(0, ondisk::kDir3DataMagic, "dir data block 0");
  if (!block0.ok()) {
    // Could be a single-block directory.
    auto blockb = io.ReadDirBlock(0, ondisk::kDir3BlockMagic, "dir block");
    if (!blockb.ok()) return block0.error();
    return XfsDirFormat::kBlock;
  }
  // Data block: root is at the leaf space offset. Leaf1 vs node.
  auto root =
      io.ReadDirBlock(io.LeafOffsetBytes(), ondisk::kDir3Leaf1Magic, "dir root");
  if (root.ok()) return XfsDirFormat::kLeaf;
  auto rootn = io.ReadDirBlock(io.LeafOffsetBytes(), ondisk::kDaNode3Magic,
                               "dir root node");
  if (rootn.ok()) return XfsDirFormat::kNode;
  return root.error();
}

namespace {

// Enumerates entries by walking leaf entries in order (leaf / node formats).
Status EnumerateViaLeaf(DirIo& io, bool has_ftype,
                        const std::function<Status(const XfsDirEntry&)>& emit) {
  const XfsInode& inode = io.inode();
  // Find the leftmost leaf.
  std::vector<uint8_t> node_buf;
  uint64_t file_off = io.LeafOffsetBytes();
  auto root = io.ReadDirBlock(file_off, ondisk::kDaNode3Magic, "dir node");
  if (root.ok()) {
    node_buf = std::move(root.value());
    // Descend entry[0].before until level 0 (leaf).
    for (int depth = 0; depth < ondisk::kDaNodeMaxDepth + 2; ++depth) {
      auto node = ParseNodeBlock(node_buf);
      if (!node.ok()) return node.error();
      uint32_t before = node.value().entries[0].second;
      uint64_t next_off = static_cast<uint64_t>(before) << io.DirBlockLog();
      auto blk = io.ReadDirBlock(next_off, ondisk::kDir3LeafnMagic, "dir leaf");
      if (blk.ok()) {
        node_buf = std::move(blk.value());
        break;
      }
      // Not a leaf: must be another node.
      auto deeper = io.ReadDirBlock(next_off, ondisk::kDaNode3Magic, "dir node");
      if (!deeper.ok()) return blk.error();
      node_buf = std::move(deeper.value());
      if (depth == ondisk::kDaNodeMaxDepth + 1) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "directory node tree too deep")
            .Ino(inode.ino);
      }
    }
    // node_buf now holds the leftmost leaf block (leafn magic); parse it.
    auto leaf = ParseLeafBlock(std::move(node_buf));
    if (!leaf.ok()) return leaf.error();
    LeafView view = std::move(leaf.value());
    std::vector<uint8_t> data_cache;
    uint32_t cached_db = 0xffffffffu;
    std::unordered_set<uint64_t> visited;
    for (;;) {
      // Iterate leaf entries.
      for (uint16_t i = 0; i < view.count; ++i) {
        uint32_t addr = LoadBE32(&view.ents[i].address);
        if (addr == kNullDataPtr) continue;  // stale
        uint32_t db = addr >> (io.DirBlockLog() - 3);
        uint32_t off = (addr & ((1u << (io.DirBlockLog() - 3)) - 1)) << 3;
        if (db != cached_db) {
          auto blk = io.ReadDirBlock(static_cast<uint64_t>(db)
                                         << io.DirBlockLog(),
                                     ondisk::kDir3DataMagic, "dir data block");
          if (!blk.ok()) return blk.error();
          data_cache = std::move(blk.value());
          cached_db = db;
        }
        const uint8_t* block = data_cache.data();
        uint32_t block_len = static_cast<uint32_t>(data_cache.size());
        if (off + 9 > block_len) {
          return XFS_ERR(XfsError::kCorruptDirectory,
                         "leaf address points outside data block")
              .Ino(inode.ino)
              .Actual(addr);
        }
        uint64_t inumber = GetBE64(block + off);
        uint8_t namelen = GetU8(block + off + 8);
        size_t entsize = d::DataEntrySize(namelen, has_ftype);
        if (off + entsize > block_len) {
          return XFS_ERR(XfsError::kCorruptDirectory,
                         "data entry extends past block")
              .Ino(inode.ino)
              .Offset(off);
        }
        // Must be a real entry, not a free record.
        if (GetBE16(block + off) == d::kDataFreeTag) {
          return XFS_ERR(XfsError::kCorruptDirectory,
                         "leaf points at a free record")
              .Ino(inode.ino)
              .Offset(off);
        }
        if (GetBE16(block + off + entsize - 2) != off) {
          return XFS_ERR(XfsError::kCorruptDirectory,
                         "data entry tag mismatch")
              .Ino(inode.ino)
              .Offset(off)
              .Expected(off)
              .Actual(GetBE16(block + off + entsize - 2));
        }
        const uint8_t* name = block + off + 9;
        Status s = ValidateName(name, namelen, inode.ino, off);
        if (!s.ok()) return s;
        XfsDirEntry e;
        e.name.assign(reinterpret_cast<const char*>(name), namelen);
        e.ino = inumber;
        if (has_ftype) {
          uint8_t ftype = GetU8(block + off + 9 + namelen);
          if (ftype >= d::kFtMax) {
            return XFS_ERR(XfsError::kCorruptDirectory,
                           "invalid dirent file type")
                .Ino(inode.ino)
                .Offset(off)
                .Actual(ftype);
          }
          e.ftype = ftype;
        }
        s = emit(e);
        if (!s.ok()) return s;
      }
      if (view.forw == 0) break;
      if (!visited.insert(view.forw).second) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "directory leaf chain is cyclic")
            .Ino(inode.ino);
      }
      if (visited.size() > (1u << 20)) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "directory leaf chain too long")
            .Ino(inode.ino);
      }
      auto next = io.ReadDirBlock(static_cast<uint64_t>(view.forw)
                                      << io.DirBlockLog(),
                                  ondisk::kDir3LeafnMagic, "dir leaf");
      if (!next.ok()) return next.error();
      auto parsed = ParseLeafBlock(std::move(next.value()));
      if (!parsed.ok()) return parsed.error();
      view = std::move(parsed.value());
      if (!view.has_forw_back) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "leaf chain block missing links")
            .Ino(inode.ino);
      }
    }
    return OkStatus();
  }
  // Single leaf1 block.
  auto root1 = io.ReadDirBlock(file_off, ondisk::kDir3Leaf1Magic, "dir leaf1");
  if (!root1.ok()) return root1.error();
  auto leaf = ParseLeafBlock(std::move(root1.value()));
  if (!leaf.ok()) return leaf.error();
  LeafView view = std::move(leaf.value());
  std::vector<uint8_t> data_cache;
  uint32_t cached_db = 0xffffffffu;
  for (uint16_t i = 0; i < view.count; ++i) {
    uint32_t addr = LoadBE32(&view.ents[i].address);
    if (addr == kNullDataPtr) continue;
    uint32_t db = addr >> (io.DirBlockLog() - 3);
    uint32_t off = (addr & ((1u << (io.DirBlockLog() - 3)) - 1)) << 3;
    if (db != cached_db) {
      auto blk = io.ReadDirBlock(static_cast<uint64_t>(db)
                                     << io.DirBlockLog(),
                                 ondisk::kDir3DataMagic, "dir data block");
      if (!blk.ok()) return blk.error();
      data_cache = std::move(blk.value());
      cached_db = db;
    }
    const uint8_t* block = data_cache.data();
    uint32_t block_len = static_cast<uint32_t>(data_cache.size());
    if (off + 9 > block_len) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "leaf address points outside data block")
          .Ino(inode.ino)
          .Actual(addr);
    }
    uint64_t inumber = GetBE64(block + off);
    uint8_t namelen = GetU8(block + off + 8);
    size_t entsize = d::DataEntrySize(namelen, has_ftype);
    if (off + entsize > block_len ||
        GetBE16(block + off) == d::kDataFreeTag ||
        GetBE16(block + off + entsize - 2) != off) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "invalid data entry referenced by leaf")
          .Ino(inode.ino)
          .Offset(off);
    }
    const uint8_t* name = block + off + 9;
    Status s = ValidateName(name, namelen, inode.ino, off);
    if (!s.ok()) return s;
    XfsDirEntry e;
    e.name.assign(reinterpret_cast<const char*>(name), namelen);
    e.ino = inumber;
    if (has_ftype) {
      uint8_t ftype = GetU8(block + off + 9 + namelen);
      if (ftype >= d::kFtMax) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "invalid dirent file type")
            .Ino(inode.ino)
            .Offset(off)
            .Actual(ftype);
      }
      e.ftype = ftype;
    }
    s = emit(e);
    if (!s.ok()) return s;
  }
  return OkStatus();
}

}  // namespace

Status ListDirectory(BlockDevice& dev, const XfsGeometry& geo,
                     const XfsInode& inode,
                     const std::function<Status(const XfsDirEntry&)>& emit) {
  if (!inode.IsDir()) {
    return XFS_ERR(XfsError::kNotDirectory, "inode is not a directory")
        .Ino(inode.ino);
  }
  bool has_ftype = geo.features.ftype;
  if (inode.format == di::kFormatLocal) {
    return ParseShortform(inode, has_ftype, emit);
  }
  DirIo io(dev, geo, inode);
  auto format = DetectDirFormat(dev, geo, inode);
  if (!format.ok()) return format.error();
  switch (format.value()) {
    case XfsDirFormat::kShortform:
      return ParseShortform(inode, has_ftype, emit);
    case XfsDirFormat::kBlock: {
      auto block = io.ReadDirBlock(0, ondisk::kDir3BlockMagic, "dir block");
      if (!block.ok()) return block.error();
      const std::vector<uint8_t>& buf = block.value();
      uint32_t len = static_cast<uint32_t>(buf.size());
      uint32_t count = GetBE32(buf.data(), len - 8);
      uint32_t stale = GetBE32(buf.data(), len - 4);
      (void)stale;
      size_t leaf_entries_start = len - 8 - uint64_t(count) * 8;
      if (count > (len - 8 - d::kBlock3HeaderSize) / 8 ||
          leaf_entries_start < d::kBlock3HeaderSize) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "block directory leaf count out of range")
            .Ino(inode.ino)
            .Actual(count);
      }
      return ParseDataEntries(buf.data(), len, d::kBlock3HeaderSize,
                              static_cast<uint32_t>(leaf_entries_start),
                              has_ftype, inode.ino, 0, emit, nullptr);
    }
    case XfsDirFormat::kLeaf: {
      auto root = io.ReadDirBlock(io.LeafOffsetBytes(),
                                  ondisk::kDir3Leaf1Magic, "dir leaf1");
      if (!root.ok()) return root.error();
      auto leaf = ParseLeafBlock(std::move(root.value()));
      if (!leaf.ok()) return leaf.error();
      // Enumerate data blocks 0..bestcount-1 directly for stable order.
      const LeafView& view = leaf.value();
      for (uint32_t db = 0; db < view.bestcount; ++db) {
        auto block = io.ReadDirBlock(static_cast<uint64_t>(db)
                                         << io.DirBlockLog(),
                                     ondisk::kDir3DataMagic, "dir data block");
        if (!block.ok()) return block.error();
        const std::vector<uint8_t>& buf = block.value();
        Status s = ParseDataEntries(
            buf.data(), static_cast<uint32_t>(buf.size()),
            d::kData3HeaderSize, static_cast<uint32_t>(buf.size()), has_ftype,
            inode.ino, static_cast<uint64_t>(db) << io.DirBlockLog(), emit,
            nullptr);
        if (!s.ok()) return s;
      }
      return OkStatus();
    }
    case XfsDirFormat::kNode:
      return EnumerateViaLeaf(io, has_ftype, emit);
  }
  return XFS_ERR(XfsError::kInternal, "unhandled directory format")
      .Ino(inode.ino);
}

Result<XfsDirEntry> LookupName(BlockDevice& dev, const XfsGeometry& geo,
                               const XfsInode& inode, std::string_view name) {
  if (!inode.IsDir()) {
    return XFS_ERR(XfsError::kNotDirectory, "inode is not a directory")
        .Ino(inode.ino);
  }
  if (name.empty() || name.size() > kNameMaxLen) {
    return XFS_ERR(XfsError::kNameTooLong, "invalid lookup name")
        .Ino(inode.ino);
  }
  bool has_ftype = geo.features.ftype;

  // Shortform and single-block formats: linear scan.
  auto linear_scan = [&](bool shortform) -> Result<XfsDirEntry> {
    Result<XfsDirEntry> found =
        XFS_ERR(XfsError::kNotFound, "name not found").Ino(inode.ino);
    Status s = OkStatus();
    auto emit = [&](const XfsDirEntry& e) -> Status {
      if (e.name.size() == name.size() &&
          std::memcmp(e.name.data(), name.data(), name.size()) == 0) {
        found = e;
        return XFS_ERR(XfsError::kNotFound, "stop");  // sentinel, unused
      }
      return OkStatus();
    };
    if (shortform) {
      s = ParseShortform(inode, has_ftype, emit);
    } else {
      DirIo io(dev, geo, inode);
      auto block = io.ReadDirBlock(0, ondisk::kDir3BlockMagic, "dir block");
      if (!block.ok()) return block.error();
      const std::vector<uint8_t>& buf = block.value();
      uint32_t len = static_cast<uint32_t>(buf.size());
      uint32_t count = GetBE32(buf.data(), len - 8);
      if (count > (len - 8 - d::kBlock3HeaderSize) / 8) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "block directory leaf count out of range")
            .Ino(inode.ino)
            .Actual(count);
      }
      size_t leaf_entries_start = len - 8 - uint64_t(count) * 8;
      s = ParseDataEntries(buf.data(), len, d::kBlock3HeaderSize,
                           static_cast<uint32_t>(leaf_entries_start), has_ftype,
                           inode.ino, 0, emit, nullptr);
    }
    if (!s.ok()) {
      // The sentinel error above is only returned after a match; verify.
      if (found.ok()) return found;
      return s.error();
    }
    return found;
  };

  if (inode.format == di::kFormatLocal) return linear_scan(true);

  auto format = DetectDirFormat(dev, geo, inode);
  if (!format.ok()) return format.error();
  if (format.value() == XfsDirFormat::kShortform) return linear_scan(true);
  if (format.value() == XfsDirFormat::kBlock) return linear_scan(false);

  // Leaf / node formats: hash search.
  uint32_t target = XfsDirNameHash(name);
  DirIo io(dev, geo, inode);

  auto search_leaf = [&](const LeafView& view, size_t* match_index_out)
      -> Result<XfsDirEntry> {
    std::vector<uint8_t> data_cache;
    uint32_t cached_db = 0xffffffffu;
    uint32_t idx = LeafLowerBound(view, target);
    for (; idx < view.count; ++idx) {
      uint32_t hash = LoadBE32(&view.ents[idx].hashval);
      if (hash != target) break;
      uint32_t addr = LoadBE32(&view.ents[idx].address);
      if (addr == kNullDataPtr) continue;
      uint32_t db = addr >> (io.DirBlockLog() - 3);
      uint32_t off = (addr & ((1u << (io.DirBlockLog() - 3)) - 1)) << 3;
      if (db != cached_db) {
        auto blk = io.ReadDirBlock(static_cast<uint64_t>(db)
                                       << io.DirBlockLog(),
                                   ondisk::kDir3DataMagic, "dir data block");
        if (!blk.ok()) return blk.error();
        data_cache = std::move(blk.value());
        cached_db = db;
      }
      const uint8_t* block = data_cache.data();
      uint32_t block_len = static_cast<uint32_t>(data_cache.size());
      if (off + 9 > block_len) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "leaf address outside data block")
            .Ino(inode.ino)
            .Actual(addr);
      }
      if (GetBE16(block + off) == d::kDataFreeTag) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "leaf points at a free record")
            .Ino(inode.ino)
            .Offset(off);
      }
      uint8_t namelen = GetU8(block + off + 8);
      size_t entsize = d::DataEntrySize(namelen, has_ftype);
      if (off + entsize > block_len ||
          GetBE16(block + off + entsize - 2) != off) {
        return XFS_ERR(XfsError::kCorruptDirectory,
                       "invalid data entry referenced by leaf")
            .Ino(inode.ino)
            .Offset(off);
      }
      const uint8_t* dname = block + off + 9;
      XfsDirEntry e;
      e.name.assign(reinterpret_cast<const char*>(dname), namelen);
      e.ino = GetBE64(block + off);
      if (has_ftype) {
        uint8_t ftype = GetU8(block + off + 9 + namelen);
        if (ftype >= d::kFtMax) {
          return XFS_ERR(XfsError::kCorruptDirectory,
                         "invalid dirent file type")
              .Ino(inode.ino)
              .Offset(off)
              .Actual(ftype);
        }
        e.ftype = ftype;
      }
      if (e.name.size() == name.size() &&
          std::memcmp(e.name.data(), name.data(), name.size()) == 0) {
        *match_index_out = idx;
        return e;
      }
    }
    return XFS_ERR(XfsError::kNotFound, "name not found").Ino(inode.ino);
  };

  if (format.value() == XfsDirFormat::kLeaf) {
    auto root = io.ReadDirBlock(io.LeafOffsetBytes(),
                                ondisk::kDir3Leaf1Magic, "dir leaf1");
    if (!root.ok()) return root.error();
    auto leaf = ParseLeafBlock(std::move(root.value()));
    if (!leaf.ok()) return leaf.error();
    size_t idx = 0;
    return search_leaf(leaf.value(), &idx);
  }

  // Node format: descend to the first leaf whose last hash >= target, then
  // scan its hash run, shifting forward across leaves while the run continues.
  auto node_buf_res =
      io.ReadDirBlock(io.LeafOffsetBytes(), ondisk::kDaNode3Magic, "dir node");
  if (!node_buf_res.ok()) return node_buf_res.error();
  std::vector<uint8_t> node_buf = std::move(node_buf_res.value());
  bool at_leaf = false;
  for (int depth = 0; depth < ondisk::kDaNodeMaxDepth + 2 && !at_leaf; ++depth) {
    auto node = ParseNodeBlock(node_buf);
    if (!node.ok()) return node.error();
    // First entry with hashval >= target; if none, use the last entry.
    size_t chosen = 0;
    bool have = false;
    for (size_t i = 0; i < node.value().entries.size(); ++i) {
      if (node.value().entries[i].first >= target) {
        chosen = i;
        have = true;
        break;
      }
    }
    if (!have) chosen = node.value().entries.size() - 1;
    uint32_t before = node.value().entries[chosen].second;
    uint64_t next_off = static_cast<uint64_t>(before) << io.DirBlockLog();
    if (next_off == io.LeafOffsetBytes()) {
      return XFS_ERR(XfsError::kCorruptDirectory,
                     "da node points back at root")
          .Ino(inode.ino);
    }
    auto blk = io.ReadDirBlock(next_off, ondisk::kDir3LeafnMagic, "dir leaf");
    if (blk.ok()) {
      node_buf = std::move(blk.value());
      at_leaf = true;
      break;
    }
    auto deeper = io.ReadDirBlock(next_off, ondisk::kDaNode3Magic, "dir node");
    if (!deeper.ok()) return blk.error();
    node_buf = std::move(deeper.value());
  }
  if (!at_leaf) {
    return XFS_ERR(XfsError::kCorruptDirectory, "directory node tree too deep")
        .Ino(inode.ino);
  }

  for (;;) {
    auto leaf = ParseLeafBlock(std::move(node_buf));
    if (!leaf.ok()) return leaf.error();
    LeafView view = std::move(leaf.value());
    size_t idx = 0;
    auto found = search_leaf(view, &idx);
    if (found.ok()) return found;
    if (found.error().code != XfsError::kNotFound) return found.error();
    // If the leaf ends with the target hash, duplicates may continue in the
    // next leaf.
    if (LeafLastHash(view) != target || view.forw == 0) {
      return XFS_ERR(XfsError::kNotFound, "name not found").Ino(inode.ino);
    }
    auto next = io.ReadDirBlock(static_cast<uint64_t>(view.forw)
                                    << io.DirBlockLog(),
                                ondisk::kDir3LeafnMagic, "dir leaf");
    if (!next.ok()) return next.error();
    node_buf = std::move(next.value());
  }
}

}  // namespace xfs
