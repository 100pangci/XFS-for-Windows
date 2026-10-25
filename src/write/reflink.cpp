// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - native reflink range cloning (RW-6C).
//
// This is xfs_reflink_remap_blocks / xfs_reflink_remap_extent: the source
// physical extents are shared, never copied.  Written source extents get their
// refcount increased and a destination rmap owner; holes and unwritten source
// extents become holes in the destination.  The destination range is replaced
// in bounded rolling transactions so a large clone does not have to fit one
// log record.  Each transaction is internally atomic.
#include <algorithm>
#include <cstring>
#include <vector>

#include "xfs/alloc.h"
#include "xfs/bmap.h"
#include "xfs/crash.h"
#include "xfs/endian.h"
#include "xfs/filesystem.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"

namespace xfs {
namespace {

namespace di = ondisk::inode;

constexpr uint64_t kCloneChunkBlocks = 32;
constexpr uint64_t kMaxFileOffset = (1ull << 54);

bool FindExtentAt(const std::vector<XfsExtent>& extents, uint64_t block,
                  const XfsExtent** out) {
  size_t lo = 0, hi = extents.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (extents[mid].startoff <= block) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return false;
  const XfsExtent& e = extents[lo - 1];
  if (block >= e.EndBlock()) return false;
  *out = &e;
  return true;
}

Status StoreTimestampFields(uint8_t* raw, const XfsTimestamp& now) {
  Status s = StoreInodeTimestamp(raw, di::kMtime, now);
  if (!s.ok()) return s;
  return StoreInodeTimestamp(raw, di::kCtime, now);
}

struct ClonePiece {
  uint64_t dst_block = 0;
  uint32_t len = 0;
  uint64_t src_fsb = 0;
  bool written = false;
  bool already_mapped = false;
};

// One desired destination mapping for a single file block.
struct DesiredBlock {
  bool written = false;
  uint64_t src_fsb = 0;
  bool already = false;
};

}  // namespace

Result<uint64_t> XfsFilesystem::ReflinkRange(uint64_t src_ino,
                                             uint64_t src_offset,
                                             uint64_t dst_ino,
                                             uint64_t dst_offset,
                                             uint64_t length) {
  if (!writable_) {
    return XFS_ERR(XfsError::kReadOnlyVolume,
                   "filesystem is mounted read-only")
        .Ino(src_ino)
        .Metadata("reflink");
  }
  if (!geometry_.features.reflink) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "filesystem does not support reflink")
        .Metadata("reflink");
  }
  const uint64_t bs = geometry_.block_size;
  if ((src_offset & (bs - 1)) != 0 || (dst_offset & (bs - 1)) != 0) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "reflink offsets must be filesystem-block aligned")
        .Ino(src_ino)
        .Metadata("reflink");
  }
  if (src_offset > kMaxFileOffset || dst_offset > kMaxFileOffset) {
    return XFS_ERR(XfsError::kOutOfRange, "reflink offset out of range")
        .Ino(src_ino)
        .Metadata("reflink");
  }

  std::lock_guard<std::mutex> lock(mutation_mutex_);
  auto src = OpenInode(src_ino);
  if (!src.ok()) return src.error();
  auto dst = OpenInode(dst_ino);
  if (!dst.ok()) return dst.error();
  if (!src.value().IsRegular() || !dst.value().IsRegular()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "reflink requires two regular files")
        .Ino(src_ino)
        .Metadata("reflink");
  }
  // A regular file with no allocated extents can still have a local (inline)
  // data fork: an empty file or a sparse file grown with ftruncate.  Both are
  // all-hole sources and legal clone destinations.
  if ((src.value().format != di::kFormatLocal &&
       src.value().format != di::kFormatExtents &&
       src.value().format != di::kFormatBtree) ||
      (dst.value().format != di::kFormatLocal &&
       dst.value().format != di::kFormatExtents &&
       dst.value().format != di::kFormatBtree)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "reflink requires a regular data fork")
        .Ino(src_ino)
        .Metadata("reflink");
  }
  if ((src.value().flags & di::kDiFlagImmutable) != 0 ||
      (dst.value().flags & di::kDiFlagImmutable) != 0 ||
      (src.value().flags & di::kDiFlagAppend) != 0 ||
      (dst.value().flags & di::kDiFlagAppend) != 0) {
    return XFS_ERR(XfsError::kPermissionDenied,
                   "reflink refuses immutable/append-only files")
        .Ino(src_ino)
        .Metadata("reflink");
  }

  const uint64_t src_size = src.value().size;
  const uint64_t dst_size = dst.value().size;
  if (length == 0) {
    if (src_offset >= src_size) return uint64_t{0};
    length = src_size - src_offset;
  } else {
    if (src_offset >= src_size) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "reflink source range starts at or beyond EOF")
          .Ino(src_ino)
          .Metadata("reflink");
    }
    length = std::min(length, src_size - src_offset);
  }
  if (length == 0) return uint64_t{0};
  if (dst_offset > kMaxFileOffset || length > kMaxFileOffset - dst_offset) {
    return XFS_ERR(XfsError::kOutOfRange, "reflink destination overflows")
        .Ino(dst_ino)
        .Metadata("reflink");
  }

  const uint64_t src_first = src_offset >> geometry_.block_log;
  const uint64_t dst_first = dst_offset >> geometry_.block_log;
  const bool ends_at_src_eof = src_offset + length == src_size;
  uint64_t src_last = 0;
  if (ends_at_src_eof) {
    src_last = (src_size + bs - 1) >> geometry_.block_log;
    if (src_last > src_first) --src_last;
    else src_last = src_first;
  } else {
    if ((length & (bs - 1)) != 0) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "a non-EOF reflink length must be block aligned")
          .Ino(src_ino)
          .Metadata("reflink");
    }
    src_last = (src_offset + length - 1) >> geometry_.block_log;
  }
  const uint64_t block_count = src_last - src_first + 1;
  if (block_count == 0) return uint64_t{0};

  // A partial source EOF block may only be shared when it also becomes the
  // destination EOF.  Otherwise stale bytes beyond the source EOF would leak.
  if (!ends_at_src_eof &&
      (dst_offset + length) > dst_size) {
    // fine: block-aligned
  } else if ((length & (bs - 1)) != 0 &&
             dst_offset + length < dst_size) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "partial EOF block would land inside the destination EOF")
        .Ino(dst_ino)
        .Metadata("reflink");
  }

  const uint64_t dst_block_end = dst_first + block_count;
  if (src_ino == dst_ino &&
      src_first < dst_block_end && dst_first < src_first + block_count) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "overlapping reflink within one file is not allowed")
        .Ino(src_ino)
        .Metadata("reflink");
  }

  auto src_extents = LoadExtentsCached(src.value());
  if (!src_extents.ok()) return src_extents.error();
  auto dst_extents = LoadExtentsCached(dst.value());
  if (!dst_extents.ok()) return dst_extents.error();

  // Build the desired mapping for every destination block, split into runs
  // that either already map exactly the desired source block or must change.
  std::vector<ClonePiece> pieces;
  for (uint64_t i = 0; i < block_count; ++i) {
    const uint64_t sb = src_first + i;
    const uint64_t db = dst_first + i;
    DesiredBlock desired;
    const XfsExtent* se = nullptr;
    if (FindExtentAt(*src_extents.value(), sb, &se) && !se->unwritten) {
      desired.written = true;
      desired.src_fsb = se->startblock + (sb - se->startoff);
    }
    const XfsExtent* de = nullptr;
    if (FindExtentAt(*dst_extents.value(), db, &de) && !de->unwritten) {
      if (desired.written && de->startblock + (db - de->startoff) ==
                                 desired.src_fsb) {
        desired.already = true;
      }
    } else if (!desired.written && de == nullptr) {
      desired.already = true;
    }

    if (!pieces.empty()) {
      ClonePiece& last = pieces.back();
      const bool contiguous =
          last.written == desired.written &&
          last.already_mapped == desired.already &&
          last.dst_block + last.len == db &&
          (!desired.written ||
           last.src_fsb + last.len == desired.src_fsb);
      if (contiguous) {
        ++last.len;
        continue;
      }
    }
    ClonePiece piece;
    piece.dst_block = db;
    piece.len = 1;
    piece.src_fsb = desired.src_fsb;
    piece.written = desired.written;
    piece.already_mapped = desired.already;
    pieces.push_back(piece);
  }

  const uint64_t dst_new_size = std::max(dst_size, dst_offset + length);
  bool source_flagged = (src.value().flags2 & di::kDiFlag2Reflink) != 0;
  bool any_shared = false;
  for (const ClonePiece& p : pieces) {
    if (p.written && !p.already_mapped) any_shared = true;
  }

  // Process changed pieces in <=32-block transactions.  Already-mapped ranges
  // need no refcount change; this also makes cloning a file onto its own
  // mapping idempotent (xfs_reflink_remap_extent skips dmap == smap).
  std::vector<ClonePiece> work;
  for (const ClonePiece& p : pieces) {
    if (p.already_mapped) continue;
    work.push_back(p);
  }

  size_t wi = 0;
  while (wi < work.size()) {
    std::vector<ClonePiece> chunk;
    uint64_t used = 0;
    while (wi < work.size() && used < kCloneChunkBlocks) {
      ClonePiece p = work[wi++];
      const uint64_t room = kCloneChunkBlocks - used;
      if (p.len > room) {
        ClonePiece head = p;
        head.len = static_cast<uint32_t>(room);
        chunk.push_back(head);
        p.dst_block += room;
        p.src_fsb += room;
        p.len -= static_cast<uint32_t>(room);
        work.insert(work.begin() + static_cast<ptrdiff_t>(wi), p);
        used += room;
        break;
      }
      chunk.push_back(p);
      used += p.len;
    }
    if (chunk.empty()) break;

    auto txn = BeginTransaction();
    if (!txn.ok()) return txn.error();
    BlockAllocator alloc(&txn.value(), device_.get(), &geometry_);
    auto fresh = OpenInode(dst_ino);
    if (!fresh.ok()) return fresh.error();
    DataForkEditor editor(&txn.value(), &alloc, device_.get(), &geometry_);
    Status s = editor.Load(fresh.value());
    if (!s.ok()) return s.error();
    editor.SetFreeRemovedBlocks(true);

    uint64_t chunk_last_block = 0;
    for (const ClonePiece& p : chunk) {
      chunk_last_block = std::max<uint64_t>(chunk_last_block,
                                            p.dst_block + p.len);
      s = editor.RemoveRange(p.dst_block, p.dst_block + p.len);
      if (!s.ok()) return s.error();
      if (!p.written) continue;
      // Share the source physical range.
      s = alloc.RefcountIncrease(p.src_fsb, p.len);
      if (!s.ok()) return s.error();
      s = alloc.RmapMap(
          p.src_fsb, p.len,
          RmapOwner::Data(dst_ino, p.dst_block, /*unwritten=*/false));
      if (!s.ok()) return s.error();
      XfsExtent mapping;
      mapping.startoff = p.dst_block;
      mapping.startblock = p.src_fsb;
      mapping.blockcount = p.len;
      mapping.unwritten = false;
      s = editor.InsertExtent(mapping);
      if (!s.ok()) return s.error();
      editor.AddNblocks(static_cast<int64_t>(p.len));
      any_shared = true;
    }

    auto raw = ReadRawInode(dst_ino);
    if (!raw.ok()) return raw.error();
    std::vector<uint8_t> raw_inode = std::move(raw.value());
    uint32_t ilog_fields = 0;
    std::vector<uint8_t> fork_region;
    s = editor.Finalize(&raw_inode, &ilog_fields, &fork_region);
    if (!s.ok()) return s.error();
    uint64_t chunk_size = std::max<uint64_t>(
        dst_size, std::min<uint64_t>(dst_new_size, chunk_last_block * bs));
    if (any_shared) {
      PutBE64(raw_inode.data(), di::kFlags2,
              GetBE64(raw_inode.data(), di::kFlags2) | di::kDiFlag2Reflink);
    }
    PutBE64(raw_inode.data(), di::kSize, chunk_size);
    const XfsTimestamp now = CurrentTimestamp();
    s = StoreTimestampFields(raw_inode.data(), now);
    if (!s.ok()) return s.error();
    s = LogPreparedInode(txn.value(), dst_ino, std::move(raw_inode),
                         ilog_fields, fork_region);
    if (!s.ok()) return s.error();

    // The source's reflink flag must become durable in the same transaction as
    // the first shared mapping; otherwise a later source write could treat a
    // shared extent as private.
    if (src_ino != dst_ino && any_shared && !source_flagged) {
      s = UpdateInodeInTransaction(
          txn.value(), src_ino, 0, {},
          [](uint8_t* inode) -> Status {
            PutBE64(inode, di::kFlags2,
                    GetBE64(inode, di::kFlags2) | di::kDiFlag2Reflink);
            return OkStatus();
          });
      if (!s.ok()) return s.error();
      source_flagged = true;
    }
    s = alloc.Commit();
    if (!s.ok()) return s.error();
    MaybeCrash(CrashPoint::kAfterBmapSwitch, "reflink clone");
    s = CommitTransaction(txn.value());
    if (!s.ok()) return s.error();
    InvalidateCaches(CacheInvalidation::Extents(dst_ino));
    InvalidateCaches(CacheInvalidation::Inode(dst_ino));
  }

  // Exact final size/flags.  This also covers all-hole clones (no shared
  // record) and skip-only clones whose size still has to move.
  auto final_inode = OpenInode(dst_ino);
  if (!final_inode.ok()) return final_inode.error();
  const bool need_size = final_inode.value().size != dst_new_size;
  const bool need_flag =
      any_shared &&
      (final_inode.value().flags2 & di::kDiFlag2Reflink) == 0;
  if (need_size || need_flag) {
    auto txn = BeginTransaction();
    if (!txn.ok()) return txn.error();
    const XfsTimestamp now = CurrentTimestamp();
    Status s = UpdateInodeInTransaction(
        txn.value(), dst_ino, 0, {},
        [&](uint8_t* raw) -> Status {
          PutBE64(raw, di::kSize, dst_new_size);
          if (need_flag) {
            PutBE64(raw, di::kFlags2,
                    GetBE64(raw, di::kFlags2) | di::kDiFlag2Reflink);
          }
          return StoreTimestampFields(raw, now);
        });
    if (!s.ok()) return s.error();
    if (src_ino != dst_ino && any_shared && !source_flagged) {
      s = UpdateInodeInTransaction(
          txn.value(), src_ino, 0, {},
          [](uint8_t* raw) -> Status {
            PutBE64(raw, di::kFlags2,
                    GetBE64(raw, di::kFlags2) | di::kDiFlag2Reflink);
            return OkStatus();
          });
      if (!s.ok()) return s.error();
    }
    s = CommitTransaction(txn.value());
    if (!s.ok()) return s.error();
    InvalidateCaches(CacheInvalidation::Inode(dst_ino));
  }

  XFS_EXT_DEBUG("reflink " << src_ino << "@" << src_offset << " -> "
                           << dst_ino << "@" << dst_offset << " length "
                           << length);
  return length;
}

}  // namespace xfs
