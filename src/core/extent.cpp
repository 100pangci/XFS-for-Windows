// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/extent.h"

#include <algorithm>
#include <cstring>

#include "xfs/btree.h"
#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/verifier.h"

namespace xfs {
namespace {

namespace di = ondisk::inode;
namespace bt = ondisk::btree;

constexpr uint64_t kStartBlockMaskBits = 15 + 20;
constexpr uint64_t kStartBlockValueBits = 17;
constexpr uint64_t kStartBlockMask =
    (((1ULL << kStartBlockMaskBits) - 1) << kStartBlockValueBits);

bool IsNullStartBlock(uint64_t sb) {
  return (sb & kStartBlockMask) == kStartBlockMask;
}

XfsExtent ToExtent(const uint8_t* rec) {
  ondisk::BmbtRec r = ondisk::DecodeBmbtRec(GetBE64(rec), GetBE64(rec + 8));
  XfsExtent e;
  e.startoff = r.startoff;
  e.startblock = r.startblock;
  e.blockcount = r.blockcount;
  e.unwritten = r.unwritten;
  return e;
}

Status ValidateExtentList(const XfsGeometry& geo, const XfsInode& inode,
                          std::vector<XfsExtent>& extents) {
  uint64_t prev_end = 0;
  for (size_t i = 0; i < extents.size(); ++i) {
    const XfsExtent& e = extents[i];
    if (e.blockcount == 0) {
      return XFS_ERR(XfsError::kCorruptExtent, "zero-length extent")
          .Ino(inode.ino)
          .Metadata("extent")
          .Actual(e.startoff);
    }
    if (e.startoff > ondisk::kBmbtMaxStartoff) {
      return XFS_ERR(XfsError::kCorruptExtent, "extent startoff out of range")
          .Ino(inode.ino)
          .Metadata("extent")
          .Actual(e.startoff);
    }
    if (i > 0 && e.startoff < prev_end) {
      return XFS_ERR(XfsError::kCorruptExtent,
                     "extents overlap or are out of order")
          .Ino(inode.ino)
          .Metadata("extent")
          .Expected(prev_end)
          .Actual(e.startoff);
    }
    if (IsNullStartBlock(e.startblock)) {
      return XFS_ERR(XfsError::kCorruptExtent,
                     "on-disk extent has delayed-allocation startblock")
          .Ino(inode.ino)
          .Metadata("extent")
          .Actual(e.startblock);
    }
    // startblock encodes (AG number, AG-relative block), not a linear block
    // index. It can exceed sb_dblocks when agblocks is not a power of two.
    // Validate the decoded range, including the shorter final AG.
    if ((e.startblock >> geo.agblklog) >= geo.agcount ||
        !geo.FsbRangeValid(e.startblock, e.blockcount)) {
      return XFS_ERR(XfsError::kCorruptExtent,
                     "extent crosses an allocation group boundary or points "
                     "outside the filesystem")
          .Ino(inode.ino)
          .Metadata("extent")
          .Actual(e.startblock);
    }
    prev_end = e.startoff + e.blockcount;
  }
  return OkStatus();
}

Status LoadExtentsFormat(const XfsGeometry& geo, const XfsInode& inode,
                         std::vector<XfsExtent>* out) {
  if (static_cast<uint64_t>(inode.nextents) * 16 > inode.data_fork.size()) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "data fork extent count exceeds fork size")
        .Ino(inode.ino)
        .Metadata("inode")
        .Expected(inode.data_fork.size() / 16)
        .Actual(inode.nextents);
  }
  out->reserve(inode.nextents);
  for (uint32_t i = 0; i < inode.nextents; ++i) {
    out->push_back(ToExtent(inode.data_fork.data() + i * 16));
  }
  return ValidateExtentList(geo, inode, *out);
}

Status LoadBtreeFormat(BlockDevice& dev, const XfsGeometry& geo,
                       const XfsInode& inode, std::vector<XfsExtent>* out,
                       uint64_t cap) {
  if (inode.data_fork.size() < bt::kBmdrSize) {
    return XFS_ERR(XfsError::kCorruptInode, "bmbt fork too small")
        .Ino(inode.ino)
        .Metadata("inode");
  }
  uint16_t root_level = GetBE16(inode.data_fork.data(), 0);
  uint16_t root_numrecs = GetBE16(inode.data_fork.data(), 2);
  const size_t fork_size = inode.data_fork.size();
  const size_t node_maxrecs =
      (fork_size - bt::kBmdrSize) / (bt::kBmbtKeySize + bt::kBmbtPtrSize);
  const size_t leaf_maxrecs = (fork_size - bt::kBmdrSize) / bt::kBmbtRecSize;

  std::vector<XfsExtent> extents;

  if (root_level == 0) {
    // Root itself is a leaf: records live in the fork.
    if (root_numrecs == 0 || root_numrecs > leaf_maxrecs) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "bmbt root leaf record count invalid")
          .Ino(inode.ino)
          .Metadata("inode")
          .Expected(leaf_maxrecs)
          .Actual(root_numrecs);
    }
    for (uint16_t i = 0; i < root_numrecs; ++i) {
      extents.push_back(
          ToExtent(inode.data_fork.data() + bt::kBmdrSize + i * 16));
    }
  } else {
    if (root_numrecs == 0 || root_numrecs > node_maxrecs) {
      return XFS_ERR(XfsError::kCorruptInode,
                     "bmbt root node record count invalid")
          .Ino(inode.ino)
          .Metadata("inode")
          .Expected(node_maxrecs)
          .Actual(root_numrecs);
    }

    BtreeSpec spec;
    spec.name = "bmbt";
    spec.format = BtreeFormat::kLong;
    spec.block_size = geo.block_size;
    spec.key_size = bt::kBmbtKeySize;
    spec.ptr_size = bt::kBmbtPtrSize;
    spec.rec_size = bt::kBmbtRecSize;
    spec.magic = ondisk::kBmapCrcMagic;
    spec.max_depth = bt::kMaxTraversalLevels;
    spec.null_ptr = 0xffffffffffffffffull;
    spec.expected_owner = inode.ino;
    spec.owner_size = 8;
    spec.uuid = geo.meta_uuid;
    spec.sb_lsn = 0;
    spec.ptr_to_daddr = [&](uint64_t fsb, uint64_t* daddr) {
      if (!geo.FsbValid(fsb)) return false;
      *daddr = geo.FsbToByte(fsb) / 512;
      return true;
    };

    BtreeWalkCallbacks cb;
    cb.read_block = [&](uint64_t fsb, std::vector<uint8_t>* buf) -> bool {
      if (!geo.FsbValid(fsb)) return false;
      buf->resize(geo.block_size);
      return dev.Read(geo.FsbToByte(fsb), buf->data(), geo.block_size);
    };
    cb.ptr_in_range = [&](uint64_t ptr) {
      return ptr != spec.null_ptr && geo.FsbValid(ptr);
    };
    cb.visit = [&](uint64_t /*fsb*/, const BtreeBlockMeta& meta,
                   const uint8_t* buf) -> Status {
      if (meta.level != 0) return OkStatus();
      for (uint16_t i = 0; i < meta.numrecs; ++i) {
        extents.push_back(ToExtent(BtreeLeafRec(buf, meta, spec, i)));
        if (extents.size() > cap) {
          return XFS_ERR(XfsError::kUnsupportedGeometry,
                         "file has too many extents for this build")
              .Ino(inode.ino)
              .Metadata("bmbt");
        }
      }
      return OkStatus();
    };
    cb.check_siblings = true;

    // Root children: keys at 4 + i*8, pointers at 4 + maxrecs*8 + i*8.
    const uint8_t* ptrs =
        inode.data_fork.data() + bt::kBmdrSize + node_maxrecs * 8;
    const uint8_t* keys = inode.data_fork.data() + bt::kBmdrSize;

    // All children belong to one embedded-root tree. Walking each separately
    // resets sibling state and falsely rejects a second internal child whose
    // left sibling is the first child (bmdr level >= 2).
    uint64_t prev_key = 0;
    std::vector<uint64_t> children;
    for (uint16_t i = 0; i < root_numrecs; ++i) {
      uint64_t key = GetBE64(keys + i * 8);
      if (i > 0 && key <= prev_key) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "bmbt root keys out of order")
            .Ino(inode.ino)
            .Metadata("bmbt");
      }
      prev_key = key;
      uint64_t child = GetBE64(ptrs + i * 8);
      if (child == spec.null_ptr) {
        return XFS_ERR(XfsError::kCorruptInode, "bmbt root has null child")
            .Ino(inode.ino)
            .Metadata("bmbt")
            .Actual(i);
      }
      children.push_back(child);
    }
    auto original_visit = cb.visit;
    cb.visit = [&](uint64_t fsb, const BtreeBlockMeta& meta,
                   const uint8_t* buf) -> Status {
      for (size_t i = 0; i < children.size(); ++i) {
        if (children[i] != fsb) continue;
        const uint64_t actual = meta.level == 0
            ? ToExtent(BtreeLeafRec(buf, meta, spec, 0)).startoff
            : BtreeKeyValue(buf, meta, spec, 0);
        const uint64_t expected = GetBE64(keys + i * 8);
        if (actual != expected) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "bmbt root key does not match its child")
              .Ino(inode.ino)
              .Metadata("bmbt")
              .Expected(expected)
              .Actual(actual);
        }
        break;
      }
      return original_visit(fsb, meta, buf);
    };
    Status walked = WalkBtreeForest(spec, children,
                                    static_cast<uint16_t>(root_level - 1), cb);
    if (!walked.ok()) return walked;
  }

  Status s = ValidateExtentList(geo, inode, extents);
  if (!s.ok()) return s;
  if (extents.size() != inode.nextents) {
    XFS_WARN("inode " << inode.ino << ": di_nextents=" << inode.nextents
                      << " but enumerated " << extents.size() << " extents");
  }
  *out = std::move(extents);
  return OkStatus();
}

}  // namespace

Status LoadDataForkExtents(BlockDevice& dev, const XfsGeometry& geo,
                           const XfsInode& inode, std::vector<XfsExtent>* out,
                           uint64_t cap) {
  out->clear();
  switch (inode.format) {
    case di::kFormatExtents:
      return LoadExtentsFormat(geo, inode, out);
    case di::kFormatBtree:
      return LoadBtreeFormat(dev, geo, inode, out, cap);
    case di::kFormatLocal:
      return XFS_ERR(XfsError::kInvalidArgument,
                     "local format has no extent list")
          .Ino(inode.ino)
          .Metadata("inode");
    case di::kFormatDev:
      return XFS_ERR(XfsError::kInvalidArgument,
                     "device node has no extent list")
          .Ino(inode.ino)
          .Metadata("inode");
    default:
      return XFS_ERR(XfsError::kCorruptInode, "unexpected fork format")
          .Ino(inode.ino)
          .Metadata("inode")
          .Actual(inode.format);
  }
}

Result<XfsFileMapping> MapFileOffset(BlockDevice& dev, const XfsGeometry& geo,
                                     const XfsInode& inode,
                                     uint64_t file_offset) {
  XfsFileMapping m;
  // Note: file_offset is NOT bounded by inode.size. Directory leaf/free
  // blocks live at huge logical offsets (XFS_DIR2_LEAF_OFFSET = 32 GiB,
  // XFS_DIR2_FREE_OFFSET = 64 GiB) and are mapped by real extents; the data
  // fork is the authority. Callers that read file data clamp to inode.size
  // themselves; directory callers detect corruption via the block magic.
  uint64_t file_block = file_offset >> geo.block_log;

  std::vector<XfsExtent> extents;
  Status s = LoadDataForkExtents(dev, geo, inode, &extents);
  if (!s.ok()) return s.error();

  // Largest extent whose startoff <= file_block.
  size_t lo = 0, hi = extents.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (extents[mid].startoff <= file_block) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  uint64_t file_blocks_total = (inode.size + geo.block_size - 1) >> geo.block_log;
  if (lo == 0) {
    m.is_hole = true;
    m.block_offset = 0;
    m.block_count = extents.empty() ? file_blocks_total : extents[0].startoff;
    if (m.block_count <= file_block) m.block_count = file_block + 1;
    return m;
  }
  const XfsExtent& e = extents[lo - 1];
  if (file_block >= e.EndBlock()) {
    m.is_hole = true;
    m.block_offset = e.EndBlock();
    uint64_t next_start =
        lo < extents.size() ? extents[lo].startoff : file_blocks_total;
    if (next_start <= file_block) next_start = file_block + 1;
    m.block_count = next_start - e.EndBlock();
    return m;
  }
  m.is_hole = false;
  m.is_unwritten = e.unwritten;
  m.fs_block = e.startblock + (file_block - e.startoff);
  m.block_offset = e.startoff;
  m.block_count = e.blockcount;
  return m;
}

Status ReadLocalData(const XfsInode& inode, uint64_t offset, void* buffer,
                     size_t len, size_t* bytes_read) {
  *bytes_read = 0;
  if (offset >= inode.size) return OkStatus();
  uint64_t avail = inode.size - offset;
  size_t to_copy = static_cast<size_t>(std::min<uint64_t>(avail, len));
  if (offset + to_copy > inode.data_fork.size()) {
    return XFS_ERR(XfsError::kCorruptInode, "local data extends past fork")
        .Ino(inode.ino)
        .Metadata("inode");
  }
  std::memcpy(buffer, inode.data_fork.data() + offset, to_copy);
  *bytes_read = to_copy;
  return OkStatus();
}

Result<size_t> ReadFileData(BlockDevice& dev, const XfsGeometry& geo,
                            const XfsInode& inode, uint64_t file_offset,
                            void* buffer, size_t len,
                            const std::vector<XfsExtent>* cached_extents) {
  if (inode.format == di::kFormatLocal) {
    size_t copied = 0;
    Status s = ReadLocalData(inode, file_offset, buffer, len, &copied);
    if (!s.ok()) return s.error();
    return copied;
  }
  if (inode.format != di::kFormatExtents && inode.format != di::kFormatBtree) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "inode has no readable data fork")
        .Ino(inode.ino)
        .Actual(inode.format);
  }

  // Parse the extent list once per call (or use the caller's cached copy).
  // The previous implementation re-parsed it for every mapped chunk, which is
  // O(extents^2) for fragmented files.
  std::vector<XfsExtent> local_extents;
  const std::vector<XfsExtent>* extents = cached_extents;
  if (!extents) {
    Status s = LoadDataForkExtents(dev, geo, inode, &local_extents);
    if (!s.ok()) return s.error();
    extents = &local_extents;
  }

  uint8_t* out = static_cast<uint8_t*>(buffer);
  size_t total = 0;
  const uint64_t block_size = geo.block_size;
  const uint64_t file_blocks_total =
      (inode.size + block_size - 1) >> geo.block_log;

  while (total < len && file_offset < inode.size) {
    const size_t want = static_cast<size_t>(
        std::min<uint64_t>(len - total, inode.size - file_offset));
    const uint64_t file_block = file_offset >> geo.block_log;

    // Largest extent whose startoff <= file_block.
    size_t lo = 0, hi = extents->size();
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if ((*extents)[mid].startoff <= file_block) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    const XfsExtent* e = lo ? &(*extents)[lo - 1] : nullptr;
    const uint64_t offset_in_block = file_offset & (block_size - 1);
    size_t chunk = 0;

    if (!e || file_block >= e->EndBlock()) {
      // Hole: zero up to the next extent (or EOF) in one pass.
      const uint64_t next_start =
          lo < extents->size() ? (*extents)[lo].startoff : file_blocks_total;
      uint64_t end_byte = next_start * block_size;
      if (end_byte > inode.size) end_byte = inode.size;
      if (end_byte <= file_offset) end_byte = file_offset + 1;
      chunk = static_cast<size_t>(std::min<uint64_t>(want,
                                                     end_byte - file_offset));
      std::memset(out + total, 0, chunk);
    } else {
      const uint64_t extent_offset_blocks = file_block - e->startoff;
      const uint64_t extent_remaining_bytes =
          (e->blockcount - extent_offset_blocks) * block_size - offset_in_block;
      chunk = static_cast<size_t>(
          std::min<uint64_t>(want, extent_remaining_bytes));
      if (e->unwritten) {
        std::memset(out + total, 0, chunk);
      } else {
        const uint64_t read_off =
            geo.FsbToByte(e->startblock + extent_offset_blocks) +
            offset_in_block;
        if (!dev.Read(read_off, out + total, chunk)) {
          return XFS_ERR(XfsError::kIoError, "failed to read file data")
              .Ino(inode.ino)
              .Offset(read_off);
        }
      }
    }
    if (chunk == 0) break;  // safety: never spin
    total += chunk;
    file_offset += chunk;
  }
  return total;
}

}  // namespace xfs
