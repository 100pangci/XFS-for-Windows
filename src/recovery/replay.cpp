// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - log replay (RW-5 pass1/pass2).
//
// Ports the write side of the kernel's recovery:
//
//   xlog_do_log_recovery        pass1 then pass2 over the replayed window
//   xlog_recover_buf_commit_pass1/2  buffer cancellation table
//   xlog_recover_do_reg_buffer  bitmap-guided patch of logged chunks
//   xlog_recover_do_inode_buffer     di_next_unlinked-only inode buffers
//   xlog_recover_inode_commit_pass2  logged inode core/forks
//   xlog_recover_icreate_commit_pass2  xfs_ialloc_inode_init rebuild
//   xlog_recover_validate_buf_type   post-patch type verification
//   xlog_recover_get_buf_lsn         metadata LSN rule (skip older replay)
//   xlog_unpack / xfs_buf write verifiers  LSN + CRC stamping
//   xlog_clear_stale_blocks     clear partial writes in front of the head
//
// Metadata is patched directly, never through a high-level mutation API and
// never by writing new log records: the log itself stays authoritative until
// the replay has completely finished. Buffers are held in a small cache and
// flushed when the recovery LSN changes (xfs_buf_delwri_submit semantics), so
// items of one transaction see each other's changes while the on-disk LSN
// rule keeps working.
#include "xfs/recovery.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "xfs/crc32c.h"
#include "xfs/crash.h"
#include "xfs/endian.h"
#include "xfs/ialloc.h"
#include "xfs/inode.h"
#include "xfs/log.h"
#include "xfs/log_format.h"
#include "xfs/ondisk.h"
#include "xfs/verifier.h"

namespace xfs {
namespace {

namespace lf = logfmt;
namespace di = ondisk::inode;

uint64_t LoadLE64(const uint8_t* p) {
  return static_cast<uint64_t>(LoadLE32(p)) |
         (static_cast<uint64_t>(LoadLE32(p + 4)) << 32);
}

// XFS_LSN_CMP: cycle first, then block.
int CompareLsn(uint64_t a, uint64_t b) {
  const uint32_t ca = static_cast<uint32_t>(a >> 32);
  const uint32_t cb = static_cast<uint32_t>(b >> 32);
  if (ca != cb) return ca < cb ? -1 : 1;
  const uint32_t ba = static_cast<uint32_t>(a & 0xffffffffu);
  const uint32_t bb = static_cast<uint32_t>(b & 0xffffffffu);
  if (ba != bb) return ba < bb ? -1 : 1;
  return 0;
}

uint64_t MakeLsn(uint32_t cycle, uint32_t block) {
  return (static_cast<uint64_t>(cycle) << 32) | block;
}

constexpr uint64_t kNullAgino = 0xffffffffu;

// ---------------------------------------------------------------------------
// Metadata types (xlog_recover_validate_buf_type's table)
// ---------------------------------------------------------------------------
// The magic decides how a recovered buffer is stamped: every v5 metadata block
// carries its LSN at a type-specific offset and its CRC at another; the write
// verifiers stamp the LSN and recompute the CRC at flush time. `no_stamp`
// types (inode clusters) carry per-object CRCs and are handled by their own
// replay path.
struct MetaType {
  const char* name = "unknown";
  size_t lsn_offset = 0;
  size_t crc_offset = 0;
  bool stamp = true;  // stamp LSN + recompute CRC at flush
};

bool ClassifyMetadata(const uint8_t* buf, size_t len, MetaType* out) {
  const uint32_t magic = GetBE32(buf);
  switch (magic) {
    case ondisk::kSbMagic:
      *out = {"superblock", ondisk::sb::kLsn, ondisk::sb::kCrc, true};
      return true;
    case ondisk::kAgfMagic:
      *out = {"AGF", ondisk::agf::kLsn, ondisk::agf::kCrc, true};
      return true;
    case ondisk::kAgiMagic:
      *out = {"AGI", ondisk::agi::kLsn, ondisk::agi::kCrc, true};
      return true;
    case ondisk::kAgflMagic:
      *out = {"AGFL", ondisk::agfl::kLsn, ondisk::agfl::kCrc, true};
      return true;
    case ondisk::kBtbCrcMagic:
      *out = {"bnobt", ondisk::btree::kSLsn, ondisk::btree::kSCrc, true};
      return true;
    case ondisk::kBtcCrcMagic:
      *out = {"cntbt", ondisk::btree::kSLsn, ondisk::btree::kSCrc, true};
      return true;
    case ondisk::kIbtCrcMagic:
      *out = {"inobt", ondisk::btree::kSLsn, ondisk::btree::kSCrc, true};
      return true;
    case ondisk::kFibtCrcMagic:
      *out = {"finobt", ondisk::btree::kSLsn, ondisk::btree::kSCrc, true};
      return true;
    case ondisk::kBmapCrcMagic:
      *out = {"bmbt", ondisk::btree::kLLsn, ondisk::btree::kLCrc, true};
      return true;
    case ondisk::kDir3BlockMagic:
    case ondisk::kDir3DataMagic:
    case ondisk::kDir3FreeMagic:
      *out = {"directory block", 16, 4, true};
      return true;
    case ondisk::kSymlinkMagic:
      *out = {"symlink block", ondisk::symlink::kLsn, ondisk::symlink::kCrc,
              true};
      return true;
    default:
      break;
  }
  if (GetBE16(buf) == ondisk::kDinodeMagic) {
    // Inode clusters: per-inode LSN/CRC, never stamped as a whole buffer.
    *out = {"inode cluster", kNoField, kNoField, false};
    return true;
  }
  const uint16_t da_magic = GetBE16(buf + 8);
  switch (da_magic) {
    case ondisk::kDir3Leaf1Magic:
    case ondisk::kDir3LeafnMagic:
    case ondisk::kDaNode3Magic:
      *out = {"directory leaf/node", 24, 12, true};
      return true;
    default:
      break;
  }
  (void)len;
  return false;
}

// xlog_recover_get_buf_lsn(): the on-disk LSN of a metadata buffer based on
// its current magic. -1 means "not recognised / belongs to another filesystem
// / no LSN" and the item must be replayed.
int64_t OnDiskMetadataLsn(const uint8_t* buf, size_t len, const Uuid& meta_uuid,
                          const MetaType& type) {
  if (!type.stamp) return -1;  // inode buffers recover immediately (no LSN)
  switch (GetBE32(buf)) {
    case ondisk::kSbMagic:
      if (std::memcmp(buf + ondisk::sb::kMetaUuid, meta_uuid.data(), 16) != 0) {
        return -1;
      }
      return static_cast<int64_t>(GetBE64(buf, ondisk::sb::kLsn));
    case ondisk::kAgfMagic:
      if (std::memcmp(buf + ondisk::agf::kUuid, meta_uuid.data(), 16) != 0) {
        return -1;
      }
      return static_cast<int64_t>(GetBE64(buf, ondisk::agf::kLsn));
    case ondisk::kAgiMagic:
      if (std::memcmp(buf + ondisk::agi::kUuid, meta_uuid.data(), 16) != 0) {
        return -1;
      }
      return static_cast<int64_t>(GetBE64(buf, ondisk::agi::kLsn));
    case ondisk::kAgflMagic:
      if (std::memcmp(buf + ondisk::agfl::kUuid, meta_uuid.data(), 16) != 0) {
        return -1;
      }
      return static_cast<int64_t>(GetBE64(buf, ondisk::agfl::kLsn));
    default:
      break;
  }
  if (len < 56) return -1;
  if (GetBE16(buf) == ondisk::kDinodeMagic) return -1;
  (void)meta_uuid;
  // The btree/da/symlink headers all carry a UUID; a mismatch means the block
  // predates this filesystem instance and must be recovered over.
  const size_t uuid_off = [&]() -> size_t {
    switch (GetBE32(buf)) {
      case ondisk::kBtbCrcMagic:
      case ondisk::kBtcCrcMagic:
      case ondisk::kIbtCrcMagic:
      case ondisk::kFibtCrcMagic:
        return ondisk::btree::kSUuid;
      case ondisk::kBmapCrcMagic:
        return ondisk::btree::kLUuid;
      case ondisk::kDir3BlockMagic:
      case ondisk::kDir3DataMagic:
      case ondisk::kDir3FreeMagic:
        return 24;
      case ondisk::kSymlinkMagic:
        return ondisk::symlink::kUuid;
      default:
        if (GetBE16(buf + 8) == ondisk::kDir3Leaf1Magic ||
            GetBE16(buf + 8) == ondisk::kDir3LeafnMagic ||
            GetBE16(buf + 8) == ondisk::kDaNode3Magic) {
          return 32;
        }
        return kNoField;
    }
  }();
  if (uuid_off != kNoField && uuid_off + 16 <= len &&
      std::memcmp(buf + uuid_off, meta_uuid.data(), 16) != 0) {
    return -1;
  }
  return type.stamp ? static_cast<int64_t>(GetBE64(buf, type.lsn_offset)) : -1;
}

// ---------------------------------------------------------------------------
// Recovery buffer cache (xfs_buf_delwri queue + drain per recovery LSN)
// ---------------------------------------------------------------------------
class ReplayBuffers {
 public:
  ReplayBuffers(BlockDevice& dev, const Uuid& meta_uuid)
      : dev_(dev), meta_uuid_(meta_uuid) {}

  struct Key {
    uint64_t daddr = 0;  // 512-byte units
    uint32_t len = 0;    // 512-byte units
    bool operator<(const Key& other) const {
      return daddr != other.daddr ? daddr < other.daddr : len < other.len;
    }
  };

  // Gets the buffer at (daddr, len), from the pending set or from disk. An
  // overlapping pending buffer with a different range can only be flushed
  // when its recovery LSN is older than the caller's.
  Result<std::vector<uint8_t>*> Get(Key key, uint64_t current_lsn) {
    auto exact = pending_.find(key);
    if (exact != pending_.end()) return &exact->second.data;
    for (const auto& entry : pending_) {
      if (Overlaps(entry.first, key)) {
        if (have_lsn_ && CompareLsn(lsn_, current_lsn) < 0) {
          Status flushed = FlushLocked();
          if (!flushed.ok()) return flushed.error();
          return Get(key, current_lsn);
        }
        return XFS_ERR(XfsError::kUnsupportedFeature,
                       "overlapping recovery buffers with the same LSN")
            .Expected(entry.first.daddr)
            .Actual(key.daddr)
            .Metadata("log replay");
      }
    }
    PendingBuffer buffer;
    buffer.data.assign(static_cast<size_t>(key.len) * 512, 0);
    const uint64_t offset = key.daddr * 512;
    if (!dev_.Read(offset, buffer.data.data(), buffer.data.size())) {
      return XFS_ERR(XfsError::kIoError,
                     "failed to read a metadata buffer during replay")
          .Offset(offset)
          .Metadata("log replay");
    }
    auto inserted = pending_.emplace(key, std::move(buffer));
    return &inserted.first->second.data;
  }

  void MarkDirty(Key key) {
    auto it = pending_.find(key);
    if (it != pending_.end()) it->second.dirty = true;
  }

  bool IsPending(Key key) const { return pending_.count(key) != 0; }

  // Starts processing a transaction: buffers held under another recovery LSN
  // are written out first (xlog_recover_process_ophdr's per-commit drain).
  Status BeginLsn(uint64_t lsn) {
    if (have_lsn_ && lsn_ != lsn) {
      Status flushed = FlushLocked();
      if (!flushed.ok()) return flushed;
    }
    have_lsn_ = true;
    lsn_ = lsn;
    return OkStatus();
  }

  Status Flush() { return FlushLocked(); }

  // Removes pending (clean) entries; used between the metadata flush and the
  // log-state transition if a caller re-reads buffers.
  void Clear() { pending_.clear(); }

  uint64_t bytes_written() const { return bytes_written_; }

 private:
  struct PendingBuffer {
    std::vector<uint8_t> data;
    bool dirty = false;
  };

  static bool Overlaps(const Key& a, const Key& b) {
    return a.daddr < b.daddr + b.len && b.daddr < a.daddr + a.len;
  }

  Status FlushLocked() {
    Status error = OkStatus();
    for (auto& entry : pending_) {
      if (!entry.second.dirty) continue;
      std::vector<uint8_t>& data = entry.second.data;
      MetaType type;
      if (!ClassifyMetadata(data.data(), data.size(), &type)) {
        return XFS_ERR(XfsError::kUnsupportedFeature,
                       "cannot replay an unrecognised metadata buffer")
            .Offset(entry.first.daddr * 512)
            .Metadata("log replay");
      }
      if (type.stamp) {
        PutBE64(data.data(), type.lsn_offset, lsn_);
        UpdateXfsChecksum(data.data(), data.size(), type.crc_offset);
      }
      const uint64_t offset = entry.first.daddr * 512;
      MaybeCrash(CrashPoint::kRecoveryMidBuffer,
                 std::string(type.name) + " daddr=" + std::to_string(entry.first.daddr));
      if (!dev_.Write(offset, data.data(), data.size())) {
        return XFS_ERR(XfsError::kIoError,
                       "failed to write a replayed metadata buffer")
            .Offset(offset)
            .Metadata("log replay");
      }
      bytes_written_ += data.size();
    }
    if (!pending_.empty() && !dev_.Flush()) {
      return XFS_ERR(XfsError::kIoError,
                     "failed to flush replayed metadata")
          .Metadata("log replay");
    }
    pending_.clear();
    return OkStatus();
  }

  BlockDevice& dev_;
  Uuid meta_uuid_;
  std::map<Key, PendingBuffer> pending_;
  bool have_lsn_ = false;
  uint64_t lsn_ = 0;
  uint64_t bytes_written_ = 0;
};

// ---------------------------------------------------------------------------
// Buffer cancellation table (xlog_recover_buf_commit_pass1 / buf_cancel_table)
// ---------------------------------------------------------------------------
class CancelTable {
 public:
  void Add(uint64_t blkno, uint32_t len) {
    auto key = std::make_pair(blkno, len);
    ++counts_[key];
  }
  bool IsCancelled(uint64_t blkno, uint32_t len) const {
    return counts_.count(std::make_pair(blkno, len)) != 0;
  }
  // xlog_put_buffer_cancelled(): releases one reference; the entry disappears
  // when its last occurrence has been consumed.
  void Release(uint64_t blkno, uint32_t len) {
    auto it = counts_.find(std::make_pair(blkno, len));
    if (it == counts_.end()) return;
    if (--it->second <= 0) counts_.erase(it);
  }
  // xlog_check_buf_cancel_table(): leftover entries are a log inconsistency.
  size_t Size() const { return counts_.size(); }

 private:
  std::map<std::pair<uint64_t, uint32_t>, int> counts_;
};

// ---------------------------------------------------------------------------
// Item replay
// ---------------------------------------------------------------------------
struct BufLogFormat {
  uint16_t flags = 0;
  uint16_t len = 0;
  int64_t blkno = 0;
  uint32_t map_size = 0;
  const uint8_t* map = nullptr;
};

Result<BufLogFormat> ParseBufFormat(const std::vector<uint8_t>& region) {
  if (region.size() < lf::kBufLogFormatSize) {
    return XFS_ERR(XfsError::kCorruptBtree, "buffer log item format too small")
        .Actual(region.size())
        .Metadata("log item");
  }
  BufLogFormat out;
  out.flags = LoadLE16(region.data() + 4);
  out.len = LoadLE16(region.data() + 6);
  out.blkno = static_cast<int64_t>(LoadLE32(region.data() + 8)) |
              (static_cast<int64_t>(static_cast<uint32_t>(
                   LoadLE32(region.data() + 12)))
               << 32);
  out.map_size = LoadLE32(region.data() + 16);
  out.map = region.data() + lf::kBufMapOffset;
  if (out.map_size == 0 || out.map_size > 64 ||
      lf::kBufMapOffset + static_cast<size_t>(out.map_size) * 4 >
          region.size()) {
    return XFS_ERR(XfsError::kCorruptBtree, "bad buffer log item bitmap size")
        .Actual(out.map_size)
        .Metadata("log item");
  }
  if (out.len == 0 || out.blkno < 0) {
    return XFS_ERR(XfsError::kCorruptBtree, "bad buffer log item geometry")
        .Expected(out.len)
        .Actual(out.blkno)
        .Metadata("log item");
  }
  return out;
}

// Counts contiguous runs of set bits, one per logged region (what the writer
// put in the log).
uint32_t CountRuns(const uint8_t* map, uint32_t map_size) {
  uint32_t runs = 0;
  bool in_run = false;
  const uint32_t bits = map_size * 32;
  for (uint32_t bit = 0; bit < bits; ++bit) {
    const bool set = (map[bit / 8] >> (bit % 8)) & 1u;
    if (set && !in_run) ++runs;
    in_run = set;
  }
  return runs;
}

// xlog_recover_do_reg_buffer(): copies every logged region into the buffer at
// the bitmap-driven offset. The regions may be shorter than the full run
// (the kernel splits them at page boundaries), in which case the run is
// trimmed to the region length.
Status DoRegBuffer(const RecoveredItem& item, const BufLogFormat& format,
                   std::vector<uint8_t>* buffer, const char* what) {
  const size_t buffer_bytes = buffer->size();
  const uint32_t bits = format.map_size * 32;
  size_t region_index = 1;  // region 0 is the format header
  uint32_t bit = 0;
  while (true) {
    // xfs_next_bit.
    while (bit < bits && ((format.map[bit / 8] >> (bit % 8)) & 1u) == 0) ++bit;
    if (bit >= bits) break;
    // xfs_contig_bits.
    uint32_t nbits = 0;
    while (bit + nbits < bits &&
           ((format.map[(bit + nbits) / 8] >> ((bit + nbits) % 8)) & 1u) != 0) {
      ++nbits;
    }
    if (region_index >= item.regions.size()) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "buffer log item has fewer regions than bitmap runs")
          .Metadata("log item");
    }
    const size_t region_len = item.regions[region_index].size();
    if (region_len == 0 || region_len % lf::kBufChunkSize != 0) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "buffer log item region is not a chunk multiple")
          .Actual(region_len)
          .Metadata("log item");
    }
    if ((static_cast<uint64_t>(bit) << lf::kBufChunkShift) +
            (static_cast<uint64_t>(nbits) << lf::kBufChunkShift) >
        buffer_bytes) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "buffer log item bitmap exceeds the buffer")
          .Metadata("log item");
    }
    if (region_len < (static_cast<size_t>(nbits) << lf::kBufChunkShift)) {
      nbits = static_cast<uint32_t>(region_len >> lf::kBufChunkShift);
    }
    std::memcpy(buffer->data() + (static_cast<size_t>(bit) << lf::kBufChunkShift),
                item.regions[region_index].data(),
                static_cast<size_t>(nbits) << lf::kBufChunkShift);
    bit += nbits;
    ++region_index;
  }
  if (region_index != item.regions.size()) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "buffer log item has more regions than bitmap runs")
        .Expected(region_index)
        .Actual(item.regions.size())
        .Metadata("log item");
  }
  (void)what;
  return OkStatus();
}

// xlog_recover_do_inode_buffer(): for buffers full of inodes only the
// di_next_unlinked fields are recovered, then the inode CRC is recomputed.
Status DoInodeBuffer(const RecoveredItem& item, const BufLogFormat& format,
                     std::vector<uint8_t>* buffer, const XfsGeometry& geo) {
  const uint32_t inodes_per_buf =
      static_cast<uint32_t>(buffer->size() / geo.inode_size);
  const uint32_t bits = format.map_size * 32;
  uint32_t bit = 0;
  uint32_t nbits = 0;
  uint64_t reg_offset = 0;
  uint64_t reg_bytes = 0;
  size_t region_index = 0;

  for (uint32_t i = 0; i < inodes_per_buf; ++i) {
    const uint64_t next_unlinked_offset =
        static_cast<uint64_t>(i) * geo.inode_size + di::kNextUnlinked;
    while (next_unlinked_offset >= reg_offset + reg_bytes) {
      bit += nbits;
      // xfs_next_bit.
      while (bit < bits && ((format.map[bit / 8] >> (bit % 8)) & 1u) == 0) {
        ++bit;
      }
      if (bit >= bits) return OkStatus();  // no more logged regions
      nbits = 0;
      while (bit + nbits < bits &&
             ((format.map[(bit + nbits) / 8] >> ((bit + nbits) % 8)) & 1u) !=
                 0) {
        ++nbits;
      }
      reg_offset = static_cast<uint64_t>(bit) << lf::kBufChunkShift;
      reg_bytes = static_cast<uint64_t>(nbits) << lf::kBufChunkShift;
      ++region_index;  // region 0 is the format header
    }
    if (next_unlinked_offset < reg_offset) continue;
    if (region_index >= item.regions.size()) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "inode buffer log item region underrun")
          .Metadata("log item");
    }
    const std::vector<uint8_t>& region = item.regions[region_index];
    const uint64_t at = next_unlinked_offset - reg_offset;
    if (at + 4 > region.size()) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "inode buffer log item region too small")
          .Metadata("log item");
    }
    const uint32_t logged_next = GetBE32(region.data(), at);
    if (logged_next == 0) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "inode buffer log item replays a null di_next_unlinked")
          .Metadata("log item");
    }
    PutBE32(buffer->data(), next_unlinked_offset, logged_next);
    // xfs_dinode_calc_crc of the whole inode.
    UpdateXfsChecksum(buffer->data() + static_cast<size_t>(i) * geo.inode_size,
                      geo.inode_size, di::kCrc);
  }
  return OkStatus();
}

// Verifies a metadata buffer that is being skipped because the on-disk copy
// is newer (the kernel runs the read verifier here).
Status VerifySkippedBuffer(const std::vector<uint8_t>& buffer,
                           const MetaType& type, const Uuid& meta_uuid) {
  if (type.stamp &&
      !VerifyXfsChecksum(buffer.data(), buffer.size(), type.crc_offset)) {
    return XFS_ERR(XfsError::kBadCrc,
                   std::string("skipped ") + type.name + " has a bad CRC")
        .Metadata("log replay");
  }
  (void)meta_uuid;
  return OkStatus();
}

// Copies one logged inode core (host order) into the on-disk inode (big
// endian), stamping the recovery LSN: xfs_log_dinode_to_disk().
void LogDinodeToDisk(const uint8_t* log, size_t log_size, uint8_t* disk,
                     size_t inode_size, uint64_t lsn) {
  auto put16 = [&](size_t off) {
    if (off + 2 <= log_size) PutBE16(disk, off, LoadLE16(log + off));
  };
  auto put32 = [&](size_t off) {
    if (off + 4 <= log_size) PutBE32(disk, off, LoadLE32(log + off));
  };
  auto put64 = [&](size_t off) {
    if (off + 8 <= log_size) {
      // Little-endian (log) -> big-endian (disk): the high word goes first.
      StoreBE32(disk + off, static_cast<uint32_t>(LoadLE32(log + off + 4)));
      StoreBE32(disk + off + 4, static_cast<uint32_t>(LoadLE32(log + off)));
    }
  };
  const uint8_t version = log_size > di::kVersion ? log[di::kVersion] : 0;
  const bool bigtime =
      version >= 3 && log_size >= di::kFlags2 + 8 &&
      (LoadLE64(log + di::kFlags2) & di::kDiFlag2Bigtime) != 0;
  const bool nrext64 =
      version >= 3 && log_size >= di::kFlags2 + 8 &&
      (LoadLE64(log + di::kFlags2) & di::kDiFlag2Nrext64) != 0;

  put16(di::kMagic);
  put16(di::kMode);
  if (log_size > di::kVersion) disk[di::kVersion] = log[di::kVersion];
  if (log_size > di::kFormat) disk[di::kFormat] = log[di::kFormat];
  put16(di::kOnlink);  // di_metatype in this kernel generation
  put32(di::kUid);
  put32(di::kGid);
  put32(di::kNlink);
  put16(di::kProjidLo);
  put16(di::kProjidHi);
  // Timestamps: legacy 32:32 pairs, or a single nanoseconds-since-epoch be64
  // for bigtime inodes (xfs_log_dinode_to_disk_ts).
  auto timestamp = [&](size_t off) {
    if (off + 8 > log_size) return;
    if (bigtime) {
      // A single nanoseconds-since-epoch be64.
      StoreBE32(disk + off, LoadLE32(log + off + 4));
      StoreBE32(disk + off + 4, LoadLE32(log + off));
    } else {
      StoreBE32(disk + off, LoadLE32(log + off));
      StoreBE32(disk + off + 4, LoadLE32(log + off + 4));
    }
  };
  timestamp(di::kAtime);
  timestamp(di::kMtime);
  timestamp(di::kCtime);
  put64(di::kSize);
  put64(di::kNblocks);
  put32(di::kExtsize);
  if (version >= 3) {
    if (nrext64) {
      put64(di::kBigNextents);
      put32(di::kBigAnextents);
      put16(di::kAnextents);  // di_nrext64_pad shares the anextents slot
    } else {
      StoreBE64(disk + di::kV3Pad, 0);
      put32(di::kNextents);
      put16(di::kAnextents);
    }
  } else {
    // v2 inode: flushiter lives in the pad union and is reset.
    if (log_size > di::kOnlink) {
      PutBE16(disk, di::kOnlink, 0);  // di_v2_pad6 + flushiter = 0
    }
    put32(di::kNextents);
    put16(di::kAnextents);
  }
  if (log_size > di::kForkoff) disk[di::kForkoff] = log[di::kForkoff];
  if (log_size > di::kAformat) {
    disk[di::kAformat] = log[di::kAformat];
  }
  put32(di::kDmevmask);
  put16(di::kDmstate);
  put16(di::kFlags);
  put32(di::kGen);
  if (version >= 3) {
    put64(di::kChangecount);
    put64(di::kFlags2);
    put32(di::kCowextsize);
    timestamp(di::kCrtime);
    put64(di::kIno);
    if (di::kUuid + 16 <= log_size && di::kUuid + 16 <= inode_size) {
      std::memcpy(disk + di::kUuid, log + di::kUuid, 16);
    }
    std::memset(disk + di::kPad2, 0, 12);
    PutBE64(disk, di::kLsn, lsn);
  }
}

// xfs_bmbt_to_bmdr(): the logged data-fork btree root is a full btree block;
// the inode fork stores the short-form header plus short pointers.
void BmbtToBmdr(const uint8_t* src, size_t src_len, uint8_t* dst,
                size_t dst_len) {
  if (src_len < ondisk::btree::kLHeaderLenV5 || dst_len < 4) return;
  const uint16_t level = GetBE16(src, ondisk::btree::kLevel);
  const uint16_t numrecs = GetBE16(src, ondisk::btree::kNumrecs);
  PutBE16(dst, 0, level);
  PutBE16(dst, 2, numrecs);
  const size_t key_base = ondisk::btree::kLHeaderLenV5;
  const size_t ptr_base = key_base + static_cast<size_t>(numrecs) *
                                          ondisk::btree::kBmbtKeySize;
  const size_t dst_key_base = ondisk::btree::kBmdrSize;
  const size_t dst_ptr_base =
      dst_key_base + static_cast<size_t>(numrecs) * ondisk::btree::kBmbtKeySize;
  for (uint16_t i = 0; i < numrecs; ++i) {
    const size_t key_off = key_base + static_cast<size_t>(i) * 8;
    const size_t ptr_off = ptr_base + static_cast<size_t>(i) * 8;
    const size_t dst_key_off = dst_key_base + static_cast<size_t>(i) * 8;
    const size_t dst_ptr_off = dst_ptr_base + static_cast<size_t>(i) * 8;
    if (key_off + 8 > src_len || ptr_off + 8 > src_len) break;
    if (dst_key_off + 8 > dst_len || dst_ptr_off + 8 > dst_len) break;
    std::memcpy(dst + dst_key_off, src + key_off, 8);
    // Long-form startblock is 52 bits packed into the first 4 bytes of the
    // pointer plus the top 21 bits of the second; xfs_bmbt_disk_get_startblock
    // recomposes it into a full be64 pointer.
    const uint32_t lo = GetBE32(src, ptr_off);
    const uint32_t hi = GetBE32(src, ptr_off + 4);
    const uint64_t startblock =
        (static_cast<uint64_t>(lo & 0x1FFu) << 43) | (hi >> 21);
    StoreBE64(dst + dst_ptr_off, startblock);
  }
}

struct InodeLogFormat {
  uint32_t fields = 0;
  uint16_t asize = 0;
  uint16_t dsize = 0;
  uint64_t ino = 0;
  int64_t blkno = 0;
  uint32_t len = 0;
  uint32_t boffset = 0;
};

Result<InodeLogFormat> ParseInodeFormat(const std::vector<uint8_t>& region) {
  if (region.size() != lf::kInodeLogFormatSize) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "inode log item uses the legacy 32-bit format")
        .Actual(region.size())
        .Metadata("log item");
  }
  InodeLogFormat out;
  out.fields = LoadLE32(region.data() + lf::kIlfFields);
  out.asize = LoadLE16(region.data() + lf::kIlfAsize);
  out.dsize = LoadLE16(region.data() + lf::kIlfDsize);
  out.ino = static_cast<uint64_t>(LoadLE32(region.data() + lf::kIlfIno)) |
            (static_cast<uint64_t>(LoadLE32(region.data() + lf::kIlfIno + 4))
             << 32);
  out.blkno = static_cast<int64_t>(LoadLE32(region.data() + lf::kIlfBlkno)) |
              (static_cast<int64_t>(static_cast<uint32_t>(
                   LoadLE32(region.data() + lf::kIlfBlkno + 4)))
               << 32);
  out.len = LoadLE32(region.data() + lf::kIlfLen);
  out.boffset = LoadLE32(region.data() + lf::kIlfBoffset);
  if ((out.fields & lf::kILogCore) == 0 || out.blkno < 0 || out.len == 0) {
    return XFS_ERR(XfsError::kCorruptBtree, "bad inode log item geometry")
        .Metadata("log item");
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Replay driver
// ---------------------------------------------------------------------------
namespace {

class Replayer {
 public:
  Replayer(BlockDevice& dev, const XfsSuperblock& sb, const XfsGeometry& geo,
           const LogScan& scan)
      : dev_(dev),
        sb_(sb),
        geo_(geo),
        scan_(scan),
        buffers_(dev, sb.meta_uuid) {}

  Result<ReplayStats> Run(const LogScan& scan, RecoveredLogState* state);

 private:
  Status Pass1();
  Status Pass2();
  Status ReplayBufferItem(const RecoveredItem& item, uint64_t current_lsn);
  Status ReplayInodeItem(const RecoveredItem& item, uint64_t current_lsn);
  Status ReplayIcreateItem(const RecoveredItem& item, uint64_t current_lsn);
  Status ClearStaleBlocks();
  void RecordIntent(const RecoveredItem& item, uint64_t lsn);
  void ReleaseIntent(const RecoveredItem& item);

  BlockDevice& dev_;
  const XfsSuperblock& sb_;
  const XfsGeometry& geo_;
  const LogScan& scan_;
  ReplayStats stats_;
  CancelTable cancel_;
  ReplayBuffers buffers_;
  std::vector<PendingIntent> intents_;
};

Status Replayer::Pass1() {
  for (const RecoveredTransaction& trans : scan_.transactions) {
    for (const RecoveredItem& item : trans.items) {
      if (ClassifyLogItem(item.type) != RecoveryItemClass::kBuffer) continue;
      auto format = ParseBufFormat(item.regions[0]);
      if (!format.ok()) return format.error();
      if (format.value().flags & lf::kBlfCancel) {
        cancel_.Add(static_cast<uint64_t>(format.value().blkno),
                    format.value().len);
      }
    }
  }
  return OkStatus();
}

Status Replayer::ReplayBufferItem(const RecoveredItem& item,
                                  uint64_t current_lsn) {
  auto parsed = ParseBufFormat(item.regions[0]);
  if (!parsed.ok()) return parsed.error();
  const BufLogFormat& format = parsed.value();
  const uint64_t blkno = static_cast<uint64_t>(format.blkno);
  const uint32_t len = format.len;

  const uint32_t runs = CountRuns(format.map, format.map_size);
  if (item.regions.size() != static_cast<size_t>(runs) + 1) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "buffer log item regions do not match its bitmap")
        .Expected(runs + 1)
        .Actual(item.regions.size())
        .Metadata("log item");
  }

  if (format.flags & lf::kBlfCancel) {
    cancel_.Release(blkno, len);  // last occurrence consumed
    return OkStatus();
  }
  if (cancel_.IsCancelled(blkno, len)) {
    stats_.buffers_skipped_newer++;  // counts as "not replayed"
    return OkStatus();
  }

  ReplayBuffers::Key key{blkno, len};
  auto buffer = buffers_.Get(key, current_lsn);
  if (!buffer.ok()) return buffer.error();
  std::vector<uint8_t>& data = *buffer.value();

  // Metadata LSN rule: never replay an older change over newer on-disk
  // metadata (xlog_recover_get_buf_lsn + XFS_LSN_CMP).
  MetaType type;
  const bool recognised = ClassifyMetadata(data.data(), data.size(), &type);
  const int64_t on_disk_lsn =
      recognised ? OnDiskMetadataLsn(data.data(), data.size(), geo_.meta_uuid,
                                     type)
                 : -1;
  if (on_disk_lsn > 0 && CompareLsn(static_cast<uint64_t>(on_disk_lsn),
                                    current_lsn) >= 0) {
    Status verified = VerifySkippedBuffer(data, type, geo_.meta_uuid);
    if (!verified.ok()) return verified;
    stats_.buffers_skipped_newer++;
    return OkStatus();
  }

  Status patched = OkStatus();
  if (format.flags & lf::kBlfInodeBuf) {
    patched = DoInodeBuffer(item, format, &data, geo_);
    if (!patched.ok()) return patched;
    // Per-inode CRCs are already recomputed; never stamp the whole buffer.
    buffers_.MarkDirty(key);
    stats_.buffers_replayed++;
    return OkStatus();
  }
  patched = DoRegBuffer(item, format, &data, "metadata");
  if (!patched.ok()) return patched;
  if (!ClassifyMetadata(data.data(), data.size(), &type)) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "replayed metadata buffer has an unrecognised type")
        .Offset(blkno * 512)
        .Metadata("log replay");
  }
  buffers_.MarkDirty(key);
  stats_.buffers_replayed++;
  return OkStatus();
}

Status Replayer::ReplayInodeItem(const RecoveredItem& item,
                                 uint64_t current_lsn) {
  if (item.regions.size() < 2) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "inode log item without a log dinode")
        .Metadata("log item");
  }
  auto parsed = ParseInodeFormat(item.regions[0]);
  if (!parsed.ok()) return parsed.error();
  const InodeLogFormat& in_f = parsed.value();
  if (cancel_.IsCancelled(static_cast<uint64_t>(in_f.blkno), in_f.len)) {
    return OkStatus();
  }

  auto buffer = buffers_.Get({static_cast<uint64_t>(in_f.blkno), in_f.len},
                             current_lsn);
  if (!buffer.ok()) return buffer.error();
  std::vector<uint8_t>& data = *buffer.value();
  if (static_cast<uint64_t>(in_f.boffset) + geo_.inode_size > data.size()) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "inode log item offset is outside its buffer")
        .Ino(in_f.ino)
        .Metadata("log item");
  }
  uint8_t* dip = data.data() + in_f.boffset;
  if (GetBE16(dip, di::kMagic) != ondisk::kDinodeMagic) {
    return XFS_ERR(XfsError::kBadMagic,
                   "inode log item target is not an inode")
        .Ino(in_f.ino)
        .Actual(GetBE16(dip, di::kMagic))
        .Metadata("log item");
  }
  const uint8_t* ldip = item.regions[1].data();
  const size_t log_dinode_size = item.regions[1].size();
  if (log_dinode_size > lf::kLogDinodeSize ||
      LoadLE16(ldip) != ondisk::kDinodeMagic) {
    return XFS_ERR(XfsError::kCorruptInode, "bad inode log record")
        .Ino(in_f.ino)
        .Metadata("log item");
  }

  // Inode LSN rule (strictly greater: equal LSNs replay so that every
  // checkpoint with the same start LSN is applied).
  const uint8_t on_disk_version = dip[di::kVersion];
  if (on_disk_version >= 3) {
    const uint64_t lsn = GetBE64(dip, di::kLsn);
    if (lsn != 0 && lsn != 0xffffffffffffffffull &&
        CompareLsn(lsn, current_lsn) > 0) {
      stats_.buffers_skipped_newer++;
      return OkStatus();
    }
  }

  const uint32_t fields = in_f.fields;
  // The log dinode must describe a plausible inode.
  const uint16_t mode = LoadLE16(ldip + di::kMode);
  const uint8_t format = log_dinode_size > di::kFormat ? ldip[di::kFormat] : 0;
  if ((mode & kSIfmt) == kSIfreg &&
      format != di::kFormatExtents && format != di::kFormatBtree) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "bad log dinode data fork format for a regular file")
        .Ino(in_f.ino)
        .Actual(format)
        .Metadata("log item");
  }
  if ((mode & kSIfmt) == kSIfdir && format != di::kFormatExtents &&
      format != di::kFormatBtree && format != di::kFormatLocal) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "bad log dinode data fork format for a directory")
        .Ino(in_f.ino)
        .Actual(format)
        .Metadata("log item");
  }
  if (log_dinode_size > di::kForkoff &&
      ldip[di::kForkoff] > geo_.inode_size) {
    return XFS_ERR(XfsError::kCorruptInode, "bad log dinode fork offset")
        .Ino(in_f.ino)
        .Actual(ldip[di::kForkoff])
        .Metadata("log item");
  }

  LogDinodeToDisk(ldip, log_dinode_size, dip, geo_.inode_size, current_lsn);

  // Device nodes keep the 32-bit dev_t at the start of the data fork
  // (xfs_dinode_put_rdev); the value comes from the item header's union.
  if (fields & lf::kILogDev) {
    const uint32_t rdev = LoadLE32(item.regions[0].data() + lf::kIlfU);
    PutBE32(dip, di::kCoreSizeV3, rdev);
  }

  // Fork regions. XFS_ILOG_DFORK/AFORK mask and the region order are fixed by
  // xfs_inode_item_format: format, dinode, [data fork], [attr fork].
  const uint32_t dpart = fields & (lf::kILogDData | lf::kILogDExt |
                                   lf::kILogDBRoot);
  const uint32_t apart = fields & (lf::kILogAData | lf::kILogAExt |
                                   lf::kILogABRoot);
  if ((dpart != 0 && dpart != lf::kILogDData && dpart != lf::kILogDExt &&
       dpart != lf::kILogDBRoot) ||
      (apart != 0 && apart != lf::kILogAData && apart != lf::kILogAExt &&
       apart != lf::kILogABRoot)) {
    return XFS_ERR(XfsError::kCorruptBtree, "bad inode log item fork flags")
        .Ino(in_f.ino)
        .Actual(fields)
        .Metadata("log item");
  }
  const size_t core_size = di::kCoreSizeV3;  // this build only mounts v5
  const size_t litino = geo_.inode_size > core_size
                            ? geo_.inode_size - core_size
                            : 0;
  const uint8_t forkoff = dip[di::kForkoff];
  const size_t dfork_size = forkoff ? static_cast<size_t>(forkoff) << 3 : litino;
  const size_t afork_size = litino > (forkoff << 3)
                                ? litino - (static_cast<size_t>(forkoff) << 3)
                                : 0;
  uint8_t* dfork = dip + core_size;

  size_t region_index = 2;
  if (dpart != 0) {
    if (region_index >= item.regions.size()) {
      return XFS_ERR(XfsError::kCorruptBtree, "inode log item region missing")
          .Ino(in_f.ino)
          .Metadata("log item");
    }
    const std::vector<uint8_t>& region = item.regions[region_index++];
    if (dpart == lf::kILogDBRoot) {
      if (region.size() > dfork_size || dfork_size == 0) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "inode btree root does not fit the data fork")
            .Ino(in_f.ino)
            .Metadata("log item");
      }
      BmbtToBmdr(region.data(), region.size(), dfork, dfork_size);
    } else {
      if (region.size() > dfork_size) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "inode data fork region does not fit")
            .Ino(in_f.ino)
            .Actual(region.size())
            .Metadata("log item");
      }
      std::memcpy(dfork, region.data(), region.size());
    }
  }
  if (apart != 0) {
    if (region_index >= item.regions.size()) {
      return XFS_ERR(XfsError::kCorruptBtree, "inode log item region missing")
          .Ino(in_f.ino)
          .Metadata("log item");
    }
    const std::vector<uint8_t>& region = item.regions[region_index++];
    uint8_t* afork = dip + core_size + (static_cast<size_t>(forkoff) << 3);
    if (apart == lf::kILogABRoot) {
      if (region.size() > afork_size || afork_size < 8) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "inode btree root does not fit the attr fork")
            .Ino(in_f.ino)
            .Metadata("log item");
      }
      BmbtToBmdr(region.data(), region.size(), afork, afork_size);
    } else {
      if (region.size() > afork_size) {
        return XFS_ERR(XfsError::kCorruptInode,
                       "inode attr fork region does not fit")
            .Ino(in_f.ino)
            .Actual(region.size())
            .Metadata("log item");
      }
      std::memcpy(afork, region.data(), region.size());
    }
  }

  // Re-generate the checksum and validate the recovered inode.
  UpdateXfsChecksum(dip, geo_.inode_size, di::kCrc);
  XfsInode verified;
  Status check = ParseInode(dip, geo_.inode_size, geo_, in_f.ino, &verified,
                            /*allow_free_inode=*/true);
  if (!check.ok()) {
    return XFS_ERR(XfsError::kCorruptInode,
                   "recovered inode did not verify: " + check.error().message)
        .Ino(in_f.ino)
        .Metadata("log replay");
  }
  buffers_.MarkDirty({static_cast<uint64_t>(in_f.blkno), in_f.len});
  stats_.inodes_replayed++;
  return OkStatus();
}

Status Replayer::ReplayIcreateItem(const RecoveredItem& item,
                                   uint64_t current_lsn) {
  const std::vector<uint8_t>& region = item.regions[0];
  if (region.size() < lf::kIcreateLogSize ||
      LoadLE16(region.data() + lf::kIclType) != lf::kItemIcreate) {
    return XFS_ERR(XfsError::kCorruptBtree, "bad icreate log item")
        .Metadata("log item");
  }
  if (LoadLE16(region.data() + lf::kIclSize) != 1) {
    return XFS_ERR(XfsError::kCorruptBtree, "bad icreate log item size")
        .Metadata("log item");
  }
  const uint32_t agno = GetBE32(region.data(), lf::kIclAg);
  const uint32_t agbno = GetBE32(region.data(), lf::kIclAgbno);
  const uint32_t count = GetBE32(region.data(), lf::kIclCount);
  const uint32_t isize = GetBE32(region.data(), lf::kIclIsize);
  const uint32_t length = GetBE32(region.data(), lf::kIclLength);
  const uint32_t gen = GetBE32(region.data(), lf::kIclGen);
  const XfsInodeGeometry igeo = MakeInodeGeometry(sb_, geo_);
  const bool length_ok =
      length == igeo.ialloc_blks ||
      (geo_.features.spinodes && length == igeo.ialloc_min_blks);
  if (agno >= geo_.agcount || agbno == 0 || agbno >= geo_.AgBlocksFor(agno) ||
      length > geo_.AgBlocksFor(agno) - agbno ||
      isize != geo_.inode_size || !length_ok ||
      (count >> geo_.inopblog) != length || count == 0) {
    return XFS_ERR(XfsError::kCorruptBtree, "bad icreate geometry")
        .Ag(agno)
        .Agbno(agbno)
        .Metadata("log item");
  }

  const uint32_t blocks_per_cluster = igeo.blocks_per_cluster;
  const uint32_t inodes_per_cluster = igeo.inodes_per_cluster;
  const uint32_t clusters = length / blocks_per_cluster;

  // The whole chunk is skipped when any of its clusters was cancelled
  // (xlog_recover_icreate_commit_pass2).
  const uint32_t cluster_bb = blocks_per_cluster * (geo_.block_size / 512);
  uint32_t cancelled = 0;
  for (uint32_t j = 0; j < clusters; ++j) {
    const uint32_t cluster_agbno = agbno + j * blocks_per_cluster;
    const uint64_t daddr = geo_.FsbToByte(geo_.AgFsb(agno, cluster_agbno)) / 512;
    if (cancel_.IsCancelled(daddr, cluster_bb)) ++cancelled;
  }
  if (cancelled != 0) {
    if (cancelled != clusters) {
      XFS_WARN("partial inode chunk cancellation; skipped icreate");
    }
    stats_.buffers_skipped_newer++;
    return OkStatus();
  }

  // xfs_ialloc_inode_init: pristine free inode images, per-inode CRCs.
  uint64_t ino = geo_.AginoToIno(agno, agbno << geo_.inopblog);
  for (uint32_t j = 0; j < clusters; ++j) {
    const uint32_t cluster_agbno = agbno + j * blocks_per_cluster;
    const uint64_t cluster_daddr =
        geo_.FsbToByte(geo_.AgFsb(agno, cluster_agbno)) / 512;
    auto buffer = buffers_.Get({cluster_daddr, cluster_bb}, current_lsn);
    if (!buffer.ok()) return buffer.error();
    std::vector<uint8_t>& data = *buffer.value();
    std::fill(data.begin(), data.end(), uint8_t{0});
    for (uint32_t i = 0; i < inodes_per_cluster; ++i) {
      uint8_t* inode = data.data() + static_cast<size_t>(i) * geo_.inode_size;
      PutBE16(inode, di::kMagic, ondisk::kDinodeMagic);
      inode[di::kVersion] = 3;
      PutBE32(inode, di::kGen, gen);
      PutBE32(inode, di::kNextUnlinked, kNullAgino);
      PutBE64(inode, di::kIno, ino++);
      std::memcpy(inode + di::kUuid, geo_.meta_uuid.data(), 16);
      UpdateXfsChecksum(inode, geo_.inode_size, di::kCrc);
    }
    buffers_.MarkDirty({cluster_daddr, cluster_bb});
  }
  stats_.icreate_chunks_replayed++;
  return OkStatus();
}

void Replayer::RecordIntent(const RecoveredItem& item, uint64_t lsn) {
  PendingIntent intent;
  intent.type = item.type;
  intent.lsn = lsn;
  intent.regions = item.regions;
  intents_.push_back(std::move(intent));
}

void Replayer::ReleaseIntent(const RecoveredItem& item) {
  if (item.regions.empty() ||
      item.regions[0].size() < lf::kIntentLogFormatSize) {
    return;
  }
  // Host byte order, like every log item format.
  const uint64_t id =
      LoadLE64(item.regions[0].data() + lf::kIntentIdOffset);
  const uint16_t want =
      item.type == lf::kItemEfd ? lf::kItemEfi : lf::kItemBui;
  for (auto it = intents_.begin(); it != intents_.end(); ++it) {
    if (it->type != want) continue;
    if (it->regions.empty() ||
        it->regions[0].size() < lf::kIntentLogFormatSize) {
      continue;
    }
    if (LoadLE64(it->regions[0].data() + lf::kIntentIdOffset) == id) {
      intents_.erase(it);
      break;
    }
  }
}

Status Replayer::Pass2() {
  for (const RecoveredTransaction& trans : scan_.transactions) {
    Status began = buffers_.BeginLsn(trans.lsn);
    if (!began.ok()) return began;
    for (const RecoveredItem& item : trans.items) {
      Status status = OkStatus();
      switch (ClassifyLogItem(item.type)) {
        case RecoveryItemClass::kBuffer:
          status = ReplayBufferItem(item, trans.lsn);
          break;
        case RecoveryItemClass::kInode:
          status = ReplayInodeItem(item, trans.lsn);
          break;
        case RecoveryItemClass::kIcreate:
          status = ReplayIcreateItem(item, trans.lsn);
          break;
        case RecoveryItemClass::kIntent:
          RecordIntent(item, trans.lsn);
          stats_.intents_pending++;
          break;
        case RecoveryItemClass::kIntentDone:
          ReleaseIntent(item);
          stats_.intent_done_items++;
          break;
        case RecoveryItemClass::kRefused:
        case RecoveryItemClass::kUnknown:
          status = XFS_ERR(XfsError::kUnsupportedFeature,
                           std::string("unsupported recovery item ") +
                               (LogItemTypeName(item.type)
                                    ? LogItemTypeName(item.type)
                                    : "unknown"))
              .Actual(item.type)
              .Metadata("log replay");
          break;
      }
      if (!status.ok()) return status;
      stats_.items_replayed++;
    }
    stats_.transactions_replayed++;
    MaybeCrash(CrashPoint::kRecoveryAfterTransaction,
               "tid=" + std::to_string(trans.tid) + " lsn=0x" + [&] {
                 char buf[32];
                 std::snprintf(buf, sizeof(buf), "%llx",
                               static_cast<unsigned long long>(trans.lsn));
                 return std::string(buf);
               }());
  }
  return OkStatus();
}

// xlog_clear_stale_blocks(): overwrite the partial records in front of the
// head with empty records so a future crash cannot mistake them for data.
Status Replayer::ClearStaleBlocks() {
  const uint32_t log_blocks = scan_.geometry.log_blocks;
  const uint32_t tail_cycle = static_cast<uint32_t>(scan_.tail_lsn >> 32);
  const uint32_t tail_block = static_cast<uint32_t>(scan_.tail_lsn);
  const uint32_t head_cycle = scan_.head_cycle;
  const uint32_t head_block = scan_.head_block;
  int64_t tail_distance = 0;
  if (head_cycle == tail_cycle) {
    if (head_block < tail_block || head_block >= log_blocks) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "log head/tail are inconsistent")
          .Metadata("log");
    }
    tail_distance = static_cast<int64_t>(tail_block) +
                    (static_cast<int64_t>(log_blocks) - head_block);
  } else {
    if (head_block >= tail_block || head_cycle != tail_cycle + 1) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "log head/tail are inconsistent")
          .Metadata("log");
    }
    tail_distance = static_cast<int64_t>(tail_block) - head_block;
  }
  if (tail_distance <= 0) return OkStatus();
  int64_t max_distance = 4096;  // XLOG_TOTAL_REC_SHIFT
  if (tail_distance < max_distance) max_distance = tail_distance;

  auto write_empty = [&](uint32_t cycle, uint32_t start,
                         uint32_t blocks) -> Status {
    if (blocks == 0) return OkStatus();
    std::vector<uint8_t> buffer(static_cast<size_t>(blocks) * 512, 0);
    for (uint32_t i = 0; i < blocks; ++i) {
      uint8_t* block = buffer.data() + static_cast<size_t>(i) * 512;
      PutBE32(block, lf::kRecMagic, lf::kRecordMagic);
      PutBE32(block, lf::kRecCycle, cycle);
      PutBE32(block, lf::kRecVersion, lf::kVersion2);
      PutBE64(block, lf::kRecLsn, MakeLsn(cycle, start + i));
      PutBE64(block, lf::kRecTailLsn, MakeLsn(tail_cycle, tail_block));
      PutBE32(block, lf::kRecFmt, lf::kFmtLinuxLe);
      std::memcpy(block + lf::kRecUuid, sb_.uuid.data(), 16);
    }
    if (!dev_.Write(scan_.geometry.BlockOffset(start), buffer.data(),
                    buffer.size())) {
      return XFS_ERR(XfsError::kIoError,
                     "failed to clear stale log blocks")
          .Offset(scan_.geometry.BlockOffset(start))
          .Metadata("log");
    }
    return OkStatus();
  };

  Status status = OkStatus();
  if (head_block + max_distance <= log_blocks) {
    status = write_empty(head_cycle - 1, head_block,
                         static_cast<uint32_t>(max_distance));
  } else {
    status = write_empty(head_cycle - 1, head_block, log_blocks - head_block);
    if (status.ok()) {
      status = write_empty(head_cycle, 0,
                           static_cast<uint32_t>(max_distance -
                                                 (log_blocks - head_block)));
    }
  }
  if (!status.ok()) return status;
  if (!dev_.Flush()) {
    return XFS_ERR(XfsError::kIoError, "failed to flush the log")
        .Metadata("log");
  }
  return OkStatus();
}

Result<ReplayStats> Replayer::Run(const LogScan& scan, RecoveredLogState* state) {
  if (!scan.Recoverable()) {
    const std::string& first =
        scan.unsupported_items.empty() ? std::string("unknown")
                                       : scan.unsupported_items[0];
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "cannot recover dirty log: " + first)
        .Metadata("log");
  }
  Status status = Pass1();
  if (!status.ok()) return status.error();
  MaybeCrash(CrashPoint::kRecoveryBeforePass2,
             "head=" + std::to_string(scan.head_block) + " tail=" +
                 std::to_string(scan.tail_block));
  status = Pass2();
  if (!status.ok()) return status.error();
  status = buffers_.Flush();
  if (!status.ok()) return status.error();
  status = ClearStaleBlocks();
  if (!status.ok()) return status.error();

  stats_.bytes_written = buffers_.bytes_written();
  stats_.records_scanned = scan.records.size();
  stats_.intents_pending = intents_.size();
  if (cancel_.Size() != 0) {
    XFS_WARN("log recovery: " << cancel_.Size()
                              << " cancelled buffer(s) were not consumed");
  }

  if (state != nullptr) {
    state->head_block = scan.head_block;
    state->head_cycle = scan.head_cycle;
    state->prev_block = scan.prev_block == 0 && scan.records.empty()
                            ? 0xffffffffu
                            : scan.prev_block;
    // While unresolved intents exist the recovery tail stays pinned to the
    // start of the replayed window: a crash during intent completion must
    // re-replay the whole window (idempotently) to rebuild the intent list.
    state->tail_lsn = MakeLsn(scan.tail_cycle, scan.tail_block);
    state->log_blocks = scan.geometry.log_blocks;
    state->record_size = 32768;
  }
  return stats_;
}

}  // namespace

Result<ReplayStats> ReplayLog(BlockDevice& dev, const XfsSuperblock& sb,
                              const XfsGeometry& geo, const LogScan& scan,
                              RecoveredLogState* state) {
  Replayer replayer(dev, sb, geo, scan);
  return replayer.Run(scan, state);
}

}  // namespace xfs
