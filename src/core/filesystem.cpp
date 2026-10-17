#include "xfs/filesystem.h"

#include <algorithm>
#include <cstring>

#include "xfs/ag.h"
#include "xfs/btree.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/extent.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/symlink.h"
#include "xfs/verifier.h"

namespace xfs {
namespace {

namespace bt = ondisk::btree;
namespace di = ondisk::inode;
namespace sym = ondisk::symlink;

constexpr uint32_t kNullAgBlock = 0xffffffffu;

std::string Hex(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

}  // namespace

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

Result<std::unique_ptr<XfsFilesystem>> XfsFilesystem::Mount(
    std::unique_ptr<BlockDevice> device, const MountOptions& options) {
  if (!device) {
    return XFS_ERR(XfsError::kInvalidArgument, "no block device supplied");
  }
  auto sb_result = ReadSuperblock(*device, 0, options.verify_crc);
  if (!sb_result.ok()) return sb_result.error();
  XfsSuperblock sb = std::move(sb_result.value());

  Status mountable = CheckMountable(sb);
  if (!mountable.ok()) return mountable.error();

  XfsGeometry geo = MakeGeometry(sb);
  if (geo.dblocks * geo.block_size > device->Size()) {
    return XFS_ERR(XfsError::kCorruptSuperblock,
                   "filesystem larger than the device")
        .Expected(device->Size())
        .Actual(geo.dblocks * geo.block_size);
  }

  auto fs = std::unique_ptr<XfsFilesystem>(new XfsFilesystem());
  fs->superblock_ = std::move(sb);
  fs->geometry_ = std::move(geo);
  fs->options_ = options;
  fs->device_ = std::move(device);

  // Whitespace in the label: pad with spaces, not NULs (mkfs does this).
  XFS_SB_INFO("XFS v5 filesystem, uuid=" << FormatUuid(fs->superblock_.uuid));
  XFS_SB_INFO("blocksize=" << fs->geometry_.block_size
                           << " sectorsize=" << fs->geometry_.sector_size
                           << " inode size=" << fs->geometry_.inode_size);
  XFS_SB_INFO("agcount=" << fs->geometry_.agcount
                         << " agblocks=" << fs->geometry_.agblocks
                         << " dblocks=" << fs->geometry_.dblocks);
  XFS_SB_INFO("logstart=" << fs->geometry_.logstart
                          << " logblocks=" << fs->geometry_.logblocks);
  XFS_SB_INFO("root inode=" << fs->geometry_.rootino);

  if (fs->superblock_.features.ro_compat_unknown) {
    fs->warnings_.push_back(
        "superblock has unknown read-only compatible features (" +
        Hex(fs->superblock_.features.ro_compat_unknown) +
        "); read-only access proceeds");
  }
  if (fs->superblock_.features.compat_unknown) {
    fs->warnings_.push_back(
        "superblock has unknown compatible features (" +
        Hex(fs->superblock_.features.compat_unknown) + ")");
  }
  if (HasLogIncompatFeatures(fs->superblock_)) {
    fs->warnings_.push_back(
        "log-incompatible features are set; the log may contain records "
        "this build does not understand");
  }

  // Read the root inode before declaring success.
  auto root = fs->OpenInode(fs->geometry_.rootino);
  if (!root.ok()) return root.error();
  if (!root.value().IsDir()) {
    return XFS_ERR(XfsError::kCorruptInode, "root inode is not a directory")
        .Ino(fs->geometry_.rootino);
  }

  // Log cleanliness.
  if (options.check_log) {
    auto log = CheckLogClean(*fs->device_, fs->geometry_, fs->superblock_);
    if (!log.ok()) {
      if (!options.force_unsafe_ro) return log.error();
      fs->warnings_.push_back("log check failed: " + log.error().message);
    } else {
      const LogCheckResult& r = log.value();
      if (r.state == LogState::kClean || r.state == LogState::kEmpty) {
        XFS_SB_INFO("log is " << ToString(r.state) << " (" << r.detail << ")");
      } else {
        if (!options.force_unsafe_ro) {
          return XFS_ERR(XfsError::kDirtyLog,
                         "log is not clean (" + r.detail +
                             "); refusing to mount. The filesystem was not "
                             "cleanly unmounted or needs journal replay, "
                             "which this build does not implement. Use "
                             "--force-unsafe-ro to mount read-only anyway.")
              .Metadata("log");
        }
        fs->warnings_.push_back(
            "log is " + std::string(ToString(r.state)) + " (" + r.detail +
            "); mounting read-only with --force-unsafe-ro");
      }
    }
  }

  for (const auto& w : fs->warnings_) XFS_WARN(w);
  return fs;
}

Result<XfsInode> XfsFilesystem::OpenInode(uint64_t ino) const {
  auto inode = ReadInode(*device_, geometry_, ino);
  if (!inode.ok()) return inode.error();
  if (inode.value().OnUnlinkedList()) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "inode is on the unlinked list; reading it read-only is "
                   "not safe without journal replay")
        .Ino(ino)
        .Metadata("inode");
  }
  return inode;
}

Result<XfsInode> XfsFilesystem::LookupPath(std::string_view path) const {
  uint64_t ino = geometry_.rootino;
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();

  size_t pos = 0;
  while (pos < path.size()) {
    while (pos < path.size() && path[pos] == '/') ++pos;
    if (pos >= path.size()) break;
    size_t end = path.find('/', pos);
    if (end == std::string_view::npos) end = path.size();
    std::string_view component = path.substr(pos, end - pos);
    pos = end;

    if (component == ".") continue;
    if (!inode.value().IsDir()) {
      return XFS_ERR(XfsError::kNotDirectory, "path component is not a "
                                             "directory")
          .Ino(inode.value().ino);
    }
    auto entry = LookupName(inode.value().ino, component);
    if (!entry.ok()) return entry.error();
    auto next = OpenInode(entry.value().ino);
    if (!next.ok()) return next.error();
    inode = std::move(next);
  }
  return inode;
}

Result<std::vector<XfsDirEntry>> XfsFilesystem::ListDirectory(
    uint64_t ino) const {
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  if (!inode.value().IsDir()) {
    return XFS_ERR(XfsError::kNotDirectory, "inode is not a directory")
        .Ino(ino);
  }
  std::vector<XfsDirEntry> entries;
  Status s = xfs::ListDirectory(
      *device_, geometry_, inode.value(),
      [&](const XfsDirEntry& e) -> Status {
        entries.push_back(e);
        return OkStatus();
      });
  if (!s.ok()) return s.error();
  return entries;
}

Result<XfsDirEntry> XfsFilesystem::LookupName(uint64_t dir_ino,
                                              std::string_view name) const {
  auto inode = OpenInode(dir_ino);
  if (!inode.ok()) return inode.error();
  return xfs::LookupName(*device_, geometry_, inode.value(), name);
}

Result<size_t> XfsFilesystem::ReadFile(uint64_t ino, uint64_t offset,
                                       void* buffer, size_t length) const {
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  if (inode.value().IsDir()) {
    return XFS_ERR(XfsError::kNotRegularFile, "cannot ReadFile a directory")
        .Ino(ino);
  }
  return ReadFileData(*device_, geometry_, inode.value(), offset, buffer,
                      length);
}

Result<std::string> XfsFilesystem::ReadSymlink(uint64_t ino) const {
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  if (!inode.value().IsSymlink()) {
    return XFS_ERR(XfsError::kNotSymlink, "inode is not a symlink").Ino(ino);
  }
  return ReadSymlinkTarget(*device_, geometry_, inode.value());
}


Result<XfsFileInfo> XfsFilesystem::GetFileInfo(uint64_t ino) const {
  auto inode = OpenInode(ino);
  if (!inode.ok()) return inode.error();
  const XfsInode& i = inode.value();
  XfsFileInfo info;
  info.ino = i.ino;
  info.mode = i.mode;
  info.uid = i.uid;
  info.gid = i.gid;
  info.projid = i.projid;
  info.nlink = i.nlink;
  info.size = i.size;
  info.nblocks = i.nblocks;
  info.nextents = i.nextents;
  info.anextents = i.anextents;
  info.atime = i.atime;
  info.mtime = i.mtime;
  info.ctime = i.ctime;
  info.crtime = i.crtime;
  info.di_flags = i.flags;
  info.immutable = (i.flags & di::kDiFlagImmutable) != 0;
  info.append_only = (i.flags & di::kDiFlagAppend) != 0;
  return info;
}

// ---------------------------------------------------------------------------
// Consistency check
// ---------------------------------------------------------------------------

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

}  // namespace

CheckReport XfsFilesystem::CheckConsistency(const CheckOptions& options) const {
  CheckReport report;
  const XfsGeometry& geo = geometry_;
  BlockDevice& dev = *device_;

  report.Add("superblock", CheckStatus::kOk,
             "magic/uuid/geometry OK, crc " +
                 std::string(superblock_.crc_verified ? "verified" : "skipped"));

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
      bool same = s.uuid == superblock_.uuid && s.blocksize == superblock_.blocksize &&
                  s.dblocks == superblock_.dblocks &&
                  s.agblocks == superblock_.agblocks &&
                  s.agcount == superblock_.agcount &&
                  s.inodesize == superblock_.inodesize &&
                  s.rootino == superblock_.rootino &&
                  s.logstart == superblock_.logstart &&
                  s.logblocks == superblock_.logblocks &&
                  s.features_incompat == superblock_.features_incompat;
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
      total_free += agf.freeblks;
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
        bnobt.null_ptr = kNullAgBlock;
        bnobt.expected_owner = agno;
        bnobt.owner_size = 4;
        bnobt.uuid = geo.meta_uuid;
        bnobt.sb_lsn = superblock_.lsn;
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
        uint32_t last_end = 0;
        bool first = true;
        BtreeWalkCallbacks cb;
        cb.read_block = read_ag_block;
        cb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
        cb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                       const uint8_t* buf) -> Status {
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
            bno.blocks += len;
            bno.longest = std::max(bno.longest, len);
            ++bno.records;
          }
          return OkStatus();
        };
        Status s = WalkBtree(bnobt, agf.bno_root, cb);
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
        BtreeWalkCallbacks ccb;
        ccb.read_block = read_ag_block;
        ccb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
        ccb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                        const uint8_t* buf) -> Status {
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

        // inobt.
        BtreeSpec inobt;
        inobt.name = "inobt";
        inobt.format = BtreeFormat::kShort;
        inobt.block_size = geo.block_size;
        inobt.key_size = bt::kInobtKeySize;
        inobt.ptr_size = bt::kInobtPtrSize;
        inobt.rec_size = bt::kInobtRecSize;
        inobt.magic = ondisk::kIbtCrcMagic;
        inobt.null_ptr = kNullAgBlock;
        inobt.expected_owner = agno;
        inobt.owner_size = 4;
        inobt.uuid = geo.meta_uuid;
        inobt.sb_lsn = superblock_.lsn;
        inobt.ptr_to_daddr = bnobt.ptr_to_daddr;

        InodeCounts ino;
        uint32_t last_startino = 0;
        bool ino_first = true;
        BtreeWalkCallbacks icb;
        icb.read_block = read_ag_block;
        icb.ptr_in_range = [&](uint64_t ptr) { return ptr < agf.length; };
        icb.visit = [&](uint64_t, const BtreeBlockMeta& meta,
                        const uint8_t* buf) -> Status {
          if (meta.level != 0) return OkStatus();
          for (uint16_t i = 0; i < meta.numrecs; ++i) {
            const uint8_t* rec = BtreeLeafRec(buf, meta, inobt, i);
            uint32_t startino = GetBE32(rec);
            uint64_t free_mask = GetBE64(rec + 8);
            if (geo.features.spinodes) {
              uint16_t holemask = GetBE16(rec + 4);
              uint8_t count = GetU8(rec + 6);
              uint8_t freecount = GetU8(rec + 7);
              uint32_t chunk_count = 0;
              for (int biti = 0; biti < 16; ++biti) {
                if (!(holemask & (1u << biti))) chunk_count += 4;
              }
              if (count != chunk_count) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "sparse inode record count mismatch")
                    .Ag(agno)
                    .Metadata("inobt")
                    .Expected(chunk_count)
                    .Actual(count);
              }
              uint32_t actual_free = 0;
              for (int ino_i = 0; ino_i < 64; ++ino_i) {
                bool hole = false;
                if (geo.features.spinodes) {
                  uint16_t holemask2 = GetBE16(rec + 4);
                  hole = (holemask2 & (1u << (ino_i / 4))) != 0;
                }
                if (hole) continue;
                if (free_mask & (1ULL << ino_i)) ++actual_free;
              }
              if (actual_free != freecount) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "sparse inode freecount mismatch")
                    .Ag(agno)
                    .Metadata("inobt")
                    .Expected(actual_free)
                    .Actual(freecount);
              }
              ino.count += count;
              ino.freecount += freecount;
            } else {
              uint32_t freecount = GetBE32(rec + 4);
              uint32_t actual_free = 0;
              for (int ino_i = 0; ino_i < 64; ++ino_i) {
                if (free_mask & (1ULL << ino_i)) ++actual_free;
              }
              if (actual_free != freecount) {
                return XFS_ERR(XfsError::kCorruptBtree,
                               "inobt freecount does not match mask")
                    .Ag(agno)
                    .Metadata("inobt")
                    .Expected(actual_free)
                    .Actual(freecount);
              }
              ino.count += 64;
              ino.freecount += freecount;
            }
            if (!ino_first && startino <= last_startino) {
              return XFS_ERR(XfsError::kCorruptBtree,
                             "inobt records out of order")
                  .Ag(agno)
                  .Metadata("inobt");
            }
            ino_first = false;
            last_startino = startino;
            ++ino.records;
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
      }
    }
  }

  if (options.allocation_groups) {
    if (total_free != superblock_.fdblocks) {
      report.Add("superblock counters", CheckStatus::kWarning,
                 "sum of AG free blocks " + std::to_string(total_free) +
                     " != sb_fdblocks " + std::to_string(superblock_.fdblocks) +
                     (superblock_.features2 & ondisk::kVersion2LazySbCount
                          ? " (lazy counter)"
                          : ""));
    } else {
      report.Add("superblock counters", CheckStatus::kOk,
                 "free block counters match");
    }
    if (total_icount != superblock_.icount ||
        total_ifree != superblock_.ifree) {
      report.Add("superblock inode counters", CheckStatus::kWarning,
                 "AGI inode counts " + std::to_string(total_icount) + "/" +
                     std::to_string(total_ifree) + " != sb " +
                     std::to_string(superblock_.icount) + "/" +
                     std::to_string(superblock_.ifree));
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
      auto entries = ListDirectory(geo.rootino);
      if (!entries.ok()) {
        report.Add("root directory", CheckStatus::kCorrupt,
                   entries.error().Format());
      } else {
        report.Add("root directory", CheckStatus::kOk,
                   std::to_string(entries.value().size()) + " entries");
        if (options.root_children) {
          uint32_t bad = 0;
          for (const auto& e : entries.value()) {
            if (e.name == "." || e.name == "..") continue;
            auto inode = OpenInode(e.ino);
            if (!inode.ok()) {
              report.Add("root entry '" + e.name + "'", CheckStatus::kCorrupt,
                         inode.error().Format());
              ++bad;
              if (bad > 8) break;
            }
          }
        }
      }
    }
  }

  // Log state.
  if (options.log) {
    auto log = CheckLogClean(dev, geo, superblock_);
    if (!log.ok()) {
      report.Add("log", CheckStatus::kCorrupt, log.error().Format());
    } else {
      LogState state = log.value().state;
      CheckStatus cs = state == LogState::kUnknown ? CheckStatus::kWarning
                                                   : CheckStatus::kOk;
      report.Add("log", cs,
                 std::string(ToString(state)) + " - " + log.value().detail);
    }
  }

  return report;
}

}  // namespace xfs
