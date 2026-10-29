// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Typed rmapbt reader and verifier (RW-6B). See docs/RMAPBT.md.
#include "xfs/rmap.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "xfs/ag.h"
#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/verifier.h"

namespace xfs {
namespace {

namespace bt = ondisk::btree;
namespace agf = ondisk::agf;

constexpr uint32_t kNullAgBlock = 0xffffffffu;
constexpr size_t kHeader = bt::kSHeaderLenV5;  // 56
constexpr size_t kKey = 20;
constexpr size_t kPair = 2 * kKey;             // 40
constexpr size_t kRec = 24;
constexpr size_t kPtr = 4;
constexpr uint64_t kMaxFileOffset = (1ULL << 54) + (1ULL << 21) - 2;

uint64_t PackKeyOffset(uint64_t offset, uint32_t flags) {
  uint64_t packed = offset & kRmapOffsetMask;
  if (flags & kRmapAttrFork) packed |= kRmapOffsetAttr;
  if (flags & kRmapBmbtBlock) packed |= kRmapOffsetBmbt;
  return packed;
}

Status UnpackOffset(uint64_t packed, bool key_context, RmapKey* out) {
  if (packed & ~(kRmapOffsetMask | kRmapOffsetAttr | kRmapOffsetBmbt |
                 kRmapOffsetUnwritten)) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "rmap offset has reserved or invalid flag bits")
        .Actual(packed)
        .Metadata("rmapbt");
  }
  if (key_context && (packed & kRmapOffsetUnwritten)) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "unwritten bit is set in an rmapbt key")
        .Actual(packed)
        .Metadata("rmapbt");
  }
  out->offset = packed & kRmapOffsetMask;
  if (packed & kRmapOffsetAttr) out->flags |= kRmapAttrFork;
  if (packed & kRmapOffsetBmbt) out->flags |= kRmapBmbtBlock;
  if (!key_context && (packed & kRmapOffsetUnwritten)) {
    out->flags |= kRmapUnwritten;
  }
  return OkStatus();
}

// xfs_rmap_key: 4-byte startblock, 8-byte owner, 8-byte offset (20 bytes).
RmapKey ParseKey(const uint8_t* p, bool key_context, Status* status) {
  RmapKey k;
  k.startblock = GetBE32(p);
  k.owner = GetBE64(p + 4);
  *status = UnpackOffset(GetBE64(p + 12), key_context, &k);
  return k;
}

// xfs_rmap_rec: startblock, blockcount, owner, offset (24 bytes). Its owner
// and offset are at different offsets than the packed key.
RmapKey ParseRecordKey(const uint8_t* p, uint32_t* blockcount, Status* status) {
  RmapKey k;
  k.startblock = GetBE32(p);
  *blockcount = GetBE32(p + 4);
  k.owner = GetBE64(p + 8);
  *status = UnpackOffset(GetBE64(p + 16), false, &k);
  return k;
}

// xfs_rmapbt_init_high_key_from_rec: only inode fork extents carry the logical
// end offset; metadata and bmbt-block records have a zero offset component.
RmapKey HighKey(const RmapRecord& rec) {
  RmapKey high = rec.key;
  const uint32_t adj = rec.blockcount - 1;
  high.startblock += adj;
  if (RmapNonInodeOwner(high.owner) ||
      (high.flags & kRmapBmbtBlock)) {
    return high;
  }
  high.offset = ((high.offset + adj) & kRmapOffsetMask) |
                (PackKeyOffset(0, high.flags) &
                 (kRmapOffsetAttr | kRmapOffsetBmbt));
  return high;
}

bool FileRangeValid(uint64_t off, uint64_t len) {
  return len != 0 && off + len > off &&
         off <= kMaxFileOffset && off + len - 1 <= kMaxFileOffset;
}

Status ValidateRecord(const RmapRecord& rec, const XfsGeometry& geo,
                      uint32_t agno) {
  const uint64_t agblocks = geo.AgBlocksFor(agno);
  const uint32_t agfl_block = static_cast<uint32_t>(
      (3ull * geo.sector_size) / geo.block_size);
  if (rec.blockcount == 0) {
    return XFS_ERR(XfsError::kCorruptBtree, "rmap record has zero blockcount")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  if (rec.key.startblock <= agfl_block) {
    if (rec.key.owner != kRmapOwnerFs) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "AG static metadata record has the wrong owner")
          .Ag(agno)
          .Actual(rec.key.owner)
          .Metadata("rmapbt");
    }
    if (rec.blockcount != agfl_block + 1) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "AG static metadata record has the wrong length")
          .Ag(agno)
          .Expected(agfl_block + 1)
          .Actual(rec.blockcount)
          .Metadata("rmapbt");
    }
  } else if (rec.key.startblock >= agblocks ||
             rec.blockcount > agblocks - rec.key.startblock) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "rmap extent leaves its allocation group")
        .Ag(agno)
        .Actual(rec.key.startblock)
        .Metadata("rmapbt");
  }

  const bool inode = !RmapNonInodeOwner(rec.key.owner);
  if (!inode) {
    if (rec.key.owner < kRmapOwnerMin || rec.key.owner > kRmapOwnerFs) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmap record has an invalid special owner")
          .Ag(agno)
          .Actual(rec.key.owner)
          .Metadata("rmapbt");
    }
    if (rec.key.offset != 0 || rec.key.flags != 0) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "special owner record has an offset or flag")
          .Ag(agno)
          .Actual(rec.key.owner)
          .Metadata("rmapbt");
    }
    return OkStatus();
  }

  // A data or bmbt extent may live in an AG other than the owner inode's AG;
  // the record's owner is validated against the filesystem, not against the
  // AG that stores this record.
  const uint32_t owner_agfl =
      static_cast<uint32_t>((3ull * geo.sector_size) / geo.block_size);
  if (!geo.InoValid(rec.key.owner) ||
      geo.InoAgbno(rec.key.owner) <= owner_agfl) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "rmap record owner is not a valid AG inode number")
        .Ag(agno)
        .Actual(rec.key.owner)
        .Metadata("rmapbt");
  }
  if (rec.key.flags & ~(kRmapKeyFlags | kRmapUnwritten)) {
    return XFS_ERR(XfsError::kCorruptBtree, "rmap record has invalid flags")
        .Ag(agno)
        .Actual(rec.key.flags)
        .Metadata("rmapbt");
  }
  const bool bmbt = (rec.key.flags & kRmapBmbtBlock) != 0;
  const bool attr = (rec.key.flags & kRmapAttrFork) != 0;
  if (bmbt && rec.key.offset != 0) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "rmap bmbt record has a nonzero fork offset")
        .Ag(agno)
        .Actual(rec.key.offset)
        .Metadata("rmapbt");
  }
  if (rec.unwritten() && (bmbt || attr)) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "unwritten rmap record belongs to metadata or attr fork")
        .Ag(agno)
        .Actual(rec.key.flags)
        .Metadata("rmapbt");
  }
  if (!bmbt && !FileRangeValid(rec.key.offset, rec.blockcount)) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "rmap inode fork offset/length is out of range")
        .Ag(agno)
        .Actual(rec.key.offset)
        .Metadata("rmapbt");
  }
  return OkStatus();
}

class TreeChecker {
 public:
  TreeChecker(BlockDevice& device, const XfsGeometry& geo, const XfsAgf& agf,
              uint32_t agno, uint64_t sb_lsn)
      : device_(device), geo_(geo), agf_(agf), agno_(agno), sb_lsn_(sb_lsn) {}

  Status Run(RmapTree* out) {
    const uint64_t agblocks = geo_.AgBlocksFor(agno_);
    if (geo_.block_size < kHeader || agblocks == 0) {
      return XFS_ERR(XfsError::kUnsupportedGeometry,
                     "invalid geometry for the rmapbt")
          .Ag(agno_)
          .Metadata("rmapbt");
    }
    if (agf_.rmap_root == kNullAgBlock || agf_.rmap_root >= agblocks ||
        agf_.rmap_level < 1 ||
        agf_.rmap_level > bt::kMaxTraversalLevels) {
      return XFS_ERR(XfsError::kCorruptAgHeader,
                     "AGF rmap root or level is invalid")
          .Ag(agno_)
          .Expected(bt::kMaxTraversalLevels)
          .Actual(agf_.rmap_level)
          .Metadata("rmapbt");
    }
    if (agf_.rmap_blocks == 0 || agf_.rmap_blocks > agblocks) {
      return XFS_ERR(XfsError::kCorruptAgHeader,
                     "AGF rmap_blocks is out of range")
          .Ag(agno_)
          .Actual(agf_.rmap_blocks)
          .Metadata("rmapbt");
    }
    max_leaf_ = static_cast<uint32_t>((geo_.block_size - kHeader) / kRec);
    max_node_ =
        static_cast<uint32_t>((geo_.block_size - kHeader) / (kPair + kPtr));
    if (max_leaf_ == 0 || max_node_ == 0) {
      return XFS_ERR(XfsError::kUnsupportedGeometry,
                     "rmapbt geometry has no room for records")
          .Ag(agno_)
          .Metadata("rmapbt");
    }

    RmapKey low;
    RmapKey high;
    Status s = Visit(agf_.rmap_root, static_cast<uint16_t>(agf_.rmap_level - 1),
                     true, &low, &high);
    if (!s.ok()) return s;
    if (visited_.size() != agf_.rmap_blocks) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "AGF rmap_blocks disagrees with the reachable tree")
          .Ag(agno_)
          .Expected(agf_.rmap_blocks)
          .Actual(visited_.size())
          .Metadata("rmapbt");
    }
    for (const auto& entry : last_at_level_) {
      if (entry.second.rightsib != kNullAgBlock) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rightmost rmapbt block has a right sibling")
            .Ag(agno_)
            .Actual(entry.second.rightsib)
            .Metadata("rmapbt");
      }
    }
    out->agno = agno_;
    out->root = agf_.rmap_root;
    out->level = agf_.rmap_level;
    out->blocks = static_cast<uint32_t>(visited_.size());
    out->records = std::move(records_);
    return OkStatus();
  }

 private:
  struct Link {
    uint32_t ptr = 0;
    uint32_t rightsib = 0;
  };

  Status Visit(uint32_t agbno, uint16_t expected_level, bool root,
               RmapKey* low_out, RmapKey* high_out) {
    const uint64_t agblocks = geo_.AgBlocksFor(agno_);
    if (agbno == kNullAgBlock || agbno >= agblocks) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmapbt child pointer is out of range")
          .Ag(agno_)
          .Actual(agbno)
          .Metadata("rmapbt");
    }
    if (!visited_.insert(agbno).second) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmapbt is cyclic or a block is referenced twice")
          .Ag(agno_)
          .Actual(agbno)
          .Metadata("rmapbt");
    }
    if (visited_.size() > agblocks || visited_.size() > (1u << 22)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmapbt has an implausible number of blocks")
          .Ag(agno_)
          .Actual(visited_.size())
          .Metadata("rmapbt");
    }

    std::vector<uint8_t> buf(geo_.block_size);
    const uint64_t fsb = geo_.AgFsb(agno_, agbno);
    const uint64_t offset = geo_.FsbToByte(fsb);
    if (!device_.Read(offset, buf.data(), buf.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to read an rmapbt block")
          .Ag(agno_)
          .Agbno(agbno)
          .Offset(offset)
          .Metadata("rmapbt");
    }
    V5MetaCheck check;
    check.type = "rmapbt";
    check.magic_offset = bt::kMagic;
    check.magic = ondisk::kRmapCrcMagic;
    check.crc_offset = bt::kSCrc;
    check.uuid_offset = bt::kSUuid;
    check.blkno_offset = bt::kSBlkno;
    check.lsn_offset = bt::kSLsn;
    check.owner_offset = bt::kSOwner;
    check.owner_size = 4;
    Status s = VerifyV5Metadata(buf.data(), buf.size(), check, offset / 512,
                                agno_, geo_.meta_uuid, sb_lsn_);
    if (!s.ok()) return s;

    const uint16_t level = GetBE16(buf.data(), bt::kLevel);
    const uint16_t numrecs = GetBE16(buf.data(), bt::kNumrecs);
    if (level != expected_level) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmapbt block level does not match its parent")
          .Ag(agno_)
          .Agbno(agbno)
          .Expected(expected_level)
          .Actual(level)
          .Metadata("rmapbt");
    }
    const uint32_t maxrecs = level == 0 ? max_leaf_ : max_node_;
    const uint32_t minrecs = maxrecs / 2;
    if (numrecs > maxrecs) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmapbt record count exceeds its block geometry")
          .Ag(agno_)
          .Agbno(agbno)
          .Expected(maxrecs)
          .Actual(numrecs)
          .Metadata("rmapbt");
    }
    if (numrecs == 0) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmapbt block is empty")
          .Ag(agno_)
          .Agbno(agbno)
          .Metadata("rmapbt");
    }
    if (!root && numrecs < minrecs) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "non-root rmapbt block is below half full")
          .Ag(agno_)
          .Agbno(agbno)
          .Expected(minrecs)
          .Actual(numrecs)
          .Metadata("rmapbt");
    }

    const uint32_t leftsib = GetBE32(buf.data(), bt::kSLeftsib);
    const uint32_t rightsib = GetBE32(buf.data(), bt::kSRightSib);
    auto last = last_at_level_.find(level);
    if (last == last_at_level_.end()) {
      if (leftsib != kNullAgBlock) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "leftmost rmapbt block has a left sibling")
            .Ag(agno_)
            .Agbno(agbno)
            .Actual(leftsib)
            .Metadata("rmapbt");
      }
    } else {
      if (leftsib != last->second.ptr ||
          last->second.rightsib != agbno) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmapbt sibling links are inconsistent")
            .Ag(agno_)
            .Agbno(agbno)
            .Expected(last->second.ptr)
            .Actual(leftsib)
            .Metadata("rmapbt");
      }
    }
    last_at_level_[level] = Link{agbno, rightsib};

    if (level == 0) {
      return ReadLeaf(buf.data(), numrecs, agbno, low_out, high_out);
    }
    return ReadNode(buf.data(), numrecs, agbno, level, low_out, high_out);
  }

  Status ReadLeaf(const uint8_t* buf, uint16_t numrecs, uint32_t agbno,
                  RmapKey* low_out, RmapKey* high_out) {
    bool first = true;
    for (uint16_t i = 0; i < numrecs; ++i) {
      const uint8_t* p = buf + kHeader + static_cast<size_t>(i) * kRec;
      RmapRecord rec;
      Status s = OkStatus();
      rec.key = ParseRecordKey(p, &rec.blockcount, &s);
      if (!s.ok()) return s;
      s = ValidateRecord(rec, geo_, agno_);
      if (!s.ok()) return s;
      if (!records_.empty() &&
          records_.back().key.Compare(rec.key) >= 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmap records are not strictly ordered by key")
            .Ag(agno_)
            .Agbno(agbno)
            .Actual(rec.key.startblock)
            .Metadata("rmapbt");
      }
      const RmapKey high = HighKey(rec);
      if (first) {
        *low_out = rec.key;
        *high_out = high;
        first = false;
      } else if (high.Compare(*high_out) > 0) {
        *high_out = high;
      }
      records_.push_back(rec);
    }
    return OkStatus();
  }

  Status ReadNode(const uint8_t* buf, uint16_t numrecs, uint32_t agbno,
                  uint16_t level, RmapKey* low_out, RmapKey* high_out) {
    const size_t ptr_base = kHeader + static_cast<size_t>(max_node_) * kPair;
    bool first = true;
    RmapKey previous_low;
    for (uint16_t i = 0; i < numrecs; ++i) {
      const uint8_t* lp = buf + kHeader + static_cast<size_t>(i) * kPair;
      const uint8_t* hp = lp + kKey;
      Status s = OkStatus();
      const RmapKey low = ParseKey(lp, true, &s);
      if (!s.ok()) return s;
      const RmapKey high = ParseKey(hp, true, &s);
      if (!s.ok()) return s;
      if (low.Compare(high) > 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmapbt node low key exceeds its high key")
            .Ag(agno_)
            .Agbno(agbno)
            .Actual(i)
            .Metadata("rmapbt");
      }
      if (!first && previous_low.Compare(low) > 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmapbt node keys are out of order")
            .Ag(agno_)
            .Agbno(agbno)
            .Actual(i)
            .Metadata("rmapbt");
      }
      previous_low = low;
      const uint32_t child =
          GetBE32(buf + ptr_base + static_cast<size_t>(i) * kPtr);
      RmapKey child_low;
      RmapKey child_high;
      s = Visit(child, static_cast<uint16_t>(level - 1), false, &child_low,
                &child_high);
      if (!s.ok()) return s;
      if (child_low.Compare(low) != 0 || child_high.Compare(high) != 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmapbt parent key does not equal its child subtree")
            .Ag(agno_)
            .Agbno(agbno)
            .Actual(i)
            .Metadata("rmapbt");
      }
      if (first) {
        *low_out = low;
        *high_out = high;
        first = false;
      } else if (child_high.Compare(*high_out) > 0) {
        *high_out = child_high;
      }
    }
    return OkStatus();
  }

  BlockDevice& device_;
  const XfsGeometry& geo_;
  const XfsAgf& agf_;
  uint32_t agno_;
  uint64_t sb_lsn_;
  uint32_t max_leaf_ = 0;
  uint32_t max_node_ = 0;
  std::set<uint32_t> visited_;
  std::map<uint16_t, Link> last_at_level_;
  std::vector<RmapRecord> records_;
};

}  // namespace

Status ValidateRmapRecord(const RmapRecord& rec, const XfsGeometry& geo,
                          uint32_t agno) {
  return ValidateRecord(rec, geo, agno);
}

RmapKey RmapRecordHighKey(const RmapRecord& rec) { return HighKey(rec); }

int RmapKey::Compare(const RmapKey& other) const {
  if (startblock != other.startblock) return startblock < other.startblock ? -1 : 1;
  if (owner != other.owner) return owner < other.owner ? -1 : 1;
  const uint64_t a = PackKeyOffset(offset, flags);
  const uint64_t b = PackKeyOffset(other.offset, other.flags);
  if (a != b) return a < b ? -1 : 1;
  return 0;
}

Result<RmapRecord> RmapTree::LookupEq(const RmapKey& key) const {
  auto it = std::lower_bound(records.begin(), records.end(), key,
                             [](const RmapRecord& r, const RmapKey& k) {
                               return r.key.Compare(k) < 0;
                             });
  if (it == records.end() || it->key.Compare(key) != 0) {
    return XFS_ERR(XfsError::kNotFound, "rmap key not found")
        .Ag(agno)
        .Actual(key.startblock)
        .Metadata("rmapbt");
  }
  return *it;
}

Result<RmapRecord> RmapTree::LookupLE(const RmapKey& key) const {
  auto it = std::upper_bound(records.begin(), records.end(), key,
                             [](const RmapKey& k, const RmapRecord& r) {
                               return k.Compare(r.key) < 0;
                             });
  if (it == records.begin()) {
    return XFS_ERR(XfsError::kNotFound, "no rmap key at or below the target")
        .Ag(agno)
        .Actual(key.startblock)
        .Metadata("rmapbt");
  }
  return *(it - 1);
}

Result<RmapRecord> RmapTree::LookupGE(const RmapKey& key) const {
  auto it = std::lower_bound(records.begin(), records.end(), key,
                             [](const RmapRecord& r, const RmapKey& k) {
                               return r.key.Compare(k) < 0;
                             });
  if (it == records.end()) {
    return XFS_ERR(XfsError::kNotFound, "no rmap key at or above the target")
        .Ag(agno)
        .Actual(key.startblock)
        .Metadata("rmapbt");
  }
  return *it;
}

Result<RmapRecord> RmapTree::Predecessor(const RmapKey& key) const {
  auto it = std::lower_bound(records.begin(), records.end(), key,
                             [](const RmapRecord& r, const RmapKey& k) {
                               return r.key.Compare(k) < 0;
                             });
  if (it == records.begin()) {
    return XFS_ERR(XfsError::kNotFound, "rmap key has no predecessor")
        .Ag(agno)
        .Actual(key.startblock)
        .Metadata("rmapbt");
  }
  return *(it - 1);
}

Result<RmapRecord> RmapTree::Successor(const RmapKey& key) const {
  auto it = std::upper_bound(records.begin(), records.end(), key,
                             [](const RmapKey& k, const RmapRecord& r) {
                               return k.Compare(r.key) < 0;
                             });
  if (it == records.end()) {
    return XFS_ERR(XfsError::kNotFound, "rmap key has no successor")
        .Ag(agno)
        .Actual(key.startblock)
        .Metadata("rmapbt");
  }
  return *it;
}

Result<RmapTree> LoadRmapTree(BlockDevice& device, const XfsGeometry& geo,
                              uint32_t agno, uint64_t sb_lsn) {
  if (!geo.features.rmapbt) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "filesystem does not have the rmapbt feature")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  if (agno >= geo.agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  auto agf = ReadAgf(device, geo, agno);
  if (!agf.ok()) return agf.error();
  auto tree = std::make_unique<RmapTree>();
  TreeChecker checker(device, geo, agf.value(), agno, sb_lsn);
  Status s = checker.Run(tree.get());
  if (!s.ok()) return s.error();
  return std::move(*tree);
}

std::string RmapOwnerName(uint64_t owner) {
  switch (owner) {
    case kRmapOwnerNull: return "OWN_NULL";
    case kRmapOwnerUnknown: return "OWN_UNKNOWN";
    case kRmapOwnerFs: return "OWN_FS";
    case kRmapOwnerLog: return "OWN_LOG";
    case kRmapOwnerAg: return "OWN_AG";
    case kRmapOwnerInobt: return "OWN_INOBT";
    case kRmapOwnerInodes: return "OWN_INODES";
    case kRmapOwnerRefc: return "OWN_REFC";
    case kRmapOwnerCow: return "OWN_COW";
    case kRmapOwnerMin: return "OWN_MIN";
    default: break;
  }
  char buf[48];
  std::snprintf(buf, sizeof(buf), "inode %llu",
                static_cast<unsigned long long>(owner));
  return buf;
}

}  // namespace xfs
