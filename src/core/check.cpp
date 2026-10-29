// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - read-only structural consistency validation.
//
// Owner: CheckReport aggregation, inode enumeration from the inobt/finobt and
// every cross-structure check performed by `xfswin check`. This module never
// writes metadata and does not participate in recovery or writer transactions.
#include "xfs/filesystem.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "xfs/ag.h"
#include "xfs/btree.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/extent.h"
#include "xfs/ialloc.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/refcount.h"
#include "xfs/rmap.h"
#include "xfs/symlink.h"
#include "xfs/verifier.h"
#include "xfs/write.h"

namespace xfs {

namespace bt = ondisk::btree;
namespace di = ondisk::inode;
namespace sym = ondisk::symlink;
namespace lf = logfmt;

const char* ToString(CheckStatus status) {
  switch (status) {
    case CheckStatus::kOk: return "OK";
    case CheckStatus::kWarning: return "WARNING";
    case CheckStatus::kCorrupt: return "CORRUPT";
    case CheckStatus::kUnsupported: return "UNSUPPORTED";
  }
  return "?";
}

void CheckReport::Add(std::string name, CheckStatus status,
                      std::string message) {
  items.push_back({std::move(name), status, std::move(message)});
}

CheckStatus CheckReport::Overall() const {
  CheckStatus worst = CheckStatus::kOk;
  auto rank = [](CheckStatus s) {
    switch (s) {
      case CheckStatus::kOk: return 0;
      case CheckStatus::kWarning: return 1;
      case CheckStatus::kUnsupported: return 2;
      case CheckStatus::kCorrupt: return 3;
    }
    return 0;
  };
  for (const auto& item : items) {
    if (rank(item.status) > rank(worst)) worst = item.status;
  }
  return worst;
}

std::string CheckReport::Summary() const {
  int ok = 0, warn = 0, corrupt = 0, unsupported = 0;
  for (const auto& item : items) {
    switch (item.status) {
      case CheckStatus::kOk: ++ok; break;
      case CheckStatus::kWarning: ++warn; break;
      case CheckStatus::kCorrupt: ++corrupt; break;
      case CheckStatus::kUnsupported: ++unsupported; break;
    }
  }
  std::string s = std::string(ToString(Overall())) + ": " +
                  std::to_string(ok) + " ok, " + std::to_string(warn) +
                  " warnings, " + std::to_string(corrupt) + " corrupt, " +
                  std::to_string(unsupported) + " unsupported";
  return s;
}

namespace {

struct FreeSpaceCounts {
  uint64_t blocks = 0;
  uint32_t longest = 0;
  uint64_t records = 0;
};

struct InodeCounts {
  uint64_t count = 0;
  uint64_t freecount = 0;
  uint64_t records = 0;
};

// Counts the bmbt blocks reachable from an in-fork root.  Shared by the data
// and attribute forks; only the fork bytes/root counters differ.
Status CountForkBmbtBlocks(BlockDevice& dev, const XfsGeometry& geo,
                           uint64_t ino, const std::vector<uint8_t>& fork,
                           uint16_t root_level, uint16_t root_numrecs,
                           const char* what, uint64_t* out,
                           std::vector<uint64_t>* block_list = nullptr) {
  *out = 0;
  if (root_level == 0) return OkStatus();
  if (fork.size() < bt::kBmdrSize) {
    return XFS_ERR(XfsError::kCorruptInode, std::string(what) + " root too small")
        .Ino(ino)
        .Metadata("inode");
  }
  const size_t node_maxrecs =
      (fork.size() - bt::kBmdrSize) /
      (bt::kBmbtKeySize + bt::kBmbtPtrSize);
  BtreeSpec spec;
  spec.name = what;
  spec.format = BtreeFormat::kLong;
  spec.block_size = geo.block_size;
  spec.key_size = bt::kBmbtKeySize;
  spec.ptr_size = bt::kBmbtPtrSize;
  spec.rec_size = bt::kBmbtRecSize;
  spec.magic = ondisk::kBmapCrcMagic;
  spec.max_depth = bt::kMaxTraversalLevels;
  spec.null_ptr = ~0ull;
  spec.expected_owner = ino;
  spec.owner_size = 8;
  spec.uuid = geo.meta_uuid;
  spec.ptr_to_daddr = [&](uint64_t fsb, uint64_t* daddr) {
    if (!geo.FsbValid(fsb)) return false;
    *daddr = geo.FsbToByte(fsb) / 512;
    return true;
  };
  uint64_t bmbt_blocks = 0;
  BtreeWalkCallbacks cb;
  cb.read_block = [&](uint64_t fsb, std::vector<uint8_t>* buf) {
    if (!geo.FsbValid(fsb)) return false;
    buf->resize(geo.block_size);
    return dev.Read(geo.FsbToByte(fsb), buf->data(), buf->size());
  };
  cb.ptr_in_range = [&](uint64_t ptr) {
    return ptr != spec.null_ptr && geo.FsbValid(ptr);
  };
  cb.visit = [&](uint64_t fsb, const BtreeBlockMeta&,
                 const uint8_t*) -> Status {
    ++bmbt_blocks;
    if (block_list != nullptr) block_list->push_back(fsb);
    return OkStatus();
  };
  std::vector<uint64_t> children;
  for (uint16_t i = 0; i < root_numrecs; ++i) {
    const uint64_t ptr =
        GetBE64(fork.data(), bt::kBmdrSize + node_maxrecs * 8 + i * 8);
    if (ptr == spec.null_ptr) {
      return XFS_ERR(XfsError::kCorruptInode,
                     std::string(what) + " root has a null child")
          .Ino(ino)
          .Metadata("inode");
    }
    children.push_back(ptr);
  }
  Status s = WalkBtreeForest(spec, children,
                             static_cast<uint16_t>(root_level - 1), cb);
  if (!s.ok()) return s;
  *out = bmbt_blocks;
  return OkStatus();
}

// Cross-checks every attribute-fork data extent against rmapbt.  DA blocks and
// remote values use OWN_INODE + ATTR_FORK + logical offset; a missing record or
// a record owned by another logical block is corruption.
Status CheckAttrRmap(BlockDevice& dev, const XfsGeometry& geo,
                     const XfsInode& inode, const XattrFork& fork,
                     uint64_t sb_lsn) {
  if (!geo.features.rmapbt) return OkStatus();
  std::map<uint32_t, RmapTree> trees;
  auto lookup_rmap = [&](uint64_t fsb, uint32_t flags, uint64_t logical,
                         const char* what) -> Status {
    const uint32_t agno = static_cast<uint32_t>(fsb >> geo.agblklog);
    const uint32_t agbno = static_cast<uint32_t>(
        fsb & ((1ULL << geo.agblklog) - 1));
    auto it = trees.find(agno);
    if (it == trees.end()) {
      auto loaded = LoadRmapTree(dev, geo, agno, sb_lsn);
      if (!loaded.ok()) return loaded.error();
      it = trees.emplace(agno, std::move(loaded.value())).first;
    }
    const RmapRecord* covering = nullptr;
    for (const RmapRecord& rec : it->second.records) {
      if (rec.key.startblock > agbno) break;
      if (agbno >= rec.key.startblock && agbno < rec.EndBlock()) {
        covering = &rec;
      }
    }
    if (covering == nullptr) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     std::string(what) + " has no reverse mapping")
          .Ino(inode.ino)
          .Ag(agno)
          .Agbno(agbno)
          .Metadata("rmap");
    }
    if (covering->key.owner != inode.ino ||
        (covering->key.flags & kRmapAttrFork) == 0 ||
        covering->unwritten()) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     std::string(what) +
                         " reverse mapping has the wrong owner/flags")
          .Ino(inode.ino)
          .Ag(agno)
          .Agbno(agbno)
          .Metadata("rmap")
          .Expected(inode.ino)
          .Actual(covering->key.owner);
    }
    uint64_t expected_offset = 0;
    if (flags == kRmapBmbtBlock) {
      expected_offset = 0;
    } else {
      expected_offset =
          covering->key.offset + (agbno - covering->key.startblock);
    }
    if (expected_offset != logical) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     std::string(what) +
                         " reverse mapping has the wrong offset")
          .Ino(inode.ino)
          .Ag(agno)
          .Agbno(agbno)
          .Metadata("rmap")
          .Expected(logical)
          .Actual(expected_offset);
    }
    return OkStatus();
  };

  for (const XfsExtent& e : fork.extents) {
    for (uint64_t i = 0; i < e.blockcount; ++i) {
      const uint64_t logical = e.startoff + i;
      Status s = lookup_rmap(e.startblock + i, kRmapAttrFork, logical,
                             "attr fork block");
      if (!s.ok()) return s;
    }
  }
  if (inode.aformat == di::kFormatBtree) {
    std::vector<uint64_t> bmbt;
    uint64_t count = 0;
    Status s = CountForkBmbtBlocks(
        dev, geo, inode.ino, inode.attr_fork,
        GetBE16(inode.attr_fork.data(), 0), GetBE16(inode.attr_fork.data(), 2),
        "attr bmbt", &count, &bmbt);
    if (!s.ok()) return s;
    for (uint64_t fsb : bmbt) {
      s = lookup_rmap(fsb, kRmapAttrFork | kRmapBmbtBlock, 0,
                      "attr bmbt block");
      if (!s.ok()) return s;
    }
  }
  return OkStatus();
}

// Recomputes di_nblocks from both forks (data/attr extents plus their bmbt
// blocks) and verifies the attribute fork at the same time.  This is the value
// xfs_repair checks.
Status ExpectedInodeBlocks(BlockDevice& dev, const XfsGeometry& geo,
                           const XfsInode& inode, uint64_t* out) {
  uint64_t blocks = 0;
  if (inode.format != di::kFormatLocal && inode.format != di::kFormatDev &&
      inode.format != di::kFormatUuid) {
    std::vector<XfsExtent> extents;
    Status s = LoadDataForkExtents(dev, geo, inode, &extents);
    if (!s.ok()) return s;
    for (const XfsExtent& e : extents) blocks += e.blockcount;
    if (inode.format == di::kFormatBtree) {
      if (inode.data_fork.size() < bt::kBmdrSize) {
        return XFS_ERR(XfsError::kCorruptInode, "bmbt fork too small")
            .Ino(inode.ino)
            .Metadata("inode");
      }
      uint64_t bmbt = 0;
      s = CountForkBmbtBlocks(dev, geo, inode.ino, inode.data_fork,
                              GetBE16(inode.data_fork.data(), 0),
                              GetBE16(inode.data_fork.data(), 2), "bmbt",
                              &bmbt);
      if (!s.ok()) return s;
      blocks += bmbt;
    }
  }
  if (inode.HasAttrFork()) {
    auto attr = ReadXattrFork(dev, geo, inode);
    if (!attr.ok()) return attr.error();
    for (const XfsExtent& e : attr.value().extents) {
      blocks += e.blockcount;
    }
    if (inode.aformat == di::kFormatBtree) {
      uint64_t bmbt = 0;
      Status s = CountForkBmbtBlocks(dev, geo, inode.ino, inode.attr_fork,
                                     GetBE16(inode.attr_fork.data(), 0),
                                     GetBE16(inode.attr_fork.data(), 2),
                                     "attr bmbt", &bmbt);
      if (!s.ok()) return s;
      blocks += bmbt;
    }
  }
  *out = blocks;
  return OkStatus();
}

}  // namespace

Result<std::vector<uint64_t>> XfsFilesystem::ListAllocatedInodes() const {
  std::vector<uint64_t> out;
  const XfsGeometry& geo = geometry_;
  BlockDevice& dev = *device_;
  uint64_t sb_lsn = superblock_.lsn;
  auto live_sb = ReadSuperblock(dev, 0, /*verify_crc=*/true);
  if (live_sb.ok()) sb_lsn = live_sb.value().lsn;
  for (uint32_t agno = 0; agno < geo.agcount; ++agno) {
    auto headers = ReadAgHeaders(dev, geo, agno);
    if (!headers.ok()) return headers.error();
    const XfsAgf& agf = headers.value().agf;
    const XfsAgi& agi = headers.value().agi;

    BtreeSpec inobt;
    inobt.name = "inobt";
    inobt.format = BtreeFormat::kShort;
    inobt.block_size = geo.block_size;
    inobt.key_size = bt::kInobtKeySize;
    inobt.ptr_size = bt::kInobtPtrSize;
    inobt.rec_size = bt::kInobtRecSize;
    inobt.magic = ondisk::kIbtCrcMagic;
    inobt.null_ptr = bt::kNullAgBlock;
    inobt.expected_owner = agno;
    inobt.owner_size = 4;
    inobt.uuid = geo.meta_uuid;
    inobt.sb_lsn = sb_lsn;
    inobt.ptr_to_daddr = [&](uint64_t agbno, uint64_t* daddr) {
      if (agbno >= agf.length) return false;
      *daddr = geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))) /
               512;
      return true;
    };
    BtreeWalkCallbacks cb;
    cb.read_block = [&](uint64_t agbno, std::vector<uint8_t>* buf) {
      if (agbno >= agf.length) return false;
      buf->resize(geo.block_size);
      return dev.Read(
          geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))),
          buf->data(), geo.block_size);
    };
    cb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
    cb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                   const uint8_t* buf) -> Status {
      if (meta.level != 0) return OkStatus();
      for (uint16_t i = 0; i < meta.numrecs; ++i) {
        const uint8_t* rec = BtreeLeafRec(buf, meta, inobt, i);
        InobtRecord r;
        r.startino = GetBE32(rec);
        r.free = GetBE64(rec + 8);
        if (geo.features.spinodes) {
          r.holemask = GetBE16(rec + 4);
          r.count = GetU8(rec + 6);
          r.freecount = GetU8(rec + 7);
        } else {
          r.holemask = 0;
          r.count = ondisk::kInodesPerChunk;
          r.freecount = GetBE32(rec + 4);
        }
        const uint64_t allocmask =
            geo.features.spinodes ? InobtAllocMask(r.holemask) : ~0ULL;
        for (uint32_t bit = 0; bit < ondisk::kInodesPerChunk; ++bit) {
          if (!((allocmask >> bit) & 1ULL)) continue;
          if ((r.free >> bit) & 1ULL) continue;
          out.push_back(geo.AginoToIno(agno, r.startino + bit));
        }
      }
      return OkStatus();
    };
    Status s = WalkBtree(inobt, agi.root, cb);
    if (!s.ok()) return s.error();
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

CheckReport XfsFilesystem::CheckConsistency(const CheckOptions& options) const {
  CheckReport report;
  const XfsGeometry& geo = geometry_;
  BlockDevice& dev = *device_;

  // Counters and the metadata LSN move with every mutation, so read the
  // primary superblock again instead of trusting the mount-time copy.
  XfsSuperblock live_sb = superblock_;
  auto fresh_sb = ReadSuperblock(dev, 0, /*verify_crc=*/true);
  if (fresh_sb.ok()) live_sb = fresh_sb.value();

  report.Add("superblock", CheckStatus::kOk,
             "magic/uuid/geometry OK, crc " +
                 std::string(live_sb.crc_verified ? "verified" : "skipped"));

  // Secondary superblocks.
  if (options.secondary_superblocks && geo.agcount > 1) {
    uint32_t mismatches = 0;
    uint32_t checked = 0;
    for (uint32_t agno = 1; agno < geo.agcount; ++agno) {
      uint64_t off = geo.AgByteOffset(agno);
      auto sb2 = ReadSuperblock(dev, off, /*verify_crc=*/true);
      if (!sb2.ok()) {
        report.Add("superblock AG" + std::to_string(agno),
                   CheckStatus::kCorrupt, sb2.error().Format());
        ++mismatches;
        continue;
      }
      ++checked;
      const XfsSuperblock& s = sb2.value();
      // Like xfs_repair's compare_sb: only the geometry fields must match.
      // Counters, version numbers, root inode, feature bits and other fields
      // may legitimately differ in secondary copies.
      bool same = s.blocksize == live_sb.blocksize &&
                  s.dblocks == live_sb.dblocks &&
                  s.rblocks == live_sb.rblocks &&
                  s.rextents == live_sb.rextents &&
                  s.logstart == live_sb.logstart &&
                  s.rextsize == live_sb.rextsize &&
                  s.agblocks == live_sb.agblocks &&
                  s.agcount == live_sb.agcount &&
                  s.rbmblocks == live_sb.rbmblocks &&
                  s.logblocks == live_sb.logblocks &&
                  s.sectsize == live_sb.sectsize &&
                  s.inodesize == live_sb.inodesize;
      if (!same) {
        report.Add("superblock AG" + std::to_string(agno),
                   CheckStatus::kWarning,
                   "secondary superblock differs from primary");
        ++mismatches;
      }
    }
    if (mismatches == 0) {
      report.Add("secondary superblocks", CheckStatus::kOk,
                 std::to_string(checked) + " copies verified");
    }
  }

  // Per-AG checks.
  uint64_t total_free = 0;
  uint64_t total_icount = 0;
  uint64_t total_ifree = 0;
  bool rmap_seen = false;
  bool rmap_clean = true;
  bool refcount_seen = false;
  bool refcount_clean = true;
  uint64_t refcount_cow_records = 0;
  if (options.allocation_groups) {
    for (uint32_t agno = 0; agno < geo.agcount; ++agno) {
      auto headers = ReadAgHeaders(dev, geo, agno);
      if (!headers.ok()) {
        report.Add("AG" + std::to_string(agno), CheckStatus::kCorrupt,
                   headers.error().Format());
        continue;
      }
      const XfsAgf& agf = headers.value().agf;
      const XfsAgi& agi = headers.value().agi;
      // sb_fdblocks includes AGFL-reserved blocks and the non-root free space
      // btree blocks (the kernel's fdblocks = freeblks + flcount + btreeblks
      // identity, also used by xfs_initialize_perag_data and xfs_repair).
      total_free += agf.freeblks + agf.flcount + agf.btreeblks;
      total_icount += agi.count;
      total_ifree += agi.freecount;

      if (options.btrees) {
        // bnobt: enumerate free space, validate ordering and totals.
        BtreeSpec bnobt;
        bnobt.name = "bnobt";
        bnobt.format = BtreeFormat::kShort;
        bnobt.block_size = geo.block_size;
        bnobt.key_size = bt::kAllocKeySize;
        bnobt.ptr_size = bt::kAllocPtrSize;
        bnobt.rec_size = bt::kAllocRecSize;
        bnobt.magic = ondisk::kBtbCrcMagic;
        bnobt.null_ptr = bt::kNullAgBlock;
        bnobt.expected_owner = agno;
        bnobt.owner_size = 4;
        bnobt.uuid = geo.meta_uuid;
        bnobt.sb_lsn = live_sb.lsn;
        bnobt.ptr_to_daddr = [&](uint64_t agbno, uint64_t* daddr) {
          if (agbno >= agf.length) return false;
          *daddr = geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))) / 512;
          return true;
        };

        auto read_ag_block = [&](uint64_t agbno, std::vector<uint8_t>* buf) {
          if (agbno >= agf.length) return false;
          buf->resize(geo.block_size);
          return dev.Read(geo.FsbToByte(geo.AgFsb(agno, static_cast<uint32_t>(agbno))),
                          buf->data(), geo.block_size);
        };

        FreeSpaceCounts bno;
        std::vector<std::pair<uint32_t, uint32_t>> free_ranges;
        uint32_t last_end = 0;
        bool first = true;
        uint32_t bno_blocks = 0;
        BtreeWalkCallbacks cb;
        cb.read_block = read_ag_block;
        cb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
        cb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                       const uint8_t* buf) -> Status {
          ++bno_blocks;
          if (meta.level != 0) return OkStatus();
          for (uint16_t i = 0; i < meta.numrecs; ++i) {
            const uint8_t* rec = BtreeLeafRec(buf, meta, bnobt, i);
            uint32_t start = GetBE32(rec);
            uint32_t len = GetBE32(rec + 4);
            if (len == 0 || start >= agf.length ||
                len > agf.length - start) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "invalid free space record")
                  .Ag(agno)
                  .Metadata("bnobt")
                  .Actual(start);
            }
            if (!first && start <= last_end) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "free space records out of order/overlapping")
                  .Ag(agno)
                  .Metadata("bnobt")
                  .Expected(last_end)
                  .Actual(start);
            }
            first = false;
            last_end = start + len - 1;
            free_ranges.push_back({start, len});
            bno.blocks += len;
            bno.longest = std::max(bno.longest, len);
            ++bno.records;
          }
          return OkStatus();
        };
        Status s = WalkBtree(bnobt, agf.bno_root, cb);
        const bool bno_ok = s.ok();
        if (!s.ok()) {
          report.Add("AG" + std::to_string(agno) + " bnobt",
                     CheckStatus::kCorrupt, s.error().Format());
        } else {
          if (bno.blocks != agf.freeblks) {
            report.Add("AG" + std::to_string(agno) + " free space",
                       CheckStatus::kCorrupt,
                       "bnobt total " + std::to_string(bno.blocks) +
                           " != AGF freeblks " + std::to_string(agf.freeblks));
          }
          if (bno.longest != agf.longest) {
            report.Add("AG" + std::to_string(agno) + " free space",
                       CheckStatus::kCorrupt,
                       "bnobt longest " + std::to_string(bno.longest) +
                           " != AGF longest " + std::to_string(agf.longest));
          }
          if (bno.blocks == agf.freeblks && bno.longest == agf.longest) {
            report.Add("AG" + std::to_string(agno) + " bnobt",
                       CheckStatus::kOk,
                       std::to_string(bno.records) + " free extents, " +
                           std::to_string(bno.blocks) + " blocks");
          }
        }

        // cntbt: same records, ordered by block count.
        BtreeSpec cnt = bnobt;
        cnt.name = "cntbt";
        cnt.magic = ondisk::kBtcCrcMagic;
        FreeSpaceCounts cnt_counts;
        uint32_t last_count = 0;
        bool cnt_first = true;
        uint32_t cnt_blocks = 0;
        BtreeWalkCallbacks ccb;
        ccb.read_block = read_ag_block;
        ccb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
        ccb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                        const uint8_t* buf) -> Status {
          ++cnt_blocks;
          if (meta.level != 0) return OkStatus();
          for (uint16_t i = 0; i < meta.numrecs; ++i) {
            const uint8_t* rec = BtreeLeafRec(buf, meta, cnt, i);
            uint32_t start = GetBE32(rec);
            uint32_t len = GetBE32(rec + 4);
            if (len == 0 || start >= agf.length || len > agf.length - start) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "invalid cntbt record")
                  .Ag(agno)
                  .Metadata("cntbt")
                  .Actual(start);
            }
            if (!cnt_first && len < last_count) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "cntbt records out of order")
                  .Ag(agno)
                  .Metadata("cntbt")
                  .Expected(last_count)
                  .Actual(len);
            }
            cnt_first = false;
            last_count = len;
            cnt_counts.blocks += len;
            ++cnt_counts.records;
          }
          return OkStatus();
        };
        s = WalkBtree(cnt, agf.cnt_root, ccb);
        const bool cnt_ok = s.ok();
        if (!s.ok()) {
          report.Add("AG" + std::to_string(agno) + " cntbt",
                     CheckStatus::kCorrupt, s.error().Format());
        } else if (cnt_counts.blocks != bno.blocks ||
                   cnt_counts.records != bno.records) {
          report.Add("AG" + std::to_string(agno) + " cntbt",
                     CheckStatus::kCorrupt,
                     "cntbt total " + std::to_string(cnt_counts.blocks) +
                         " != bnobt total " + std::to_string(bno.blocks));
        }

        // agf_btreeblks is the number of *non-root* blocks of the free space
        // btrees plus, when the rmapbt is enabled, the non-root rmap blocks
        // (xchk_agf_xref_btreeblks and xfs_repair count exactly these; the
        // refcount tree is not part of it, see the comment in xfs_ag.c). The
        // root blocks are represented by the trees themselves, and the AGFL
        // blocks are tracked by agf_flcount instead.
        uint32_t rmap_blocks = 0;
        bool rmap_ok = true;
        if (live_sb.features.rmapbt) {
          rmap_seen = true;
          // The generic WalkBtree cannot decode the overlapping tree's
          // interleaved low/high key pairs. The typed reader also validates
          // both child keys, record ownership and the AGF block counter.
          auto tree = LoadRmapTree(dev, geo, agno, live_sb.lsn);
          if (!tree.ok()) {
            report.Add("AG" + std::to_string(agno) + " rmapbt",
                       CheckStatus::kCorrupt, tree.error().Format());
            rmap_ok = false;
            rmap_clean = false;
          } else {
            rmap_blocks = tree.value().blocks;
            report.Add("AG" + std::to_string(agno) + " rmapbt",
                       CheckStatus::kOk,
                       "level " + std::to_string(tree.value().level) + ", " +
                           std::to_string(rmap_blocks) + " blocks, " +
                           std::to_string(tree.value().records.size()) +
                           " records (structurally verified)");
          }
        }
        if (live_sb.features.reflink) {
          refcount_seen = true;
          auto tree = LoadRefcountTree(dev, geo, agno, live_sb.lsn);
          if (!tree.ok()) {
            report.Add("AG" + std::to_string(agno) + " refcountbt",
                       CheckStatus::kCorrupt, tree.error().Format());
            refcount_clean = false;
          } else {
            uint64_t cow_records = 0;
            uint64_t shared_records = 0;
            for (const RefcountRecord& rec : tree.value().records) {
              if (rec.cow()) {
                ++cow_records;
              } else {
                ++shared_records;
              }
            }
            refcount_cow_records += cow_records;
            report.Add("AG" + std::to_string(agno) + " refcountbt",
                       CheckStatus::kOk,
                       "level " + std::to_string(tree.value().level) + ", " +
                           std::to_string(tree.value().blocks) + " blocks, " +
                           std::to_string(shared_records) +
                           " shared + " + std::to_string(cow_records) +
                           " CoW records (structurally verified)");
            if (cow_records != 0) {
              report.Add("AG" + std::to_string(agno) + " CoW staging",
                         CheckStatus::kWarning,
                         std::to_string(cow_records) +
                             " orphan CoW staging extent(s); a writable mount "
                             "will recover them");
            }
          }
        }
        if (bno_ok && cnt_ok && rmap_ok) {
          const uint32_t counted_btreeblks =
              (bno_blocks - 1) + (cnt_blocks - 1) +
              (rmap_blocks > 0 ? rmap_blocks - 1 : 0);
          if (agf.btreeblks != counted_btreeblks) {
            report.Add("AG" + std::to_string(agno) + " btree blocks",
                       CheckStatus::kCorrupt,
                       "agf_btreeblks " + std::to_string(agf.btreeblks) +
                           " != counted " +
                           std::to_string(counted_btreeblks));
          }
        }

        // inobt.
        BtreeSpec inobt;
        inobt.name = "inobt";
        inobt.format = BtreeFormat::kShort;
        inobt.block_size = geo.block_size;
        inobt.key_size = bt::kInobtKeySize;
        inobt.ptr_size = bt::kInobtPtrSize;
        inobt.rec_size = bt::kInobtRecSize;
        inobt.magic = ondisk::kIbtCrcMagic;
        inobt.null_ptr = bt::kNullAgBlock;
        inobt.expected_owner = agno;
        inobt.owner_size = 4;
        inobt.uuid = geo.meta_uuid;
        inobt.sb_lsn = live_sb.lsn;
        inobt.ptr_to_daddr = bnobt.ptr_to_daddr;

        InodeCounts ino;
        std::map<uint32_t, InobtRecord> inobt_records;
        uint32_t inobt_blocks = 0;
        uint32_t last_startino = 0;
        bool ino_first = true;
        BtreeWalkCallbacks icb;
        icb.read_block = read_ag_block;
        icb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
        icb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                        const uint8_t* buf) -> Status {
          ++inobt_blocks;
          if (meta.level != 0) return OkStatus();
          for (uint16_t i = 0; i < meta.numrecs; ++i) {
            const uint8_t* rec = BtreeLeafRec(buf, meta, inobt, i);
            InobtRecord r;
            r.startino = GetBE32(rec);
            r.free = GetBE64(rec + 8);
            if (geo.features.spinodes) {
              r.holemask = GetBE16(rec + 4);
              r.count = GetU8(rec + 6);
              r.freecount = GetU8(rec + 7);
            } else {
              r.holemask = 0;
              r.count = ondisk::kInodesPerChunk;
              r.freecount = GetBE32(rec + 4);
            }
            if (static_cast<uint64_t>(r.startino) +
                        ondisk::kInodesPerChunk >
                    static_cast<uint64_t>(agf.length) * geo.inopblock) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "inobt record startino out of range")
                  .Ag(agno)
                  .Metadata("inobt")
                  .Actual(r.startino);
            }
            const uint64_t allocmask = geo.features.spinodes
                                           ? InobtAllocMask(r.holemask)
                                           : ~0ULL;
            if (geo.features.spinodes) {
              uint32_t chunk_count = 0;
              for (uint32_t biti = 0; biti < bt::kInobtHolemaskBits;
                   ++biti) {
                if (!(r.holemask & (1u << biti))) {
                  chunk_count += ondisk::kInodesPerHolemaskBit;
                }
              }
              if (r.count != chunk_count) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "sparse inode record count mismatch")
                    .Ag(agno)
                    .Metadata("inobt")
                    .Expected(chunk_count)
                    .Actual(r.count);
              }
            }
            uint32_t actual_free = 0;
            for (uint32_t ino_i = 0; ino_i < ondisk::kInodesPerChunk; ++ino_i) {
              if (!((allocmask >> ino_i) & 1ULL)) continue;
              if (r.free & (1ULL << ino_i)) ++actual_free;
            }
            if (actual_free != r.freecount) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "inobt freecount does not match the free mask")
                  .Ag(agno)
                  .Metadata("inobt")
                  .Expected(actual_free)
                  .Actual(r.freecount);
            }
            if (!ino_first && r.startino <= last_startino) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "inobt records out of order")
                  .Ag(agno)
                  .Metadata("inobt");
            }
            ino_first = false;
            last_startino = r.startino;
            ino.count += r.count;
            ino.freecount += r.freecount;
            ++ino.records;
            inobt_records.emplace(r.startino, r);
          }
          return OkStatus();
        };
        s = WalkBtree(inobt, agi.root, icb);
        if (!s.ok()) {
          report.Add("AG" + std::to_string(agno) + " inobt",
                     CheckStatus::kCorrupt, s.error().Format());
        } else if (ino.count != agi.count || ino.freecount != agi.freecount) {
          report.Add("AG" + std::to_string(agno) + " inobt",
                     CheckStatus::kCorrupt,
                     "inobt inodes " + std::to_string(ino.count) +
                         "/free " + std::to_string(ino.freecount) +
                         " != AGI " + std::to_string(agi.count) + "/" +
                         std::to_string(agi.freecount));
        } else {
          report.Add("AG" + std::to_string(agno) + " inobt",
                     CheckStatus::kOk,
                     std::to_string(ino.records) + " chunks, " +
                         std::to_string(ino.count) + " inodes");
        }

        // Sparse chunks: every backed block belongs to the inode chunk and
        // must not also be in the free-space tree; every backed inode image
        // must pass the inode verifier (hole slots have no such image).
        if (s.ok() && geo.features.spinodes) {
          auto overlaps_free = [&](uint32_t start, uint32_t len) {
            for (const auto& fr : free_ranges) {
              if (start < fr.first + fr.second && fr.first < start + len) {
                return true;
              }
            }
            return false;
          };
          uint32_t checked_inodes = 0;
          bool sparse_ok = true;
          for (const auto& entry : inobt_records) {
            const InobtRecord& r = entry.second;
            const uint32_t sagbno = r.startino >> geo.inopblog;
            uint32_t bit = 0;
            while (bit < bt::kInobtHolemaskBits) {
              if ((r.holemask >> bit) & 1u) {
                ++bit;
                continue;
              }
              const uint32_t run_start = bit;
              while (bit < bt::kInobtHolemaskBits &&
                     ((r.holemask >> bit) & 1u) == 0) {
                ++bit;
              }
              const uint32_t inode_offset =
                  run_start * ondisk::kInodesPerHolemaskBit;
              const uint32_t inode_count =
                  (bit - run_start) * ondisk::kInodesPerHolemaskBit;
              if (inode_offset % geo.inopblock != 0 ||
                  inode_count % geo.inopblock != 0) {
                report.Add("AG" + std::to_string(agno) + " sparse backing",
                           CheckStatus::kCorrupt,
                           "unaligned backing run in chunk " +
                               std::to_string(r.startino));
                sparse_ok = false;
                break;
              }
              const uint32_t agbno = sagbno + inode_offset / geo.inopblock;
              const uint32_t blocks = inode_count / geo.inopblock;
              if (overlaps_free(agbno, blocks)) {
                report.Add("AG" + std::to_string(agno) + " sparse backing",
                           CheckStatus::kCorrupt,
                           "backed inode block " + std::to_string(agbno) +
                               " is in the free-space tree");
                sparse_ok = false;
              }
              for (uint32_t i = 0; i < inode_count; ++i) {
                const uint64_t backed_ino =
                    geo.AginoToIno(agno, r.startino + inode_offset + i);
                std::vector<uint8_t> image(geo.inode_size);
                if (!dev.Read(geo.InoToByteOffset(backed_ino), image.data(),
                              image.size())) {
                  report.Add("AG" + std::to_string(agno) + " sparse inode",
                             CheckStatus::kCorrupt,
                             "cannot read inode " +
                                 std::to_string(backed_ino));
                  sparse_ok = false;
                  break;
                }
                XfsInode parsed;
                Status ps = ParseInode(image.data(), image.size(), geo,
                                       backed_ino, &parsed,
                                       /*allow_free_inode=*/true);
                if (!ps.ok()) {
                  report.Add("AG" + std::to_string(agno) + " sparse inode",
                             CheckStatus::kCorrupt,
                             "inode " + std::to_string(backed_ino) + ": " +
                                 ps.error().Format());
                  sparse_ok = false;
                  break;
                }
                ++checked_inodes;
              }
              if (!sparse_ok) break;
            }
            if (!sparse_ok) break;
          }
          if (sparse_ok) {
            report.Add("AG" + std::to_string(agno) + " sparse backing",
                       CheckStatus::kOk,
                       std::to_string(checked_inodes) +
                           " backed inode images verified, holes unowned");
          }
        }

        // finobt: a filtered view of the inobt with an independent record
        // lifecycle. Every finobt record must match its inobt record, refer
        // to a real chunk with free inodes, and the recorded free mask must
        // account for the hole mask.
        uint32_t finobt_blocks = 0;
        if (geo.features.finobt) {
          BtreeSpec fibt = inobt;
          fibt.name = "finobt";
          fibt.magic = ondisk::kFibtCrcMagic;
          std::map<uint32_t, InobtRecord> free_records;
          BtreeWalkCallbacks fcb;
          fcb.read_block = read_ag_block;
          fcb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
          fcb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                          const uint8_t* buf) -> Status {
            ++finobt_blocks;
            if (meta.level != 0) return OkStatus();
            for (uint16_t i = 0; i < meta.numrecs; ++i) {
              const uint8_t* rec = BtreeLeafRec(buf, meta, fibt, i);
              InobtRecord f;
              f.startino = GetBE32(rec);
              f.free = GetBE64(rec + 8);
              if (geo.features.spinodes) {
                f.holemask = GetBE16(rec + 4);
                f.count = GetU8(rec + 6);
                f.freecount = GetU8(rec + 7);
              } else {
                f.holemask = 0;
                f.count = ondisk::kInodesPerChunk;
                f.freecount = GetBE32(rec + 4);
              }
              auto it = inobt_records.find(f.startino);
              if (it == inobt_records.end()) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "finobt record has no inobt record")
                    .Ag(agno)
                    .Metadata("finobt")
                    .Actual(f.startino);
              }
              const InobtRecord& r = it->second;
              if (f.holemask != r.holemask || f.count != r.count ||
                  f.free != r.free || f.freecount != r.freecount) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "finobt record does not match the inobt")
                    .Ag(agno)
                    .Metadata("finobt")
                    .Actual(f.startino);
              }
              if (f.freecount == 0) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "finobt tracks a fully allocated chunk")
                    .Ag(agno)
                    .Metadata("finobt")
                    .Actual(f.startino);
              }
              free_records.emplace(f.startino, f);
            }
            return OkStatus();
          };
          s = WalkBtree(fibt, agi.free_root, fcb);
          if (!s.ok()) {
            report.Add("AG" + std::to_string(agno) + " finobt",
                       CheckStatus::kCorrupt, s.error().Format());
          } else {
            bool missing = false;
            for (const auto& entry : inobt_records) {
              if (entry.second.freecount == 0) continue;
              if (free_records.find(entry.first) == free_records.end()) {
                report.Add("AG" + std::to_string(agno) + " finobt",
                           CheckStatus::kCorrupt,
                           "chunk " + std::to_string(entry.first) +
                               " has free inodes but no finobt record");
                missing = true;
                break;
              }
            }
            if (!missing) {
              report.Add("AG" + std::to_string(agno) + " finobt",
                         CheckStatus::kOk,
                         std::to_string(free_records.size()) +
                             " free chunks match the inobt");
            }
          }
        }

        // inobtcount: the AGI counters are the reachable block counts,
        // including each tree's root.
        if (geo.features.inobtcnt) {
          const bool ib_ok = agi.iblocks == inobt_blocks;
          const bool fb_ok =
              !geo.features.finobt || agi.fblocks == finobt_blocks;
          if (!ib_ok || !fb_ok) {
            report.Add("AG" + std::to_string(agno) + " inode btree blocks",
                       CheckStatus::kCorrupt,
                       "AGI iblocks/fblocks " + std::to_string(agi.iblocks) +
                           "/" + std::to_string(agi.fblocks) +
                           " != reachable " + std::to_string(inobt_blocks) +
                           "/" + std::to_string(finobt_blocks));
          } else {
            report.Add("AG" + std::to_string(agno) + " inode btree blocks",
                       CheckStatus::kOk,
                       std::to_string(inobt_blocks) + "/" +
                           std::to_string(finobt_blocks) + " blocks");
          }
        }
      }
    }
  }

  if (rmap_seen) {
    report.Add("rmapbt", rmap_clean ? CheckStatus::kOk : CheckStatus::kCorrupt,
               rmap_clean
                   ? "all AG reverse-mapping trees structurally clean"
                   : "one or more AG reverse-mapping trees failed");
  }
  if (refcount_seen) {
    report.Add("refcountbt",
               refcount_clean ? CheckStatus::kOk : CheckStatus::kCorrupt,
               refcount_clean
                   ? "all AG reference-count trees structurally clean"
                   : "one or more AG reference-count trees failed");
    if (refcount_clean && refcount_cow_records == 0) {
      report.Add("CoW staging", CheckStatus::kOk,
                 "no orphan CoW staging extents");
    }
  }

  if (options.allocation_groups) {
    if (total_free != live_sb.fdblocks) {
      report.Add("superblock counters", CheckStatus::kWarning,
                 "sum of AG free blocks " + std::to_string(total_free) +
                     " != sb_fdblocks " + std::to_string(live_sb.fdblocks) +
                     (live_sb.features2 & ondisk::kVersion2LazySbCount
                          ? " (lazy counter)"
                          : ""));
    } else {
      report.Add("superblock counters", CheckStatus::kOk,
                 "free block counters match");
    }
    if (total_icount != live_sb.icount ||
        total_ifree != live_sb.ifree) {
      report.Add("superblock inode counters", CheckStatus::kWarning,
                 "AGI inode counts " + std::to_string(total_icount) + "/" +
                     std::to_string(total_ifree) + " != sb " +
                     std::to_string(live_sb.icount) + "/" +
                     std::to_string(live_sb.ifree));
    } else {
      report.Add("superblock inode counters", CheckStatus::kOk,
                 "inode counters match");
    }
  }

  // Root inode + root directory.
  if (options.root_directory) {
    auto root = OpenInode(geo.rootino);
    if (!root.ok()) {
      report.Add("root inode", CheckStatus::kCorrupt, root.error().Format());
    } else {
      report.Add("root inode", CheckStatus::kOk,
                 "inode " + std::to_string(geo.rootino) + " verified");
      auto root_attrs = ReadXattrFork(dev, geo, root.value(), live_sb.lsn);
      if (!root_attrs.ok()) {
        report.Add("root inode xattrs", CheckStatus::kCorrupt,
                   root_attrs.error().Format());
      } else {
        report.Add("root inode xattrs", CheckStatus::kOk,
                   std::to_string(root_attrs.value().entries.size()) +
                       " entries, format " +
                       [&] {
                         switch (root_attrs.value().format) {
                           case XattrForkFormat::kNone: return "none";
                           case XattrForkFormat::kShortform: return "shortform";
                           case XattrForkFormat::kLeaf: return "leaf";
                           case XattrForkFormat::kNode: return "node";
                           case XattrForkFormat::kExtentsEmpty:
                             return "empty-extents";
                         }
                         return "unknown";
                       }());
        Status root_attr_rmap = CheckAttrRmap(
            dev, geo, root.value(), root_attrs.value(), live_sb.lsn);
        if (!root_attr_rmap.ok()) {
          report.Add("root inode attr rmap", CheckStatus::kCorrupt,
                     root_attr_rmap.error().Format());
        }
      }
      auto entries = ListDirectory(geo.rootino);
      if (!entries.ok()) {
        report.Add("root directory", CheckStatus::kCorrupt,
                   entries.error().Format());
      } else {
        report.Add("root directory", CheckStatus::kOk,
                   std::to_string(entries.value().size()) + " entries");
        if (options.root_children) {
          uint32_t bad = 0;
          uint32_t checked_attrs = 0;
          for (const auto& e : entries.value()) {
            if (e.name == "." || e.name == "..") continue;
            auto inode = OpenInode(e.ino);
            if (!inode.ok()) {
              report.Add("root entry '" + e.name + "'", CheckStatus::kCorrupt,
                         inode.error().Format());
              ++bad;
              if (bad > 8) break;
              continue;
            }
            auto child_attrs =
                ReadXattrFork(dev, geo, inode.value(), live_sb.lsn);
            if (!child_attrs.ok()) {
              report.Add("root entry '" + e.name + "' xattrs",
                         CheckStatus::kCorrupt,
                         child_attrs.error().Format());
              ++bad;
            } else if (child_attrs.value().HasIncomplete()) {
              report.Add("root entry '" + e.name + "' xattrs",
                         CheckStatus::kWarning,
                         "attribute fork contains an INCOMPLETE entry");
            }
            if (child_attrs.ok()) {
              ++checked_attrs;
              Status rmap_check = CheckAttrRmap(
                  dev, geo, inode.value(), child_attrs.value(), live_sb.lsn);
              if (!rmap_check.ok()) {
                report.Add("root entry '" + e.name + "' attr rmap",
                           CheckStatus::kCorrupt,
                           rmap_check.error().Format());
                ++bad;
              }
            }
            uint64_t expected_blocks = 0;
            Status blocks = ExpectedInodeBlocks(dev, geo, inode.value(),
                                                &expected_blocks);
            if (!blocks.ok()) {
              report.Add("root entry '" + e.name + "' nblocks",
                         CheckStatus::kCorrupt, blocks.error().Format());
              ++bad;
            } else if (inode.value().nblocks != expected_blocks) {
              report.Add("root entry '" + e.name + "' nblocks",
                         CheckStatus::kCorrupt,
                         "di_nblocks " + std::to_string(inode.value().nblocks) +
                             " != data extents + bmbt blocks " +
                             std::to_string(expected_blocks));
              ++bad;
            }
            if (bad > 8) break;
          }
          if (bad <= 8 && checked_attrs > 0) {
            report.Add("root child xattrs", CheckStatus::kOk,
                       std::to_string(checked_attrs) +
                           " attribute forks and rmap owners verified");
          }
        }
      }
    }
  }

  // Parent-pointer namespace audit: every real dirent has exactly one reverse
  // record and every reverse record has exactly one real dirent.  This is a
  // second, independent view of the directory tree (the root_children check
  // above only validates direct children).
  if (options.parent_pointers && live_sb.features.parent) {
    auto parents = VerifyParentPointers();
    if (!parents.ok()) {
      report.Add("parent pointers", CheckStatus::kCorrupt,
                 parents.error().Format());
    } else {
      const ParentNamespaceReport& p = parents.value();
      std::string detail = p.Summary();
      if (!p.clean()) {
        auto append = [&](const char* label,
                          const std::vector<std::string>& v) {
          if (!v.empty()) detail += std::string("; ") + label + ": " + v[0];
        };
        append("missing", p.missing);
        append("unexpected", p.unexpected);
        append("wrong generation", p.wrong_parent_generation);
        append("wrong name", p.wrong_name);
        append("duplicate", p.duplicate);
        append("dangling parent", p.dangling_parent);
        append("non-directory parent", p.non_directory_parent);
        append("dotdot", p.wrong_dotdot);
        append("nlink", p.link_count_mismatch);
        append("malformed", p.malformed);
      }
      report.Add("parent pointers",
                 p.clean() ? CheckStatus::kOk : CheckStatus::kCorrupt, detail);
    }
  }

  // Log state: classified with the recovery engine, read-only. A dirty log is
  // a legal crash state, not corruption: it is reported with what a replay
  // would do (recoverable transactions, unsupported items) so the caller can
  // decide between `xfswin recover` and giving up.
  if (options.log) {
    auto journal = AnalyzeJournal();
    if (!journal.ok()) {
      report.Add("log", CheckStatus::kCorrupt, journal.error().Format());
    } else {
      const RecoverySummary& summary = journal.value();
      std::string message =
          std::string(ToString(summary.state)) + " - " + summary.detail;
      CheckStatus cs = summary.state == LogState::kUnknown
                           ? CheckStatus::kWarning
                           : CheckStatus::kOk;
      if (summary.needed) {
        message += "; recovery required: recoverable transactions=" +
                   std::to_string(summary.recoverable_transactions) +
                   ", items=" + std::to_string(summary.items);
        if (!summary.unsupported_items.empty()) {
          message += ", unsupported items=" +
                     std::to_string(summary.unsupported_items.size()) + " (" +
                     summary.unsupported_items.front() + ")";
        } else {
          message += " (recoverable by `xfswin recover` or a read-write mount)";
        }
      }
      report.Add("log", cs, message);
    }
  }

  return report;
}

}  // namespace xfs
