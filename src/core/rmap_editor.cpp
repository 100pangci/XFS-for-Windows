// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Writable rmapbt editor (RW-6B).  See docs/RMAPBT.md.
//
// Two separate layers live here:
//
//   * logical mutations (MapExtent / UnmapExtent / ConvertExtent and the
//     metadata wrappers) implement the Linux xfs_rmap_* rules: owner, flags,
//     physical and logical adjacency, exact/prefix/suffix/middle coverage;
//   * the physical B+tree below them implements leaf/node insert, delete,
//     split, merge and borrow, sibling links, root growth/collapse and
//     overlapping-key propagation.
//
// rmapbt is an overlapping btree.  A node's high key is the maximum high key
// of its whole child subtree, not the high key of its last child record.  All
// ancestors' low/high keys are therefore recomputed along the modified path.
#include "xfs/rmap_editor.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "xfs/endian.h"
#include "xfs/ondisk.h"

namespace xfs {
namespace {

namespace bt = ondisk::btree;

constexpr uint32_t kNullAgBlock = 0xffffffffu;
constexpr size_t kHeader = bt::kSHeaderLenV5;  // 56
constexpr size_t kKeySize = 20;
constexpr size_t kPairSize = 2 * kKeySize;     // 40
constexpr size_t kRecSize = 24;
constexpr size_t kPtrSize = 4;

uint64_t PackKeyOffset(uint64_t offset, uint32_t flags) {
  uint64_t packed = offset & kRmapOffsetMask;
  if (flags & kRmapAttrFork) packed |= kRmapOffsetAttr;
  if (flags & kRmapBmbtBlock) packed |= kRmapOffsetBmbt;
  return packed;
}

RmapKey ParseKey(const uint8_t* p) {
  RmapKey k;
  k.startblock = GetBE32(p, 0);
  k.owner = GetBE64(p, 4);
  const uint64_t packed = GetBE64(p, 12);
  k.offset = packed & kRmapOffsetMask;
  if (packed & kRmapOffsetAttr) k.flags |= kRmapAttrFork;
  if (packed & kRmapOffsetBmbt) k.flags |= kRmapBmbtBlock;
  return k;
}

void StoreKey(uint8_t* p, const RmapKey& k) {
  PutBE32(p, 0, k.startblock);
  PutBE64(p, 4, k.owner);
  PutBE64(p, 12, PackKeyOffset(k.offset, k.flags));
}

RmapRecord ParseRecord(const uint8_t* p) {
  RmapRecord r;
  r.key.startblock = GetBE32(p, 0);
  r.blockcount = GetBE32(p, 4);
  r.key.owner = GetBE64(p, 8);
  const uint64_t packed = GetBE64(p, 16);
  r.key.offset = packed & kRmapOffsetMask;
  if (packed & kRmapOffsetAttr) r.key.flags |= kRmapAttrFork;
  if (packed & kRmapOffsetBmbt) r.key.flags |= kRmapBmbtBlock;
  if (packed & kRmapOffsetUnwritten) r.key.flags |= kRmapUnwritten;
  return r;
}

void StoreRecord(uint8_t* p, const RmapRecord& r) {
  PutBE32(p, 0, r.key.startblock);
  PutBE32(p, 4, r.blockcount);
  PutBE64(p, 8, r.key.owner);
  uint64_t packed = PackKeyOffset(r.key.offset, r.key.flags);
  if (r.key.flags & kRmapUnwritten) packed |= kRmapOffsetUnwritten;
  PutBE64(p, 16, packed);
}

bool IgnoreOffset(uint64_t owner, uint32_t flags) {
  return RmapNonInodeOwner(owner) || (flags & kRmapBmbtBlock) != 0;
}

bool Mergeable(const RmapRecord& rec, uint64_t owner, uint32_t flags) {
  if (rec.key.owner == kRmapOwnerNull) return false;
  if (rec.key.owner != owner) return false;
  const uint32_t mask = kRmapAttrFork | kRmapBmbtBlock | kRmapUnwritten;
  return (rec.key.flags & mask) == (flags & mask);
}

bool LogicalAdjacent(const RmapRecord& rec, uint64_t owner, uint32_t flags,
                     uint64_t offset, uint32_t len, bool left) {
  if (IgnoreOffset(owner, flags)) return true;
  if (left) {
    return rec.key.offset + rec.blockcount == offset;
  }
  return offset + len == rec.key.offset;
}

Status RangeFits(const RmapRecord& rec, const XfsGeometry& geo, uint32_t agno) {
  return ValidateRmapRecord(rec, geo, agno);
}

}  // namespace

// ---------------------------------------------------------------------------
// Per-AG in-memory tree
// ---------------------------------------------------------------------------

class RmapEditor::Impl {
 public:
  struct Node {
    uint16_t level = 0;
    std::vector<uint8_t> data;
    bool dirty = false;
  };

  struct Ag {
    uint32_t agno = 0;
    uint32_t root = kNullAgBlock;
    uint32_t level = 0;   // AGF height (1 = leaf root)
    uint32_t blocks = 0;  // reachable blocks including root
    std::vector<RmapRecord> records;
    std::map<uint32_t, Node> nodes;
  };

  struct Frame {
    uint32_t parent = 0;
    size_t index = 0;
    uint32_t child = 0;
  };

  Impl(BlockDevice* device, const XfsGeometry* geo, Hooks hooks,
       RmapEditStats* stats)
      : device_(device), geo_(geo), hooks_(std::move(hooks)), stats_(stats) {}

  Status Load(uint32_t agno, uint32_t root, uint32_t level, uint32_t blocks) {
    auto existing = ags_.find(agno);
    if (existing != ags_.end()) return OkStatus();
    if (!geo_->features.rmapbt) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "filesystem does not have the rmapbt feature")
          .Ag(agno)
          .Metadata("rmapbt");
    }
    auto verified = LoadRmapTree(*device_, *geo_, agno);
    if (!verified.ok()) return verified.error();
    if (verified.value().root != root || verified.value().level != level ||
        verified.value().blocks != blocks) {
      return XFS_ERR(XfsError::kCorruptAgHeader,
                     "AGF rmap geometry changed while loading")
          .Ag(agno)
          .Metadata("rmapbt");
    }

    Ag ag;
    ag.agno = agno;
    ag.root = root;
    ag.level = level;
    ag.blocks = blocks;
    ag.records = verified.value().records;

    std::vector<std::pair<uint32_t, uint16_t>> stack;
    stack.push_back({root, static_cast<uint16_t>(level - 1)});
    std::set<uint32_t> seen;
    while (!stack.empty()) {
      const uint32_t agbno = stack.back().first;
      const uint16_t expected = stack.back().second;
      stack.pop_back();
      if (!seen.insert(agbno).second) {
        return XFS_ERR(XfsError::kCorruptBtree, "rmapbt cycle while loading")
            .Ag(agno)
            .Actual(agbno)
            .Metadata("rmapbt");
      }
      Node node;
      Status s = ReadNode(agno, agbno, &node);
      if (!s.ok()) return s;
      if (node.level != expected) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmapbt child level mismatch while loading")
            .Ag(agno)
            .Actual(agbno)
            .Metadata("rmapbt");
      }
      if (node.level > 0) {
        const size_t ptr_base = kHeader + static_cast<size_t>(max_node()) * kPairSize;
        const uint16_t n = NumRecs(node);
        for (uint16_t i = 0; i < n; ++i) {
          const uint32_t child = GetBE32(node.data.data(), ptr_base +
                                                           static_cast<size_t>(i) *
                                                               kPtrSize);
          stack.push_back({child, static_cast<uint16_t>(node.level - 1)});
        }
      }
      ag.nodes.emplace(agbno, std::move(node));
    }
    if (ag.nodes.size() != blocks) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmapbt block count changed while loading")
          .Ag(agno)
          .Expected(blocks)
          .Actual(ag.nodes.size())
          .Metadata("rmapbt");
    }
    ags_.emplace(agno, std::move(ag));
    return OkStatus();
  }

  bool IsLoaded(uint32_t agno) const { return ags_.count(agno) != 0; }

  Result<RmapRecord> LookupEq(uint32_t agno, const RmapKey& key) const {
    const Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissing(agno);
    auto it = std::lower_bound(ag->records.begin(), ag->records.end(), key,
                               [](const RmapRecord& r, const RmapKey& k) {
                                 return r.key.Compare(k) < 0;
                               });
    if (it == ag->records.end() || it->key.Compare(key) != 0) {
      return XFS_ERR(XfsError::kNotFound, "rmap key not found")
          .Ag(agno)
          .Actual(key.startblock)
          .Metadata("rmapbt");
    }
    return *it;
  }

  Result<RmapRecord> LookupLE(uint32_t agno, const RmapKey& key) const {
    const Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissing(agno);
    auto it = std::upper_bound(ag->records.begin(), ag->records.end(), key,
                               [](const RmapKey& k, const RmapRecord& r) {
                                 return k.Compare(r.key) < 0;
                               });
    if (it == ag->records.begin()) {
      return XFS_ERR(XfsError::kNotFound, "no rmap key at or below the target")
          .Ag(agno)
          .Actual(key.startblock)
          .Metadata("rmapbt");
    }
    return *(it - 1);
  }

  Result<RmapRecord> LookupGE(uint32_t agno, const RmapKey& key) const {
    const Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissing(agno);
    auto it = std::lower_bound(ag->records.begin(), ag->records.end(), key,
                               [](const RmapRecord& r, const RmapKey& k) {
                                 return r.key.Compare(k) < 0;
                               });
    if (it == ag->records.end()) {
      return XFS_ERR(XfsError::kNotFound, "no rmap key at or above the target")
          .Ag(agno)
          .Actual(key.startblock)
          .Metadata("rmapbt");
    }
    return *it;
  }

  Status InsertExact(uint32_t agno, const RmapRecord& rec) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    Status s = RangeFits(rec, *geo_, agno);
    if (!s.ok()) return s;
    size_t pos = LowerBound(*ag, rec.key);
    if (pos < ag->records.size() &&
        ag->records[pos].key.Compare(rec.key) == 0) {
      return XFS_ERR(XfsError::kCorruptBtree, "duplicate rmap key")
          .Ag(agno)
          .Actual(rec.key.startblock)
          .Metadata("rmapbt");
    }
    if (pos > 0 && ag->records[pos - 1].EndBlock() > rec.key.startblock) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmap insert overlaps previous")
          .Ag(agno)
          .Actual(rec.key.startblock)
          .Metadata("rmapbt");
    }
    if (pos < ag->records.size() &&
        rec.EndBlock() > ag->records[pos].key.startblock) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmap insert overlaps next")
          .Ag(agno)
          .Actual(rec.key.startblock)
          .Metadata("rmapbt");
    }
    s = TreeInsert(*ag, rec);
    if (!s.ok()) return s;
    ag->records.insert(ag->records.begin() + static_cast<ptrdiff_t>(pos), rec);
    ++stats_->inserted;
    return OkStatus();
  }

  Status DeleteExact(uint32_t agno, const RmapKey& key) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    auto it = std::lower_bound(ag->records.begin(), ag->records.end(), key,
                               [](const RmapRecord& r, const RmapKey& k) {
                                 return r.key.Compare(k) < 0;
                               });
    if (it == ag->records.end() || it->key.Compare(key) != 0) {
      return XFS_ERR(XfsError::kNotFound, "rmap key not found for delete")
          .Ag(agno)
          .Actual(key.startblock)
          .Metadata("rmapbt");
    }
    Status s = TreeDelete(*ag, key);
    if (!s.ok()) return s;
    ag->records.erase(it);
    ++stats_->deleted;
    return OkStatus();
  }

  Status UpdateExact(uint32_t agno, const RmapRecord& old_rec,
                     const RmapRecord& new_rec) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    Status s = RangeFits(new_rec, *geo_, agno);
    if (!s.ok()) return s;
    if (old_rec.key.Compare(new_rec.key) != 0) {
      s = DeleteExact(agno, old_rec.key);
      if (!s.ok()) return s;
      return InsertExact(agno, new_rec);
    }
    s = TreeReplace(*ag, old_rec.key, new_rec);
    if (!s.ok()) return s;
    auto it = std::lower_bound(ag->records.begin(), ag->records.end(),
                               old_rec.key,
                               [](const RmapRecord& r, const RmapKey& k) {
                                 return r.key.Compare(k) < 0;
                               });
    if (it == ag->records.end() || it->key.Compare(old_rec.key) != 0) {
      return XFS_ERR(XfsError::kInternal,
                     "rmap record list diverged from the tree")
          .Ag(agno)
          .Metadata("rmapbt");
    }
    *it = new_rec;
    return OkStatus();
  }

  Status MapExtent(uint32_t agno, const RmapRecord& rec) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    Status s = RangeFits(rec, *geo_, agno);
    if (!s.ok()) return s;
    if (rec.blockcount == 0) {
      return XFS_ERR(XfsError::kInvalidArgument, "zero-length rmap map")
          .Ag(agno)
          .Metadata("rmapbt");
    }

    const size_t pos = LowerBound(*ag, rec.key);
    const RmapRecord* left = pos > 0 ? &ag->records[pos - 1] : nullptr;
    const RmapRecord* right =
        pos < ag->records.size() ? &ag->records[pos] : nullptr;
    if (left != nullptr && left->EndBlock() > rec.key.startblock) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmap map overlaps left record")
          .Ag(agno)
          .Actual(rec.key.startblock)
          .Metadata("rmapbt");
    }
    if (right != nullptr && rec.EndBlock() > right->key.startblock) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmap map overlaps right record")
          .Ag(agno)
          .Actual(rec.key.startblock)
          .Metadata("rmapbt");
    }

    const bool have_left =
        left != nullptr && left->EndBlock() == rec.key.startblock &&
        Mergeable(*left, rec.key.owner, rec.key.flags) &&
        LogicalAdjacent(*left, rec.key.owner, rec.key.flags, rec.key.offset,
                        rec.blockcount, true);
    const bool have_right =
        right != nullptr && rec.EndBlock() == right->key.startblock &&
        Mergeable(*right, rec.key.owner, rec.key.flags) &&
        LogicalAdjacent(*right, rec.key.owner, rec.key.flags, rec.key.offset,
                        rec.blockcount, false);

    if (have_left && have_right) {
      const uint64_t total = static_cast<uint64_t>(left->blockcount) +
                             rec.blockcount + right->blockcount;
      if (total > 0xffffffffULL) {
        // Cannot merge both: fall back to merging the left side only, exactly
        // like the kernel's XFS_RMAP_LEN_MAX check.
        RmapRecord merged = *left;
        merged.blockcount += rec.blockcount;
        return UpdateExact(agno, *left, merged);
      }
      RmapRecord merged = *left;
      merged.blockcount = static_cast<uint32_t>(total);
      const RmapRecord old_left = *left;
      const RmapKey right_key = right->key;
      s = UpdateExact(agno, old_left, merged);
      if (!s.ok()) return s;
      return DeleteExact(agno, right_key);
    }
    if (have_left) {
      const uint64_t total =
          static_cast<uint64_t>(left->blockcount) + rec.blockcount;
      if (total > 0xffffffffULL) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       "rmap extent length exceeds 32 bits")
            .Ag(agno)
            .Metadata("rmapbt");
      }
      RmapRecord merged = *left;
      merged.blockcount = static_cast<uint32_t>(total);
      return UpdateExact(agno, *left, merged);
    }
    if (have_right) {
      const uint64_t total =
          static_cast<uint64_t>(rec.blockcount) + right->blockcount;
      if (total > 0xffffffffULL) {
        return XFS_ERR(XfsError::kInvalidArgument,
                       "rmap extent length exceeds 32 bits")
            .Ag(agno)
            .Metadata("rmapbt");
      }
      RmapRecord merged = *right;
      merged.key.startblock = rec.key.startblock;
      if (!IgnoreOffset(rec.key.owner, rec.key.flags)) {
        merged.key.offset = rec.key.offset;
      }
      merged.blockcount = static_cast<uint32_t>(total);
      return UpdateExact(agno, *right, merged);
    }
    return InsertExact(agno, rec);
  }

  Status UnmapExtent(uint32_t agno, const RmapKey& key, uint32_t len,
                     bool unknown_owner) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    if (len == 0) {
      return XFS_ERR(XfsError::kInvalidArgument, "zero-length rmap unmap")
          .Ag(agno)
          .Metadata("rmapbt");
    }
    const uint64_t end = static_cast<uint64_t>(key.startblock) + len;
    if (end > geo_->AgBlocksFor(agno)) {
      return XFS_ERR(XfsError::kOutOfRange, "rmap unmap leaves its AG")
          .Ag(agno)
          .Actual(key.startblock)
          .Metadata("rmapbt");
    }

    const RmapRecord* found = nullptr;
    for (auto it = ag->records.rbegin(); it != ag->records.rend(); ++it) {
      if (it->key.startblock > key.startblock) continue;
      if (it->EndBlock() <= key.startblock) break;
      if (!unknown_owner) {
        if (it->key.owner != key.owner) continue;
        if ((it->key.flags & kRmapUnwritten) !=
            (key.flags & kRmapUnwritten)) {
          continue;
        }
        if ((it->key.flags & kRmapKeyFlags) !=
            (key.flags & kRmapKeyFlags)) {
          continue;
        }
        if (!IgnoreOffset(it->key.owner, it->key.flags)) {
          const uint64_t expected =
              it->key.offset + (key.startblock - it->key.startblock);
          if (expected != key.offset) continue;
        }
      }
      if (it->EndBlock() < end) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmap unmap is only partially covered")
            .Ag(agno)
            .Actual(key.startblock)
            .Metadata("rmapbt");
      }
      found = &*it;
      break;
    }

    if (found == nullptr) {
      // OWN_UNKNOWN permits an idempotent EFI replay only when nothing at all
      // overlaps the range.  A normal unmap must find its owner.
      bool overlap = false;
      for (const RmapRecord& r : ag->records) {
        if (r.key.startblock < end && key.startblock < r.EndBlock()) {
          overlap = true;
          break;
        }
      }
      if (unknown_owner && !overlap) return OkStatus();
      return XFS_ERR(XfsError::kCorruptBtree,
                     unknown_owner ? "OWN_UNKNOWN unmap partially overlaps "
                                     "an existing rmap record"
                                   : "rmap unmap found no covering record")
          .Ag(agno)
          .Actual(key.startblock)
          .Metadata("rmapbt");
    }

    const RmapRecord rec = *found;
    const bool ignore_off = IgnoreOffset(rec.key.owner, rec.key.flags);
    if (rec.key.startblock == key.startblock && rec.blockcount == len) {
      return DeleteExact(agno, rec.key);
    }
    if (rec.key.startblock == key.startblock) {
      RmapRecord trimmed = rec;
      trimmed.key.startblock += len;
      trimmed.blockcount -= len;
      if (!ignore_off) trimmed.key.offset += len;
      return UpdateExact(agno, rec, trimmed);
    }
    if (rec.EndBlock() == end) {
      RmapRecord trimmed = rec;
      trimmed.blockcount -= len;
      return UpdateExact(agno, rec, trimmed);
    }
    RmapRecord lower = rec;
    lower.blockcount = key.startblock - rec.key.startblock;
    const RmapKey rec_key = rec.key;
    const uint64_t right_len = rec.EndBlock() - end;
    Status s = UpdateExact(agno, rec, lower);
    if (!s.ok()) return s;
    RmapRecord upper;
    upper.key = rec_key;
    upper.key.startblock = static_cast<uint32_t>(end);
    if (!ignore_off) {
      upper.key.offset = rec_key.offset + (end - rec_key.startblock);
    }
    upper.blockcount = static_cast<uint32_t>(right_len);
    return InsertExact(agno, upper);
  }

  Status ConvertExtent(uint32_t agno, const RmapKey& key, uint32_t len,
                       bool old_unwritten) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    if (len == 0) {
      return XFS_ERR(XfsError::kInvalidArgument, "zero-length rmap convert")
          .Ag(agno)
          .Metadata("rmapbt");
    }
    if (RmapNonInodeOwner(key.owner) ||
        (key.flags & (kRmapAttrFork | kRmapBmbtBlock)) != 0) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "rmap convert only applies to inode data extents")
          .Ag(agno)
          .Actual(key.owner)
          .Metadata("rmapbt");
    }
    const uint64_t end = static_cast<uint64_t>(key.startblock) + len;
    if (end > geo_->AgBlocksFor(agno)) {
      return XFS_ERR(XfsError::kOutOfRange, "rmap convert leaves its AG")
          .Ag(agno)
          .Actual(key.startblock)
          .Metadata("rmapbt");
    }

    const RmapRecord* found = nullptr;
    for (auto it = ag->records.rbegin(); it != ag->records.rend(); ++it) {
      if (it->key.startblock > key.startblock) continue;
      if (it->EndBlock() <= key.startblock) break;
      if (it->key.owner != key.owner) continue;
      if ((it->key.flags & kRmapKeyFlags) != 0) continue;
      if (it->unwritten() != old_unwritten) continue;
      const uint64_t expected =
          it->key.offset + (key.startblock - it->key.startblock);
      if (expected != key.offset) continue;
      if (it->EndBlock() < end) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmap convert range is only partially covered")
            .Ag(agno)
            .Actual(key.startblock)
            .Metadata("rmapbt");
      }
      found = &*it;
      break;
    }
    if (found == nullptr) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmap convert found no matching old-state record")
          .Ag(agno)
          .Actual(key.startblock)
          .Metadata("rmapbt");
    }

    const RmapRecord rec = *found;
    const uint32_t target_unwritten = old_unwritten ? 0u : kRmapUnwritten;
    Status s = DeleteExact(agno, rec.key);
    if (!s.ok()) return s;

    if (key.startblock > rec.key.startblock) {
      RmapRecord left = rec;
      left.blockcount = key.startblock - rec.key.startblock;
      s = MapExtent(agno, left);
      if (!s.ok()) return s;
    }
    if (end < rec.EndBlock()) {
      RmapRecord right = rec;
      right.key.startblock = static_cast<uint32_t>(end);
      right.key.offset = rec.key.offset + (end - rec.key.startblock);
      right.blockcount = static_cast<uint32_t>(rec.EndBlock() - end);
      s = MapExtent(agno, right);
      if (!s.ok()) return s;
    }
    RmapRecord target;
    target.key.startblock = key.startblock;
    target.key.owner = key.owner;
    target.key.offset = key.offset;
    target.key.flags = (rec.key.flags & kRmapKeyFlags) | target_unwritten;
    target.blockcount = len;
    return MapExtent(agno, target);
  }

  Status AllocateMetadata(uint32_t agno, uint32_t startblock, uint32_t len,
                          uint64_t owner) {
    if (!RmapNonInodeOwner(owner) || owner > kRmapOwnerFs ||
        owner < kRmapOwnerMin) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "invalid metadata owner for rmap")
          .Ag(agno)
          .Actual(owner)
          .Metadata("rmapbt");
    }
    RmapRecord rec;
    rec.key.startblock = startblock;
    rec.key.owner = owner;
    rec.blockcount = len;
    return MapExtent(agno, rec);
  }

  Status FreeMetadata(uint32_t agno, uint32_t startblock, uint32_t len,
                      uint64_t owner) {
    RmapKey key;
    key.startblock = startblock;
    key.owner = owner;
    return UnmapExtent(agno, key, len, false);
  }

  Status AllocateExtent(uint32_t agno, uint32_t startblock, uint32_t len,
                        const RmapOwner& owner) {
    RmapRecord rec;
    rec.key.startblock = startblock;
    rec.key.owner = owner.owner;
    rec.key.offset = owner.offset;
    rec.key.flags = owner.flags;
    rec.blockcount = len;
    return MapExtent(agno, rec);
  }

  Status FreeExtent(uint32_t agno, uint32_t startblock, uint32_t len,
                    const RmapOwner& owner, bool unknown_owner) {
    RmapKey key;
    key.startblock = startblock;
    key.owner = unknown_owner ? kRmapOwnerUnknown : owner.owner;
    key.offset = owner.offset;
    key.flags = owner.flags;
    return UnmapExtent(agno, key, len, unknown_owner);
  }

  Status Convert(uint32_t agno, uint32_t startblock, uint32_t len,
                 const RmapOwner& owner, bool old_unwritten) {
    RmapKey key;
    key.startblock = startblock;
    key.owner = owner.owner;
    key.offset = owner.offset;
    key.flags = owner.flags;
    return ConvertExtent(agno, key, len, old_unwritten);
  }

  uint32_t Root(uint32_t agno) const {
    const Ag* ag = FindAg(agno);
    return ag == nullptr ? kNullAgBlock : ag->root;
  }
  uint32_t Level(uint32_t agno) const {
    const Ag* ag = FindAg(agno);
    return ag == nullptr ? 0 : ag->level;
  }
  uint32_t Blocks(uint32_t agno) const {
    const Ag* ag = FindAg(agno);
    return ag == nullptr ? 0 : ag->blocks;
  }
  const std::vector<RmapRecord>& Records(uint32_t agno) const {
    static const std::vector<RmapRecord> kEmpty;
    const Ag* ag = FindAg(agno);
    return ag == nullptr ? kEmpty : ag->records;
  }

  uint32_t ReserveBlocks(uint32_t agno) const {
    const Ag* ag = FindAg(agno);
    const uint32_t height = ag == nullptr ? 1 : ag->level;
    const uint32_t maxlevels = bt::kMaxTraversalLevels;
    const uint32_t usable = std::min<uint32_t>(height + 1, maxlevels);
    const uint32_t reserve = 2 * usable >= 2 ? 2 * usable - 2 : 0;
    return std::max<uint32_t>(reserve, 2);
  }

  Status Commit() {
    for (auto& entry : ags_) {
      Ag& ag = entry.second;
      // Catch a malformed in-memory tree before any rmap buffer is logged.
      // This validates every block, key, sibling link and record, not just the
      // path that was edited.
      Status verified = Verify(ag.agno);
      if (!verified.ok()) return verified;
      for (auto& node_entry : ag.nodes) {
        Node& node = node_entry.second;
        if (!node.dirty) continue;
        Status s = hooks_.publish_block(
            ag.agno, node_entry.first, node.data.data(), node.data.size());
        if (!s.ok()) return s;
        node.dirty = false;
      }
    }
    return OkStatus();
  }

  Status Verify(uint32_t agno) const {
    const Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    if (ag->root == kNullAgBlock || ag->level == 0) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmap root is null")
          .Ag(agno)
          .Metadata("rmapbt");
    }
    std::set<uint32_t> visited;
    std::map<uint16_t, uint32_t> last_at_level;
    std::vector<RmapRecord> observed;
    RmapKey low;
    RmapKey high;
    Status s = VerifyVisit(*ag, ag->root,
                           static_cast<uint16_t>(ag->level - 1), true,
                           &visited, &last_at_level, &observed, &low, &high);
    if (!s.ok()) return s;
    if (visited.size() != ag->blocks) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmap block count disagrees with the tree")
          .Ag(agno)
          .Expected(ag->blocks)
          .Actual(visited.size())
          .Metadata("rmapbt");
    }
    if (observed.size() != ag->records.size()) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmap record list disagrees with the tree")
          .Ag(agno)
          .Expected(ag->records.size())
          .Actual(observed.size())
          .Metadata("rmapbt");
    }
    for (size_t i = 0; i < observed.size(); ++i) {
      if (observed[i].key.Compare(ag->records[i].key) != 0 ||
          observed[i].blockcount != ag->records[i].blockcount) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmap record list disagrees with the tree")
            .Ag(agno)
            .Actual(i)
            .Metadata("rmapbt");
      }
    }
    for (const auto& entry : last_at_level) {
      const Node& node = ag->nodes.at(entry.second);
      if (RightSib(node) != kNullAgBlock) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rightmost rmap block has a right sibling")
            .Ag(agno)
            .Actual(entry.second)
            .Metadata("rmapbt");
      }
    }
    return OkStatus();
  }

 private:
  static Result<RmapRecord> AgMissing(uint32_t agno) {
    return XFS_ERR(XfsError::kInternal, "rmap AG is not loaded")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  static Status AgMissingStatus(uint32_t agno) {
    return XFS_ERR(XfsError::kInternal, "rmap AG is not loaded")
        .Ag(agno)
        .Metadata("rmapbt");
  }

  Ag* FindAg(uint32_t agno) {
    auto it = ags_.find(agno);
    return it == ags_.end() ? nullptr : &it->second;
  }
  const Ag* FindAg(uint32_t agno) const {
    auto it = ags_.find(agno);
    return it == ags_.end() ? nullptr : &it->second;
  }

  uint32_t max_leaf() const {
    return static_cast<uint32_t>((geo_->block_size - kHeader) / kRecSize);
  }
  uint32_t max_node() const {
    return static_cast<uint32_t>((geo_->block_size - kHeader) /
                                 (kPairSize + kPtrSize));
  }

  Node& NodeAt(Ag& ag, uint32_t agbno) { return ag.nodes.at(agbno); }
  const Node& NodeAt(const Ag& ag, uint32_t agbno) const {
    return ag.nodes.at(agbno);
  }

  uint16_t NumRecs(const Node& node) const {
    return GetBE16(node.data.data(), bt::kNumrecs);
  }
  void SetNumRecs(Node& node, uint16_t n) {
    PutBE16(node.data.data(), bt::kNumrecs, n);
    node.dirty = true;
  }
  uint32_t LeftSib(const Node& node) const {
    return GetBE32(node.data.data(), bt::kSLeftsib);
  }
  uint32_t RightSib(const Node& node) const {
    return GetBE32(node.data.data(), bt::kSRightSib);
  }
  void SetLeftSib(Node& node, uint32_t v) {
    PutBE32(node.data.data(), bt::kSLeftsib, v);
    node.dirty = true;
  }
  void SetRightSib(Node& node, uint32_t v) {
    PutBE32(node.data.data(), bt::kSRightSib, v);
    node.dirty = true;
  }

  RmapRecord LeafRec(const Node& node, size_t i) const {
    return ParseRecord(node.data.data() + kHeader + i * kRecSize);
  }
  void SetLeafRec(Node& node, size_t i, const RmapRecord& r) {
    StoreRecord(node.data.data() + kHeader + i * kRecSize, r);
    node.dirty = true;
  }
  RmapKey NodeLow(const Node& node, size_t i) const {
    return ParseKey(node.data.data() + kHeader + i * kPairSize);
  }
  RmapKey NodeHigh(const Node& node, size_t i) const {
    return ParseKey(node.data.data() + kHeader + i * kPairSize + kKeySize);
  }
  uint32_t NodePtr(const Node& node, size_t i) const {
    const size_t base =
        kHeader + static_cast<size_t>(max_node()) * kPairSize;
    return GetBE32(node.data.data(), base + i * kPtrSize);
  }
  void SetNodeEntry(Node& node, size_t i, const RmapKey& low,
                    const RmapKey& high, uint32_t ptr) {
    uint8_t* p = node.data.data() + kHeader + i * kPairSize;
    StoreKey(p, low);
    StoreKey(p + kKeySize, high);
    const size_t base =
        kHeader + static_cast<size_t>(max_node()) * kPairSize;
    PutBE32(node.data.data(), base + i * kPtrSize, ptr);
    node.dirty = true;
  }

  Status ReadNode(uint32_t agno, uint32_t agbno, Node* out) const {
    const uint64_t agblocks = geo_->AgBlocksFor(agno);
    if (agbno == kNullAgBlock || agbno >= agblocks) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmap block out of range")
          .Ag(agno)
          .Actual(agbno)
          .Metadata("rmapbt");
    }
    Node node;
    node.data.assign(geo_->block_size, 0);
    const uint64_t offset = geo_->FsbToByte(geo_->AgFsb(agno, agbno));
    if (!device_->Read(offset, node.data.data(), node.data.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to read rmap block")
          .Ag(agno)
          .Actual(agbno)
          .Offset(offset)
          .Metadata("rmapbt");
    }
    if (GetBE32(node.data.data(), bt::kMagic) != ondisk::kRmapCrcMagic) {
      return XFS_ERR(XfsError::kBadMagic, "rmap block magic mismatch")
          .Ag(agno)
          .Actual(agbno)
          .Metadata("rmapbt");
    }
    node.level = GetBE16(node.data.data(), bt::kLevel);
    *out = std::move(node);
    return OkStatus();
  }

  Status InitNode(Ag& ag, uint32_t agbno, uint16_t level, uint16_t numrecs) {
    auto it = ag.nodes.find(agbno);
    if (it == ag.nodes.end()) {
      return XFS_ERR(XfsError::kInternal,
                     "initializing an unloaded rmap block")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("rmapbt");
    }
    Node& node = it->second;
    node.level = level;
    node.data.assign(geo_->block_size, 0);
    PutBE32(node.data.data(), bt::kMagic, ondisk::kRmapCrcMagic);
    PutBE16(node.data.data(), bt::kLevel, level);
    PutBE16(node.data.data(), bt::kNumrecs, numrecs);
    PutBE32(node.data.data(), bt::kSLeftsib, kNullAgBlock);
    PutBE32(node.data.data(), bt::kSRightSib, kNullAgBlock);
    PutBE64(node.data.data(), bt::kSBlkno,
            geo_->FsbToByte(geo_->AgFsb(ag.agno, agbno)) / 512);
    PutBE64(node.data.data(), bt::kSLsn, 0);  // stamped by publication
    std::memcpy(node.data.data() + bt::kSUuid, geo_->meta_uuid.data(), 16);
    PutBE32(node.data.data(), bt::kSOwner, ag.agno);
    StoreLE32(node.data.data() + bt::kSCrc, 0);
    node.dirty = true;
    return OkStatus();
  }

  Status AllocTreeBlock(Ag& ag, uint32_t* agbno) {
    uint32_t bno = kNullAgBlock;
    Status s = hooks_.alloc_block(ag.agno, &bno);
    if (!s.ok()) return s;
    if (bno == kNullAgBlock || bno >= geo_->AgBlocksFor(ag.agno)) {
      return XFS_ERR(XfsError::kCorruptAgHeader,
                     "rmap block allocation returned an invalid block")
          .Ag(ag.agno)
          .Actual(bno)
          .Metadata("rmapbt");
    }
    if (ag.nodes.count(bno) != 0) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmap block allocation returned a live tree block")
          .Ag(ag.agno)
          .Actual(bno)
          .Metadata("rmapbt");
    }
    Node node;
    node.level = 0;
    node.data.assign(geo_->block_size, 0);
    ag.nodes.emplace(bno, std::move(node));
    ++ag.blocks;
    *agbno = bno;
    return OkStatus();
  }

  Status FreeTreeBlock(Ag& ag, uint32_t agbno) {
    Status s = hooks_.free_block(ag.agno, agbno);
    if (!s.ok()) return s;
    ag.nodes.erase(agbno);
    if (ag.blocks == 0) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmap block count underflow")
          .Ag(ag.agno)
          .Metadata("rmapbt");
    }
    --ag.blocks;
    return OkStatus();
  }

  size_t LowerBound(const Ag& ag, const RmapKey& key) const {
    return static_cast<size_t>(
        std::lower_bound(ag.records.begin(), ag.records.end(), key,
                         [](const RmapRecord& r, const RmapKey& k) {
                           return r.key.Compare(k) < 0;
                         }) -
        ag.records.begin());
  }

  void ComputeKeys(const Ag& ag, uint32_t agbno, RmapKey* low,
                   RmapKey* high) const {
    const Node& node = NodeAt(ag, agbno);
    if (node.level == 0) {
      const uint16_t n = NumRecs(node);
      *low = LeafRec(node, 0).key;
      *high = RmapRecordHighKey(LeafRec(node, 0));
      for (uint16_t i = 1; i < n; ++i) {
        const RmapKey h = RmapRecordHighKey(LeafRec(node, i));
        if (h.Compare(*high) > 0) *high = h;
      }
      return;
    }
    const uint16_t n = NumRecs(node);
    *low = NodeLow(node, 0);
    *high = NodeHigh(node, 0);
    for (uint16_t i = 1; i < n; ++i) {
      const RmapKey h = NodeHigh(node, i);
      if (h.Compare(*high) > 0) *high = h;
    }
  }

  void RefreshEntry(Ag& ag, uint32_t parent, size_t index) {
    Node& p = NodeAt(ag, parent);
    const uint32_t child = NodePtr(p, index);
    RmapKey low;
    RmapKey high;
    ComputeKeys(ag, child, &low, &high);
    SetNodeEntry(p, index, low, high, child);
  }

  void RefreshAncestors(Ag& ag, const std::vector<Frame>& path) {
    for (size_t i = path.size(); i > 0; --i) {
      const Frame& f = path[i - 1];
      RefreshEntry(ag, f.parent, f.index);
    }
  }

  Status Descend(Ag& ag, const RmapKey& key, std::vector<Frame>* path,
                 uint32_t* leaf) {
    uint32_t cur = ag.root;
    if (ag.nodes.count(cur) == 0) {
      return XFS_ERR(XfsError::kInternal, "rmap root block missing")
          .Ag(ag.agno)
          .Actual(cur)
          .Metadata("rmapbt");
    }
    while (NodeAt(ag, cur).level > 0) {
      Node& node = NodeAt(ag, cur);
      const uint16_t n = NumRecs(node);
      if (n == 0) {
        return XFS_ERR(XfsError::kCorruptBtree, "empty rmap node")
            .Ag(ag.agno)
            .Actual(cur)
            .Metadata("rmapbt");
      }
      size_t chosen = 0;
      for (size_t i = 0; i < n; ++i) {
        if (NodeLow(node, i).Compare(key) <= 0) {
          chosen = i;
        } else {
          break;
        }
      }
      const uint32_t child = NodePtr(node, chosen);
      path->push_back(Frame{cur, chosen, child});
      cur = child;
      if (ag.nodes.count(cur) == 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmap child block missing")
            .Ag(ag.agno)
            .Actual(cur)
            .Metadata("rmapbt");
      }
    }
    *leaf = cur;
    return OkStatus();
  }

  Status TreeInsert(Ag& ag, const RmapRecord& rec) {
    std::vector<Frame> path;
    uint32_t leaf = 0;
    Status s = Descend(ag, rec.key, &path, &leaf);
    if (!s.ok()) return s;
    Node& node = NodeAt(ag, leaf);
    const uint16_t n = NumRecs(node);
    if (n < max_leaf()) {
      size_t pos = 0;
      while (pos < n && LeafRec(node, pos).key.Compare(rec.key) < 0) ++pos;
      if (pos < n && LeafRec(node, pos).key.Compare(rec.key) == 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmap key already present in the leaf")
            .Ag(ag.agno)
            .Actual(rec.key.startblock)
            .Metadata("rmapbt");
      }
      for (size_t i = n; i > pos; --i) {
        SetLeafRec(node, i, LeafRec(node, i - 1));
      }
      SetLeafRec(node, pos, rec);
      SetNumRecs(node, static_cast<uint16_t>(n + 1));
      RefreshAncestors(ag, path);
      return OkStatus();
    }

    std::vector<RmapRecord> entries;
    entries.reserve(static_cast<size_t>(n) + 1);
    size_t pos = 0;
    while (pos < n && LeafRec(node, pos).key.Compare(rec.key) < 0) ++pos;
    for (size_t i = 0; i < pos; ++i) entries.push_back(LeafRec(node, i));
    entries.push_back(rec);
    for (size_t i = pos; i < n; ++i) entries.push_back(LeafRec(node, i));

    const size_t left_count = entries.size() / 2;
    uint32_t right = kNullAgBlock;
    s = AllocTreeBlock(ag, &right);
    if (!s.ok()) return s;
    s = InitNode(ag, right, 0, static_cast<uint16_t>(entries.size() - left_count));
    if (!s.ok()) return s;
    {
      Node& left = NodeAt(ag, leaf);
      Node& rb = NodeAt(ag, right);
      SetNumRecs(left, static_cast<uint16_t>(left_count));
      for (size_t i = 0; i < left_count; ++i) SetLeafRec(left, i, entries[i]);
      for (size_t i = left_count; i < entries.size(); ++i) {
        SetLeafRec(rb, i - left_count, entries[i]);
      }
      const uint32_t old_right = RightSib(left);
      SetRightSib(left, right);
      SetLeftSib(rb, leaf);
      SetRightSib(rb, old_right);
      if (old_right != kNullAgBlock) SetLeftSib(NodeAt(ag, old_right), right);
    }
    ++stats_->split_leaf;

    RmapKey left_low;
    RmapKey left_high;
    ComputeKeys(ag, leaf, &left_low, &left_high);
    RmapKey right_low;
    RmapKey right_high;
    ComputeKeys(ag, right, &right_low, &right_high);
    return InsertChild(ag, path, leaf, left_low, left_high, right, right_low,
                       right_high);
  }

  Status InsertChild(Ag& ag, std::vector<Frame>& path, uint32_t left,
                     const RmapKey& left_low, const RmapKey& left_high,
                     uint32_t right, const RmapKey& right_low,
                     const RmapKey& right_high) {
    if (path.empty()) {
      uint32_t new_root = kNullAgBlock;
      Status s = AllocTreeBlock(ag, &new_root);
      if (!s.ok()) return s;
      s = InitNode(ag, new_root, static_cast<uint16_t>(ag.level), 2);
      if (!s.ok()) return s;
      Node& root = NodeAt(ag, new_root);
      SetNodeEntry(root, 0, left_low, left_high, left);
      SetNodeEntry(root, 1, right_low, right_high, right);
      ag.root = new_root;
      ++ag.level;
      ++stats_->root_grow;
      return OkStatus();
    }

    Frame frame = path.back();
    path.pop_back();
    Node& parent = NodeAt(ag, frame.parent);
    if (frame.index >= NumRecs(parent)) {
      return XFS_ERR(XfsError::kInternal, "rmap parent index is stale")
          .Ag(ag.agno)
          .Actual(frame.parent)
          .Metadata("rmapbt");
    }
    SetNodeEntry(parent, frame.index, left_low, left_high, left);
    const uint16_t pn = NumRecs(parent);
    if (pn < max_node()) {
      const size_t insert_at = frame.index + 1;
      for (size_t i = pn; i > insert_at; --i) {
        SetNodeEntry(parent, i, NodeLow(parent, i - 1),
                     NodeHigh(parent, i - 1), NodePtr(parent, i - 1));
      }
      SetNodeEntry(parent, insert_at, right_low, right_high, right);
      SetNumRecs(parent, static_cast<uint16_t>(pn + 1));
      RefreshAncestors(ag, path);
      return OkStatus();
    }

    std::vector<std::pair<RmapKey, RmapKey>> keys;
    std::vector<uint32_t> ptrs;
    keys.reserve(static_cast<size_t>(pn) + 1);
    ptrs.reserve(static_cast<size_t>(pn) + 1);
    const size_t insert_at = frame.index + 1;
    for (size_t i = 0; i < insert_at; ++i) {
      keys.push_back({NodeLow(parent, i), NodeHigh(parent, i)});
      ptrs.push_back(NodePtr(parent, i));
    }
    keys.push_back({right_low, right_high});
    ptrs.push_back(right);
    for (size_t i = insert_at; i < pn; ++i) {
      keys.push_back({NodeLow(parent, i), NodeHigh(parent, i)});
      ptrs.push_back(NodePtr(parent, i));
    }
    const size_t left_count = keys.size() / 2;
    uint32_t right_node = kNullAgBlock;
    Status s = AllocTreeBlock(ag, &right_node);
    if (!s.ok()) return s;
    s = InitNode(ag, right_node, parent.level,
                 static_cast<uint16_t>(keys.size() - left_count));
    if (!s.ok()) return s;
    {
      Node& lb = NodeAt(ag, frame.parent);
      Node& rb = NodeAt(ag, right_node);
      SetNumRecs(lb, static_cast<uint16_t>(left_count));
      for (size_t i = 0; i < left_count; ++i) {
        SetNodeEntry(lb, i, keys[i].first, keys[i].second, ptrs[i]);
      }
      for (size_t i = left_count; i < keys.size(); ++i) {
        SetNodeEntry(rb, i - left_count, keys[i].first, keys[i].second,
                     ptrs[i]);
      }
      const uint32_t old_right = RightSib(lb);
      SetRightSib(lb, right_node);
      SetLeftSib(rb, frame.parent);
      SetRightSib(rb, old_right);
      if (old_right != kNullAgBlock) {
        SetLeftSib(NodeAt(ag, old_right), right_node);
      }
    }
    ++stats_->split_node;

    RmapKey nl;
    RmapKey nh;
    ComputeKeys(ag, frame.parent, &nl, &nh);
    RmapKey rl;
    RmapKey rh;
    ComputeKeys(ag, right_node, &rl, &rh);
    return InsertChild(ag, path, frame.parent, nl, nh, right_node, rl, rh);
  }

  Status TreeReplace(Ag& ag, const RmapKey& key, const RmapRecord& rec) {
    std::vector<Frame> path;
    uint32_t leaf = 0;
    Status s = Descend(ag, key, &path, &leaf);
    if (!s.ok()) return s;
    Node& node = NodeAt(ag, leaf);
    const uint16_t n = NumRecs(node);
    for (uint16_t i = 0; i < n; ++i) {
      if (LeafRec(node, i).key.Compare(key) == 0) {
        SetLeafRec(node, i, rec);
        RefreshAncestors(ag, path);
        return OkStatus();
      }
    }
    return XFS_ERR(XfsError::kNotFound,
                   "rmap key not found for in-place update")
        .Ag(ag.agno)
        .Actual(key.startblock)
        .Metadata("rmapbt");
  }

  Status TreeDelete(Ag& ag, const RmapKey& key) {
    std::vector<Frame> path;
    uint32_t leaf = 0;
    Status s = Descend(ag, key, &path, &leaf);
    if (!s.ok()) return s;
    Node& node = NodeAt(ag, leaf);
    const uint16_t n = NumRecs(node);
    size_t pos = n;
    for (size_t i = 0; i < n; ++i) {
      if (LeafRec(node, i).key.Compare(key) == 0) {
        pos = i;
        break;
      }
    }
    if (pos == n) {
      return XFS_ERR(XfsError::kNotFound, "rmap key not found in leaf")
          .Ag(ag.agno)
          .Actual(key.startblock)
          .Metadata("rmapbt");
    }
    for (size_t i = pos; i + 1 < n; ++i) {
      SetLeafRec(node, i, LeafRec(node, i + 1));
    }
    SetNumRecs(node, static_cast<uint16_t>(n - 1));

    if (leaf == ag.root) {
      if (n - 1 == 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "refusing to leave the rmap root empty")
            .Ag(ag.agno)
            .Metadata("rmapbt");
      }
      return OkStatus();
    }
    RefreshAncestors(ag, path);
    return Rebalance(ag, path);
  }

  Status Rebalance(Ag& ag, std::vector<Frame>& path) {
    while (!path.empty()) {
      Frame frame = path.back();
      Node& cur_node = NodeAt(ag, frame.child);
      const uint16_t cur_level = cur_node.level;
      const uint32_t maxr = cur_level == 0 ? max_leaf() : max_node();
      const uint32_t minr = maxr / 2;
      if (NumRecs(cur_node) >= minr) {
        RefreshAncestors(ag, path);
        break;
      }

      Node& parent = NodeAt(ag, frame.parent);
      const uint16_t pn = NumRecs(parent);
      if (frame.index >= pn) {
        return XFS_ERR(XfsError::kInternal, "rmap parent index is stale")
            .Ag(ag.agno)
            .Actual(frame.parent)
            .Metadata("rmapbt");
      }
      const uint32_t left =
          frame.index > 0 ? NodePtr(parent, frame.index - 1) : kNullAgBlock;
      const uint32_t right = frame.index + 1 < pn
                                 ? NodePtr(parent, frame.index + 1)
                                 : kNullAgBlock;

      if (left != kNullAgBlock &&
          NumRecs(NodeAt(ag, left)) > minr) {
        BorrowLast(ag, left, frame.child);
        ++stats_->borrow_leaf;
        if (frame.index > 0) RefreshEntry(ag, frame.parent, frame.index - 1);
        RefreshEntry(ag, frame.parent, frame.index);
        path.pop_back();
        RefreshAncestors(ag, path);
        break;
      }
      if (right != kNullAgBlock &&
          NumRecs(NodeAt(ag, right)) > minr) {
        BorrowFirst(ag, right, frame.child);
        ++stats_->borrow_leaf;
        RefreshEntry(ag, frame.parent, frame.index);
        RefreshEntry(ag, frame.parent, frame.index + 1);
        path.pop_back();
        RefreshAncestors(ag, path);
        break;
      }

      if (left != kNullAgBlock) {
        Node& lb = NodeAt(ag, left);
        Node& cb = NodeAt(ag, frame.child);
        if (static_cast<uint32_t>(NumRecs(lb)) + NumRecs(cb) > maxr) {
          return XFS_ERR(XfsError::kUnsupportedFeature,
                         "rmap sibling merge would exceed a block")
              .Ag(ag.agno)
              .Actual(frame.child)
              .Metadata("rmapbt");
        }
        AppendNode(lb, cb);
        const uint32_t rr = RightSib(cb);
        SetRightSib(lb, rr);
        if (rr != kNullAgBlock) SetLeftSib(NodeAt(ag, rr), left);
        if (cur_level == 0) {
          ++stats_->merge_leaf;
        } else {
          ++stats_->merge_node;
        }
        Status s = FreeTreeBlock(ag, frame.child);
        if (!s.ok()) return s;
        RemoveParentEntry(parent, frame.index);
        path.pop_back();
        if (path.empty()) {
          // Parent is the root; check for collapse below.
          if (NumRecs(parent) == 1 && ag.level > 1) {
            return CollapseRoot(ag);
          }
          return OkStatus();
        }
        if (NumRecs(parent) == 0) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "rmap node lost all its children")
              .Ag(ag.agno)
              .Actual(frame.parent)
              .Metadata("rmapbt");
        }
        continue;
      }

      if (right != kNullAgBlock) {
        Node& cb = NodeAt(ag, frame.child);
        Node& rb = NodeAt(ag, right);
        if (static_cast<uint32_t>(NumRecs(cb)) + NumRecs(rb) > maxr) {
          return XFS_ERR(XfsError::kUnsupportedFeature,
                         "rmap sibling merge would exceed a block")
              .Ag(ag.agno)
              .Actual(frame.child)
              .Metadata("rmapbt");
        }
        AppendNode(cb, rb);
        const uint32_t rr = RightSib(rb);
        SetRightSib(cb, rr);
        if (rr != kNullAgBlock) SetLeftSib(NodeAt(ag, rr), frame.child);
        if (cur_level == 0) {
          ++stats_->merge_leaf;
        } else {
          ++stats_->merge_node;
        }
        Status s = FreeTreeBlock(ag, right);
        if (!s.ok()) return s;
        RemoveParentEntry(parent, frame.index + 1);
        path.pop_back();
        if (path.empty()) {
          if (NumRecs(parent) == 1 && ag.level > 1) {
            return CollapseRoot(ag);
          }
          return OkStatus();
        }
        continue;
      }

      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "rmap block underflow has no sibling to rebalance")
          .Ag(ag.agno)
          .Actual(frame.child)
          .Metadata("rmapbt");
    }

    if (ag.level > 1 && NumRecs(NodeAt(ag, ag.root)) == 1) {
      return CollapseRoot(ag);
    }
    return OkStatus();
  }

  void AppendNode(Node& dst, const Node& src) {
    const uint16_t dn = NumRecs(dst);
    const uint16_t sn = NumRecs(src);
    if (src.level == 0) {
      for (uint16_t i = 0; i < sn; ++i) {
        SetLeafRec(dst, dn + i, LeafRec(src, i));
      }
    } else {
      for (uint16_t i = 0; i < sn; ++i) {
        SetNodeEntry(dst, dn + i, NodeLow(src, i), NodeHigh(src, i),
                     NodePtr(src, i));
      }
    }
    SetNumRecs(dst, static_cast<uint16_t>(dn + sn));
  }

  void BorrowLast(Ag& ag, uint32_t from, uint32_t to) {
    Node& src = NodeAt(ag, from);
    Node& dst = NodeAt(ag, to);
    const uint16_t sn = NumRecs(src);
    const uint16_t dn = NumRecs(dst);
    if (sn == 0) return;
    if (src.level == 0) {
      for (uint16_t i = dn; i > 0; --i) {
        SetLeafRec(dst, i, LeafRec(dst, i - 1));
      }
      SetLeafRec(dst, 0, LeafRec(src, sn - 1));
    } else {
      for (uint16_t i = dn; i > 0; --i) {
        SetNodeEntry(dst, i, NodeLow(dst, i - 1), NodeHigh(dst, i - 1),
                     NodePtr(dst, i - 1));
      }
      SetNodeEntry(dst, 0, NodeLow(src, sn - 1), NodeHigh(src, sn - 1),
                   NodePtr(src, sn - 1));
    }
    SetNumRecs(src, static_cast<uint16_t>(sn - 1));
    SetNumRecs(dst, static_cast<uint16_t>(dn + 1));
  }

  void BorrowFirst(Ag& ag, uint32_t from, uint32_t to) {
    Node& src = NodeAt(ag, from);
    Node& dst = NodeAt(ag, to);
    const uint16_t sn = NumRecs(src);
    const uint16_t dn = NumRecs(dst);
    if (sn == 0) return;
    if (src.level == 0) {
      SetLeafRec(dst, dn, LeafRec(src, 0));
      for (uint16_t i = 0; i + 1 < sn; ++i) {
        SetLeafRec(src, i, LeafRec(src, i + 1));
      }
    } else {
      SetNodeEntry(dst, dn, NodeLow(src, 0), NodeHigh(src, 0),
                   NodePtr(src, 0));
      for (uint16_t i = 0; i + 1 < sn; ++i) {
        SetNodeEntry(src, i, NodeLow(src, i + 1), NodeHigh(src, i + 1),
                     NodePtr(src, i + 1));
      }
    }
    SetNumRecs(src, static_cast<uint16_t>(sn - 1));
    SetNumRecs(dst, static_cast<uint16_t>(dn + 1));
  }

  void RemoveParentEntry(Node& parent, size_t index) {
    const uint16_t pn = NumRecs(parent);
    if (index >= pn) return;
    for (size_t i = index; i + 1 < pn; ++i) {
      SetNodeEntry(parent, i, NodeLow(parent, i + 1),
                   NodeHigh(parent, i + 1), NodePtr(parent, i + 1));
    }
    SetNumRecs(parent, static_cast<uint16_t>(pn - 1));
  }

  Status CollapseRoot(Ag& ag) {
    if (ag.level <= 1) return OkStatus();
    Node& root = NodeAt(ag, ag.root);
    if (NumRecs(root) != 1) return OkStatus();
    const uint32_t child = NodePtr(root, 0);
    if (ag.nodes.count(child) == 0) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmap root child is missing during collapse")
          .Ag(ag.agno)
          .Actual(child)
          .Metadata("rmapbt");
    }
    const uint32_t old_root = ag.root;
    ag.root = child;
    --ag.level;
    SetLeftSib(NodeAt(ag, child), kNullAgBlock);
    SetRightSib(NodeAt(ag, child), kNullAgBlock);
    Status s = FreeTreeBlock(ag, old_root);
    if (!s.ok()) return s;
    ++stats_->root_collapse;
    if (ag.level > 1 && NumRecs(NodeAt(ag, ag.root)) == 1) {
      return CollapseRoot(ag);
    }
    return OkStatus();
  }

  Status VerifyVisit(const Ag& ag, uint32_t agbno, uint16_t expected_level,
                     bool root, std::set<uint32_t>* visited,
                     std::map<uint16_t, uint32_t>* last_at_level,
                     std::vector<RmapRecord>* observed, RmapKey* low_out,
                     RmapKey* high_out) const {
    if (agbno == kNullAgBlock || agbno >= geo_->AgBlocksFor(ag.agno)) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmap child pointer out of range")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("rmapbt");
    }
    if (!visited->insert(agbno).second) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmap tree cycle")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("rmapbt");
    }
    auto it = ag.nodes.find(agbno);
    if (it == ag.nodes.end()) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmap block missing")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("rmapbt");
    }
    const Node& node = it->second;
    if (node.level != expected_level) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmap block level mismatch")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("rmapbt");
    }
    const uint16_t n = NumRecs(node);
    const uint32_t maxr = node.level == 0 ? max_leaf() : max_node();
    if (n == 0 || n > maxr) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "rmap block record count out of range")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("rmapbt");
    }
    if (!root && n < maxr / 2) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "non-root rmap block is below half full")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("rmapbt");
    }
    const uint32_t leftsib = LeftSib(node);
    auto last = last_at_level->find(node.level);
    if (last == last_at_level->end()) {
      if (leftsib != kNullAgBlock) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "leftmost rmap block has a left sibling")
            .Ag(ag.agno)
            .Actual(agbno)
            .Metadata("rmapbt");
      }
    } else if (last->second != leftsib ||
               RightSib(ag.nodes.at(last->second)) != agbno) {
      return XFS_ERR(XfsError::kCorruptBtree, "rmap sibling link mismatch")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("rmapbt");
    }
    (*last_at_level)[node.level] = agbno;

    if (node.level == 0) {
      const RmapRecord first = LeafRec(node, 0);
      *low_out = first.key;
      *high_out = RmapRecordHighKey(first);
      for (uint16_t i = 0; i < n; ++i) {
        const RmapRecord rec = LeafRec(node, i);
        Status s = ValidateRmapRecord(rec, *geo_, ag.agno);
        if (!s.ok()) return s;
        if (!observed->empty() &&
            observed->back().key.Compare(rec.key) >= 0) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "rmap records are not strictly ordered")
              .Ag(ag.agno)
              .Actual(rec.key.startblock)
              .Metadata("rmapbt");
        }
        const RmapKey high = RmapRecordHighKey(rec);
        if (high.Compare(*high_out) > 0) *high_out = high;
        observed->push_back(rec);
      }
      return OkStatus();
    }

    bool first = true;
    RmapKey previous_low;
    for (uint16_t i = 0; i < n; ++i) {
      const RmapKey low = NodeLow(node, i);
      const RmapKey high = NodeHigh(node, i);
      if (low.Compare(high) > 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmap node low key exceeds high key")
            .Ag(ag.agno)
            .Actual(agbno)
            .Metadata("rmapbt");
      }
      if (!first && previous_low.Compare(low) > 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmap node keys are out of order")
            .Ag(ag.agno)
            .Actual(agbno)
            .Metadata("rmapbt");
      }
      previous_low = low;
      const uint32_t child = NodePtr(node, i);
      RmapKey child_low;
      RmapKey child_high;
      Status s = VerifyVisit(ag, child, static_cast<uint16_t>(node.level - 1),
                             false, visited, last_at_level, observed,
                             &child_low, &child_high);
      if (!s.ok()) return s;
      if (child_low.Compare(low) != 0 || child_high.Compare(high) != 0) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rmap parent key does not equal its child subtree")
            .Ag(ag.agno)
            .Actual(agbno)
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

  BlockDevice* device_;
  const XfsGeometry* geo_;
  Hooks hooks_;
  RmapEditStats* stats_;
  std::map<uint32_t, Ag> ags_;
};

// ---------------------------------------------------------------------------
// Public facade
// ---------------------------------------------------------------------------

RmapEditor::RmapEditor(BlockDevice* device, const XfsGeometry* geo, Hooks hooks)
    : impl_(std::make_unique<Impl>(device, geo, std::move(hooks), &stats_)) {}

RmapEditor::~RmapEditor() = default;

Status RmapEditor::Load(uint32_t agno, uint32_t root, uint32_t level,
                        uint32_t blocks) {
  return impl_->Load(agno, root, level, blocks);
}
bool RmapEditor::IsLoaded(uint32_t agno) const {
  return impl_->IsLoaded(agno);
}
Result<RmapRecord> RmapEditor::LookupEq(uint32_t agno,
                                        const RmapKey& key) const {
  return impl_->LookupEq(agno, key);
}
Result<RmapRecord> RmapEditor::LookupLE(uint32_t agno,
                                        const RmapKey& key) const {
  return impl_->LookupLE(agno, key);
}
Result<RmapRecord> RmapEditor::LookupGE(uint32_t agno,
                                        const RmapKey& key) const {
  return impl_->LookupGE(agno, key);
}
Status RmapEditor::InsertExact(uint32_t agno, const RmapRecord& rec) {
  return impl_->InsertExact(agno, rec);
}
Status RmapEditor::DeleteExact(uint32_t agno, const RmapKey& key) {
  return impl_->DeleteExact(agno, key);
}
Status RmapEditor::UpdateExact(uint32_t agno, const RmapRecord& old_rec,
                               const RmapRecord& new_rec) {
  return impl_->UpdateExact(agno, old_rec, new_rec);
}
Status RmapEditor::MapExtent(uint32_t agno, const RmapRecord& rec) {
  return impl_->MapExtent(agno, rec);
}
Status RmapEditor::UnmapExtent(uint32_t agno, const RmapKey& key, uint32_t len,
                               bool unknown_owner) {
  return impl_->UnmapExtent(agno, key, len, unknown_owner);
}
Status RmapEditor::ConvertExtent(uint32_t agno, const RmapKey& key,
                                 uint32_t len, bool old_unwritten) {
  return impl_->ConvertExtent(agno, key, len, old_unwritten);
}
Status RmapEditor::AllocateMetadata(uint32_t agno, uint32_t startblock,
                                    uint32_t len, uint64_t owner) {
  return impl_->AllocateMetadata(agno, startblock, len, owner);
}
Status RmapEditor::FreeMetadata(uint32_t agno, uint32_t startblock,
                                uint32_t len, uint64_t owner) {
  return impl_->FreeMetadata(agno, startblock, len, owner);
}
Status RmapEditor::AllocateExtent(uint32_t agno, uint32_t startblock,
                                  uint32_t len, const RmapOwner& owner) {
  return impl_->AllocateExtent(agno, startblock, len, owner);
}
Status RmapEditor::FreeExtent(uint32_t agno, uint32_t startblock, uint32_t len,
                              const RmapOwner& owner, bool unknown_owner) {
  return impl_->FreeExtent(agno, startblock, len, owner, unknown_owner);
}
Status RmapEditor::Convert(uint32_t agno, uint32_t startblock, uint32_t len,
                           const RmapOwner& owner, bool old_unwritten) {
  return impl_->Convert(agno, startblock, len, owner, old_unwritten);
}
uint32_t RmapEditor::Root(uint32_t agno) const { return impl_->Root(agno); }
uint32_t RmapEditor::Level(uint32_t agno) const { return impl_->Level(agno); }
uint32_t RmapEditor::Blocks(uint32_t agno) const { return impl_->Blocks(agno); }
const std::vector<RmapRecord>& RmapEditor::Records(uint32_t agno) const {
  return impl_->Records(agno);
}
uint32_t RmapEditor::ReserveBlocks(uint32_t agno) const {
  return impl_->ReserveBlocks(agno);
}
Status RmapEditor::Commit() { return impl_->Commit(); }
Status RmapEditor::Verify(uint32_t agno) const { return impl_->Verify(agno); }

}  // namespace xfs
