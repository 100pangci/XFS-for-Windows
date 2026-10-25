// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Typed refcountbt reader and verifier (RW-6C).
#include "xfs/refcount.h"

#include <algorithm>
#include <cstdio>
#include <map>
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

constexpr uint32_t kNullAgBlock = bt::kNullAgBlock;
constexpr size_t kHeader = bt::kSHeaderLenV5;
constexpr size_t kKey = bt::kRefcountKeySize;
constexpr size_t kPtr = bt::kRefcountPtrSize;
constexpr size_t kRec = bt::kRefcountRecSize;

Status ParseEncoded(uint32_t encoded, RefcountKey* key) {
  key->domain = (encoded & kRefcountCowFlag) ? RefcountDomain::kCow
                                             : RefcountDomain::kShared;
  key->startblock = encoded & kRefcountAgBlockMask;
  return OkStatus();
}

Status ValidateRecord(const RefcountRecord& rec, const XfsGeometry& geo,
                      uint32_t agno) {
  const uint64_t agblocks = geo.AgBlocksFor(agno);
  if (agno >= geo.agcount || agblocks == 0) {
    return XFS_ERR(XfsError::kOutOfRange, "refcount AG is out of range")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  if (rec.key.startblock >= agblocks ||
      (rec.key.startblock & kRefcountCowFlag) != 0) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "refcount record startblock leaves its AG")
        .Ag(agno)
        .Actual(rec.key.startblock)
        .Metadata("refcountbt");
  }
  if (rec.blockcount == 0 || rec.blockcount > kRefcountLenMax ||
      rec.blockcount > agblocks - rec.key.startblock) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "refcount record length is invalid")
        .Ag(agno)
        .Expected(agblocks - rec.key.startblock)
        .Actual(rec.blockcount)
        .Metadata("refcountbt");
  }
  if (rec.refcount == 0 || rec.refcount > kRefcountMax) {
    return XFS_ERR(XfsError::kCorruptBtree, "refcount value is invalid")
        .Ag(agno)
        .Actual(rec.refcount)
        .Metadata("refcountbt");
  }
  if (rec.key.domain == RefcountDomain::kCow) {
    if (rec.refcount != 1) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "CoW staging record must have refcount 1")
          .Ag(agno)
          .Actual(rec.refcount)
          .Metadata("refcountbt");
    }
  } else if (rec.refcount < 2) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "shared refcount record must have refcount >= 2")
        .Ag(agno)
        .Actual(rec.refcount)
        .Metadata("refcountbt");
  }
  return OkStatus();
}

class TreeChecker {
 public:
  TreeChecker(BlockDevice& device, const XfsGeometry& geo, const XfsAgf& agf,
              uint32_t agno, uint64_t sb_lsn)
      : device_(device), geo_(geo), agf_(agf), agno_(agno), sb_lsn_(sb_lsn) {}

  Status Run(RefcountTree* out) {
    const uint64_t agblocks = geo_.AgBlocksFor(agno_);
    if (geo_.block_size < kHeader || agblocks == 0) {
      return XFS_ERR(XfsError::kUnsupportedGeometry,
                     "invalid geometry for the refcountbt")
          .Ag(agno_)
          .Metadata("refcountbt");
    }
    if (agblocks > kRefcountCowFlag) {
      return XFS_ERR(XfsError::kUnsupportedGeometry,
                     "reflink requires AGs smaller than 2^31 blocks")
          .Ag(agno_)
          .Actual(agblocks)
          .Metadata("refcountbt");
    }
    if (agf_.refcount_root == kNullAgBlock ||
        agf_.refcount_root >= agblocks || agf_.refcount_level < 1 ||
        agf_.refcount_level > bt::kMaxTraversalLevels) {
      return XFS_ERR(XfsError::kCorruptAgHeader,
                     "AGF refcount root or level is invalid")
          .Ag(agno_)
          .Expected(bt::kMaxTraversalLevels)
          .Actual(agf_.refcount_level)
          .Metadata("refcountbt");
    }
    if (agf_.refcount_blocks == 0 || agf_.refcount_blocks > agblocks) {
      return XFS_ERR(XfsError::kCorruptAgHeader,
                     "AGF refcount_blocks is out of range")
          .Ag(agno_)
          .Actual(agf_.refcount_blocks)
          .Metadata("refcountbt");
    }
    max_leaf_ = static_cast<uint32_t>((geo_.block_size - kHeader) / kRec);
    max_node_ =
        static_cast<uint32_t>((geo_.block_size - kHeader) / (kKey + kPtr));
    if (max_leaf_ == 0 || max_node_ == 0) {
      return XFS_ERR(XfsError::kUnsupportedGeometry,
                     "refcountbt geometry has no room for records")
          .Ag(agno_)
          .Metadata("refcountbt");
    }

    uint32_t low = 0;
    Status s = Visit(agf_.refcount_root,
                     static_cast<uint16_t>(agf_.refcount_level - 1), true,
                     &low);
    if (!s.ok()) return s;
    if (visited_.size() != agf_.refcount_blocks) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "AGF refcount_blocks disagrees with the reachable tree")
          .Ag(agno_)
          .Expected(agf_.refcount_blocks)
          .Actual(visited_.size())
          .Metadata("refcountbt");
    }
    for (const auto& entry : last_at_level_) {
      if (entry.second.rightsib != kNullAgBlock) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rightmost refcountbt block has a right sibling")
            .Ag(agno_)
            .Actual(entry.second.rightsib)
            .Metadata("refcountbt");
      }
    }
    out->agno = agno_;
    out->root = agf_.refcount_root;
    out->level = agf_.refcount_level;
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
               uint32_t* low_out) {
    const uint64_t agblocks = geo_.AgBlocksFor(agno_);
    if (agbno == kNullAgBlock || agbno >= agblocks) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcountbt child pointer is out of range")
          .Ag(agno_)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    if (!visited_.insert(agbno).second) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcountbt is cyclic or a block is referenced twice")
          .Ag(agno_)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    if (visited_.size() > agblocks || visited_.size() > (1u << 22)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcountbt has an implausible number of blocks")
          .Ag(agno_)
          .Actual(visited_.size())
          .Metadata("refcountbt");
    }

    std::vector<uint8_t> buf(geo_.block_size);
    const uint64_t fsb = geo_.AgFsb(agno_, agbno);
    const uint64_t offset = geo_.FsbToByte(fsb);
    if (!device_.Read(offset, buf.data(), buf.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to read a refcountbt block")
          .Ag(agno_)
          .Agbno(agbno)
          .Offset(offset)
          .Metadata("refcountbt");
    }
    V5MetaCheck check;
    check.type = "refcountbt";
    check.magic_offset = bt::kMagic;
    check.magic = ondisk::kRefcountCrcMagic;
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
                     "refcountbt block level does not match its parent")
          .Ag(agno_)
          .Agbno(agbno)
          .Expected(expected_level)
          .Actual(level)
          .Metadata("refcountbt");
    }
    const uint32_t maxrecs = level == 0 ? max_leaf_ : max_node_;
    const uint32_t minrecs = maxrecs / 2;
    if (numrecs > maxrecs) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcountbt record count exceeds its block geometry")
          .Ag(agno_)
          .Agbno(agbno)
          .Expected(maxrecs)
          .Actual(numrecs)
          .Metadata("refcountbt");
    }
    if (numrecs == 0 && !(root && level == 0)) {
      return XFS_ERR(XfsError::kCorruptBtree, "refcountbt block is empty")
          .Ag(agno_)
          .Agbno(agbno)
          .Metadata("refcountbt");
    }
    if (!root && numrecs < minrecs) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "non-root refcountbt block is below half full")
          .Ag(agno_)
          .Agbno(agbno)
          .Expected(minrecs)
          .Actual(numrecs)
          .Metadata("refcountbt");
    }

    const uint32_t leftsib = GetBE32(buf.data(), bt::kSLeftsib);
    const uint32_t rightsib = GetBE32(buf.data(), bt::kSRightSib);
    auto last = last_at_level_.find(level);
    if (last == last_at_level_.end()) {
      if (leftsib != kNullAgBlock) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "leftmost refcountbt block has a left sibling")
            .Ag(agno_)
            .Agbno(agbno)
            .Actual(leftsib)
            .Metadata("refcountbt");
      }
    } else if (leftsib != last->second.ptr ||
               last->second.rightsib != agbno) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcountbt sibling links are inconsistent")
          .Ag(agno_)
          .Agbno(agbno)
          .Expected(last->second.ptr)
          .Actual(leftsib)
          .Metadata("refcountbt");
    }
    last_at_level_[level] = Link{agbno, rightsib};

    if (level == 0) return ReadLeaf(buf.data(), numrecs, agbno, low_out);
    return ReadNode(buf.data(), numrecs, agbno, level, low_out);
  }

  Status ReadLeaf(const uint8_t* buf, uint16_t numrecs, uint32_t agbno,
                  uint32_t* low_out) {
    bool first = true;
    for (uint16_t i = 0; i < numrecs; ++i) {
      const uint8_t* p = buf + kHeader + static_cast<size_t>(i) * kRec;
      RefcountRecord rec;
      const uint32_t encoded = GetBE32(p);
      Status s = ParseEncoded(encoded, &rec.key);
      if (!s.ok()) return s;
      rec.blockcount = GetBE32(p + 4);
      rec.refcount = GetBE32(p + 8);
      s = ValidateRecord(rec, geo_, agno_);
      if (!s.ok()) return s;
      if (rec.key.startblock != (encoded & kRefcountAgBlockMask)) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "refcount record startblock encoding is invalid")
            .Ag(agno_)
            .Actual(encoded)
            .Metadata("refcountbt");
      }
      if (!records_.empty() &&
          records_.back().key.Compare(rec.key) >= 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "refcount records are not strictly ordered by key")
            .Ag(agno_)
            .Agbno(agbno)
            .Actual(rec.key.startblock)
            .Metadata("refcountbt");
      }
      // The two domains occupy disjoint physical allocations.  An overlap is
      // always corruption, irrespective of the encoded key order (CoW records
      // sort after shared records but their physical blocks are arbitrary).
      for (const RefcountRecord& other : records_) {
        if (other.key.startblock < rec.EndBlock() &&
            rec.key.startblock < other.EndBlock()) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "refcount records overlap physically")
              .Ag(agno_)
              .Agbno(agbno)
              .Actual(rec.key.startblock)
              .Metadata("refcountbt");
        }
      }
      if (first) {
        *low_out = encoded;
        first = false;
      }
      records_.push_back(rec);
    }
    return OkStatus();
  }

  Status ReadNode(const uint8_t* buf, uint16_t numrecs, uint32_t agbno,
                  uint16_t level, uint32_t* low_out) {
    const size_t ptr_base = kHeader + static_cast<size_t>(max_node_) * kKey;
    bool first = true;
    uint32_t previous_low = 0;
    for (uint16_t i = 0; i < numrecs; ++i) {
      const uint8_t* kp = buf + kHeader + static_cast<size_t>(i) * kKey;
      const uint32_t low = GetBE32(kp);
      if (!first && previous_low >= low) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "refcountbt node keys are out of order")
            .Ag(agno_)
            .Agbno(agbno)
            .Actual(i)
            .Metadata("refcountbt");
      }
      previous_low = low;
      const uint32_t child =
          GetBE32(buf + ptr_base + static_cast<size_t>(i) * kPtr);
      uint32_t child_low = 0;
      Status s = Visit(child, static_cast<uint16_t>(level - 1), false,
                       &child_low);
      if (!s.ok()) return s;
      if (child_low != low) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "refcountbt parent key does not equal its child")
            .Ag(agno_)
            .Agbno(agbno)
            .Expected(child_low)
            .Actual(low)
            .Metadata("refcountbt");
      }
      if (first) {
        *low_out = low;
        first = false;
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
  std::vector<RefcountRecord> records_;
};

bool Overlaps(const RefcountRecord& rec, uint32_t agbno, uint32_t len) {
  const uint64_t end = static_cast<uint64_t>(agbno) + len;
  return rec.key.startblock < end && agbno < rec.EndBlock();
}

}  // namespace

const char* ToString(RefcountDomain domain) {
  return domain == RefcountDomain::kCow ? "cow" : "shared";
}

std::string RefcountDomainName(RefcountDomain domain) {
  return domain == RefcountDomain::kCow ? "COW staging" : "shared";
}

int RefcountKey::Compare(const RefcountKey& other) const {
  const uint32_t a = Encoded();
  const uint32_t b = other.Encoded();
  if (a != b) return a < b ? -1 : 1;
  return 0;
}

Status ValidateRefcountRecord(const RefcountRecord& rec,
                              const XfsGeometry& geo, uint32_t agno) {
  return ValidateRecord(rec, geo, agno);
}

Result<RefcountRecord> RefcountTree::LookupEq(
    const RefcountKey& key) const {
  auto it = std::lower_bound(records.begin(), records.end(), key,
                             [](const RefcountRecord& r,
                                const RefcountKey& k) {
                               return r.key.Compare(k) < 0;
                             });
  if (it == records.end() || it->key.Compare(key) != 0) {
    return XFS_ERR(XfsError::kNotFound, "refcount key not found")
        .Ag(agno)
        .Actual(key.startblock)
        .Metadata("refcountbt");
  }
  return *it;
}

Result<RefcountRecord> RefcountTree::LookupLE(
    const RefcountKey& key) const {
  auto it = std::upper_bound(records.begin(), records.end(), key,
                             [](const RefcountKey& k,
                                const RefcountRecord& r) {
                               return k.Compare(r.key) < 0;
                             });
  if (it == records.begin()) {
    return XFS_ERR(XfsError::kNotFound,
                   "no refcount key at or below the target")
        .Ag(agno)
        .Actual(key.startblock)
        .Metadata("refcountbt");
  }
  return *(it - 1);
}

Result<RefcountRecord> RefcountTree::LookupGE(
    const RefcountKey& key) const {
  auto it = std::lower_bound(records.begin(), records.end(), key,
                             [](const RefcountRecord& r,
                                const RefcountKey& k) {
                               return r.key.Compare(k) < 0;
                             });
  if (it == records.end()) {
    return XFS_ERR(XfsError::kNotFound,
                   "no refcount key at or above the target")
        .Ag(agno)
        .Actual(key.startblock)
        .Metadata("refcountbt");
  }
  return *it;
}

Result<RefcountRecord> RefcountTree::Predecessor(
    const RefcountKey& key) const {
  auto it = std::lower_bound(records.begin(), records.end(), key,
                             [](const RefcountRecord& r,
                                const RefcountKey& k) {
                               return r.key.Compare(k) < 0;
                             });
  if (it == records.begin()) {
    return XFS_ERR(XfsError::kNotFound,
                   "refcount key has no predecessor")
        .Ag(agno)
        .Actual(key.startblock)
        .Metadata("refcountbt");
  }
  return *(it - 1);
}

Result<RefcountRecord> RefcountTree::Successor(
    const RefcountKey& key) const {
  auto it = std::upper_bound(records.begin(), records.end(), key,
                             [](const RefcountKey& k,
                                const RefcountRecord& r) {
                               return k.Compare(r.key) < 0;
                             });
  if (it == records.end()) {
    return XFS_ERR(XfsError::kNotFound, "refcount key has no successor")
        .Ag(agno)
        .Actual(key.startblock)
        .Metadata("refcountbt");
  }
  return *it;
}

bool RefcountTree::OverlapsShared(uint32_t agbno, uint32_t len) const {
  RefcountRecord out;
  return FindShared(agbno, len, &out);
}

bool RefcountTree::FindShared(uint32_t agbno, uint32_t len,
                              RefcountRecord* out) const {
  if (len == 0) return false;
  for (const RefcountRecord& rec : records) {
    if (rec.cow()) break;  // all CoW records sort after shared records
    if (!Overlaps(rec, agbno, len)) continue;
    const uint32_t start = std::max(rec.key.startblock, agbno);
    const uint64_t end = std::min<uint64_t>(rec.EndBlock(),
                                            static_cast<uint64_t>(agbno) + len);
    if (end <= start) continue;
    *out = rec;
    out->key.startblock = start;
    out->blockcount = static_cast<uint32_t>(end - start);
    return true;
  }
  return false;
}

bool RefcountTree::FindCow(uint32_t agbno, uint32_t len,
                           RefcountRecord* out) const {
  if (len == 0) return false;
  for (const RefcountRecord& rec : records) {
    if (!rec.cow()) continue;
    if (!Overlaps(rec, agbno, len)) continue;
    const uint32_t start = std::max(rec.key.startblock, agbno);
    const uint64_t end = std::min<uint64_t>(rec.EndBlock(),
                                            static_cast<uint64_t>(agbno) + len);
    if (end <= start) continue;
    *out = rec;
    out->key.startblock = start;
    out->blockcount = static_cast<uint32_t>(end - start);
    return true;
  }
  return false;
}

Result<RefcountTree> LoadRefcountTree(BlockDevice& device,
                                      const XfsGeometry& geo, uint32_t agno,
                                      uint64_t sb_lsn) {
  if (!geo.features.reflink) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "filesystem does not have the reflink feature")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  if (agno >= geo.agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  auto agf = ReadAgf(device, geo, agno);
  if (!agf.ok()) return agf.error();
  auto tree = std::make_unique<RefcountTree>();
  TreeChecker checker(device, geo, agf.value(), agno, sb_lsn);
  Status s = checker.Run(tree.get());
  if (!s.ok()) return s.error();
  return std::move(*tree);
}

}  // namespace xfs
