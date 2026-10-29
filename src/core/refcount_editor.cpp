// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Writable refcountbt editor (RW-6C).  See docs/REFLINK.md.
//
// Two layers:
//   * logical reference-count actions (Increase/Decrease/CowAlloc/CowFree)
//     follow xfs_refcount_adjust and xfs_refcount_adjust_cow: split at both
//     range boundaries, synthesize implied refcount-1 extents, modify the
//     middle, free blocks whose last reference disappeared, then merge
//     eligible neighbours;
//   * the physical short-form B+tree implements leaf/internal insert, delete,
//     split, merge and borrow, sibling links, root growth and collapse.
#include "xfs/refcount_editor.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"

namespace xfs {
namespace {

namespace bt = ondisk::btree;

constexpr uint32_t kNullAgBlock = bt::kNullAgBlock;
constexpr size_t kHeader = bt::kSHeaderLenV5;
constexpr size_t kKeySize = bt::kRefcountKeySize;
constexpr size_t kRecSize = bt::kRefcountRecSize;
constexpr size_t kPtrSize = bt::kRefcountPtrSize;

RefcountRecord ParseRecord(const uint8_t* p) {
  RefcountRecord r;
  r.key.startblock = GetBE32(p) & kRefcountAgBlockMask;
  r.key.domain = (GetBE32(p) & kRefcountCowFlag) ? RefcountDomain::kCow
                                                 : RefcountDomain::kShared;
  r.blockcount = GetBE32(p + 4);
  r.refcount = GetBE32(p + 8);
  return r;
}

void StoreRecord(uint8_t* p, const RefcountRecord& r) {
  PutBE32(p, 0, r.key.Encoded());
  PutBE32(p, 4, r.blockcount);
  PutBE32(p, 8, r.refcount);
}

bool SameRecord(const RefcountRecord& a, const RefcountRecord& b) {
  return a.key.Compare(b.key) == 0 && a.blockcount == b.blockcount &&
         a.refcount == b.refcount;
}

bool PhysicallyAdjacent(const RefcountRecord& left,
                        const RefcountRecord& right) {
  return left.key.domain == right.key.domain &&
         left.key.startblock + left.blockcount == right.key.startblock;
}

bool Mergeable(const RefcountRecord& left, const RefcountRecord& right) {
  return PhysicallyAdjacent(left, right) &&
         left.refcount == right.refcount &&
         static_cast<uint64_t>(left.blockcount) + right.blockcount <
             kRefcountLenMax;
}

}  // namespace

class RefcountEditor::Impl {
 public:
  struct Node {
    uint16_t level = 0;
    std::vector<uint8_t> data;
    bool dirty = false;
  };

  struct Ag {
    uint32_t agno = 0;
    uint32_t root = kNullAgBlock;
    uint32_t level = 0;
    uint32_t blocks = 0;
    std::vector<RefcountRecord> records;
    std::map<uint32_t, Node> nodes;
  };

  struct Frame {
    uint32_t parent = 0;
    size_t index = 0;
    uint32_t child = 0;
  };

  Impl(BlockDevice* device, const XfsGeometry* geo, Hooks hooks,
       RefcountEditStats* stats)
      : device_(device), geo_(geo), hooks_(std::move(hooks)), stats_(stats) {}

  Status Load(uint32_t agno, uint32_t root, uint32_t level, uint32_t blocks) {
    if (ags_.count(agno) != 0) return OkStatus();
    if (!geo_->features.reflink) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "filesystem does not have the reflink feature")
          .Ag(agno)
          .Metadata("refcountbt");
    }
    auto verified = LoadRefcountTree(*device_, *geo_, agno);
    if (!verified.ok()) return verified.error();
    if (verified.value().root != root || verified.value().level != level ||
        verified.value().blocks != blocks) {
      return XFS_ERR(XfsError::kCorruptAgHeader,
                     "AGF refcount geometry changed while loading")
          .Ag(agno)
          .Metadata("refcountbt");
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
        return XFS_ERR(XfsError::kCorruptBtree,
                       "refcountbt cycle while loading")
            .Ag(agno)
            .Actual(agbno)
            .Metadata("refcountbt");
      }
      Node node;
      Status s = ReadNode(agno, agbno, &node);
      if (!s.ok()) return s;
      if (node.level != expected) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "refcountbt child level mismatch while loading")
            .Ag(agno)
            .Actual(agbno)
            .Metadata("refcountbt");
      }
      if (node.level > 0) {
        const uint16_t n = NumRecs(node);
        for (uint16_t i = 0; i < n; ++i) {
          stack.push_back(
              {NodePtr(node, i), static_cast<uint16_t>(node.level - 1)});
        }
      }
      ag.nodes.emplace(agbno, std::move(node));
    }
    if (ag.nodes.size() != blocks) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcountbt block count changed while loading")
          .Ag(agno)
          .Expected(blocks)
          .Actual(ag.nodes.size())
          .Metadata("refcountbt");
    }
    ags_.emplace(agno, std::move(ag));
    return OkStatus();
  }

  bool IsLoaded(uint32_t agno) const { return ags_.count(agno) != 0; }

  Result<RefcountRecord> LookupEq(uint32_t agno,
                                  const RefcountKey& key) const {
    const Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissing(agno);
    auto it = std::lower_bound(ag->records.begin(), ag->records.end(), key,
                               [](const RefcountRecord& r,
                                  const RefcountKey& k) {
                                 return r.key.Compare(k) < 0;
                               });
    if (it == ag->records.end() || it->key.Compare(key) != 0) {
      return XFS_ERR(XfsError::kNotFound, "refcount key not found")
          .Ag(agno)
          .Actual(key.startblock)
          .Metadata("refcountbt");
    }
    return *it;
  }

  Result<RefcountRecord> LookupLE(uint32_t agno,
                                  const RefcountKey& key) const {
    const Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissing(agno);
    auto it = std::upper_bound(ag->records.begin(), ag->records.end(), key,
                               [](const RefcountKey& k,
                                  const RefcountRecord& r) {
                                 return k.Compare(r.key) < 0;
                               });
    if (it == ag->records.begin()) {
      return XFS_ERR(XfsError::kNotFound,
                     "no refcount key at or below the target")
          .Ag(agno)
          .Actual(key.startblock)
          .Metadata("refcountbt");
    }
    return *(it - 1);
  }

  Result<RefcountRecord> LookupGE(uint32_t agno,
                                  const RefcountKey& key) const {
    const Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissing(agno);
    auto it = std::lower_bound(ag->records.begin(), ag->records.end(), key,
                               [](const RefcountRecord& r,
                                  const RefcountKey& k) {
                                 return r.key.Compare(k) < 0;
                               });
    if (it == ag->records.end()) {
      return XFS_ERR(XfsError::kNotFound,
                     "no refcount key at or above the target")
          .Ag(agno)
          .Actual(key.startblock)
          .Metadata("refcountbt");
    }
    return *it;
  }

  bool OverlapsShared(uint32_t agno, uint32_t agbno, uint32_t len) const {
    const Ag* ag = FindAg(agno);
    if (ag == nullptr || len == 0) return false;
    for (const RefcountRecord& rec : ag->records) {
      if (rec.cow()) break;
      if (rec.key.startblock < static_cast<uint64_t>(agbno) + len &&
          agbno < rec.EndBlock()) {
        return true;
      }
    }
    return false;
  }

  bool FindShared(uint32_t agno, uint32_t agbno, uint32_t len,
                  RefcountRecord* out) const {
    const Ag* ag = FindAg(agno);
    if (ag == nullptr || len == 0) return false;
    for (const RefcountRecord& rec : ag->records) {
      if (rec.cow()) break;
      if (rec.key.startblock >= static_cast<uint64_t>(agbno) + len) break;
      if (rec.EndBlock() <= agbno) continue;
      const uint32_t start = std::max(rec.key.startblock, agbno);
      const uint64_t end = std::min<uint64_t>(
          rec.EndBlock(), static_cast<uint64_t>(agbno) + len);
      if (end <= start) continue;
      *out = rec;
      out->key.startblock = start;
      out->blockcount = static_cast<uint32_t>(end - start);
      return true;
    }
    return false;
  }

  bool FindCow(uint32_t agno, uint32_t agbno, uint32_t len,
               RefcountRecord* out) const {
    const Ag* ag = FindAg(agno);
    if (ag == nullptr || len == 0) return false;
    for (const RefcountRecord& rec : ag->records) {
      if (!rec.cow()) continue;
      if (rec.key.startblock >= static_cast<uint64_t>(agbno) + len) break;
      if (rec.EndBlock() <= agbno) continue;
      const uint32_t start = std::max(rec.key.startblock, agbno);
      const uint64_t end = std::min<uint64_t>(
          rec.EndBlock(), static_cast<uint64_t>(agbno) + len);
      if (end <= start) continue;
      *out = rec;
      out->key.startblock = start;
      out->blockcount = static_cast<uint32_t>(end - start);
      return true;
    }
    return false;
  }

  Status InsertExact(uint32_t agno, const RefcountRecord& rec) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    Status s = RangeCheck(rec, agno);
    if (!s.ok()) return s;
    const size_t pos = LowerBound(*ag, rec.key);
    if (pos < ag->records.size() &&
        ag->records[pos].key.Compare(rec.key) == 0) {
      return XFS_ERR(XfsError::kCorruptBtree, "duplicate refcount key")
          .Ag(agno)
          .Actual(rec.key.startblock)
          .Metadata("refcountbt");
    }
    for (const RefcountRecord& other : ag->records) {
      if (other.key.startblock < rec.EndBlock() &&
          rec.key.startblock < other.EndBlock()) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "refcount records overlap physically")
            .Ag(agno)
            .Actual(rec.key.startblock)
            .Metadata("refcountbt");
      }
    }
    s = TreeInsert(*ag, rec);
    if (!s.ok()) return s;
    ag->records.insert(ag->records.begin() + static_cast<ptrdiff_t>(pos), rec);
    ++stats_->inserted;
    return OkStatus();
  }

  Status DeleteExact(uint32_t agno, const RefcountKey& key) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    auto it = std::lower_bound(ag->records.begin(), ag->records.end(), key,
                               [](const RefcountRecord& r,
                                  const RefcountKey& k) {
                                 return r.key.Compare(k) < 0;
                               });
    if (it == ag->records.end() || it->key.Compare(key) != 0) {
      return XFS_ERR(XfsError::kNotFound,
                     "refcount key not found for delete")
          .Ag(agno)
          .Actual(key.startblock)
          .Metadata("refcountbt");
    }
    Status s = TreeDelete(*ag, key);
    if (!s.ok()) return s;
    ag->records.erase(it);
    ++stats_->deleted;
    return OkStatus();
  }

  Status UpdateExact(uint32_t agno, const RefcountRecord& old_rec,
                     const RefcountRecord& new_rec) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    Status s = RangeCheck(new_rec, agno);
    if (!s.ok()) return s;
    if (old_rec.key.Compare(new_rec.key) != 0) {
      s = DeleteExact(agno, old_rec.key);
      if (!s.ok()) return s;
      return InsertExact(agno, new_rec);
    }
    for (const RefcountRecord& other : ag->records) {
      if (other.key.Compare(old_rec.key) == 0) continue;
      if (other.key.startblock < new_rec.EndBlock() &&
          new_rec.key.startblock < other.EndBlock()) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "refcount update overlaps another record")
            .Ag(agno)
            .Actual(new_rec.key.startblock)
            .Metadata("refcountbt");
      }
    }
    s = TreeReplace(*ag, old_rec.key, new_rec);
    if (!s.ok()) return s;
    auto it = std::lower_bound(ag->records.begin(), ag->records.end(),
                               old_rec.key,
                               [](const RefcountRecord& r,
                                  const RefcountKey& k) {
                                 return r.key.Compare(k) < 0;
                               });
    if (it == ag->records.end() || it->key.Compare(old_rec.key) != 0) {
      return XFS_ERR(XfsError::kInternal,
                     "refcount record list diverged from the tree")
          .Ag(agno)
          .Metadata("refcountbt");
    }
    *it = new_rec;
    return OkStatus();
  }

  // Normalize a proposed logical record list: merge adjacent records in the
  // same domain with the same refcount (xfs_refcount_merge_extents).
  static std::vector<RefcountRecord> Normalize(
      const std::vector<RefcountRecord>& input) {
    std::vector<RefcountRecord> sorted = input;
    std::sort(sorted.begin(), sorted.end(),
              [](const RefcountRecord& a, const RefcountRecord& b) {
                return a.key.Compare(b.key) < 0;
              });
    std::vector<RefcountRecord> out;
    for (const RefcountRecord& rec : sorted) {
      if (!out.empty() && Mergeable(out.back(), rec)) {
        out.back().blockcount += rec.blockcount;
        continue;
      }
      out.push_back(rec);
    }
    return out;
  }

  Status ApplyDiff(uint32_t agno, std::vector<RefcountRecord> next) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    next = Normalize(next);
    const std::vector<RefcountRecord> old = ag->records;

    // Deletes first: an old record may overlap a new split fragment.
    for (const RefcountRecord& rec : old) {
      bool present = false;
      for (const RefcountRecord& n : next) {
        if (n.key.Compare(rec.key) == 0) {
          present = true;
          break;
        }
      }
      if (!present) {
        Status s = DeleteExact(agno, rec.key);
        if (!s.ok()) return s;
      }
    }
    // In-place updates for retained keys whose payload changed.
    for (const RefcountRecord& rec : old) {
      for (const RefcountRecord& n : next) {
        if (n.key.Compare(rec.key) != 0) continue;
        if (!SameRecord(rec, n)) {
          Status s = UpdateExact(agno, rec, n);
          if (!s.ok()) return s;
        }
        break;
      }
    }
    // Then inserts.
    for (const RefcountRecord& n : next) {
      bool present = false;
      for (const RefcountRecord& rec : old) {
        if (n.key.Compare(rec.key) == 0) {
          present = true;
          break;
        }
      }
      if (!present) {
        Status s = InsertExact(agno, n);
        if (!s.ok()) return s;
      }
    }
    return OkStatus();
  }

  Status AdjustShared(uint32_t agno, uint32_t agbno, uint32_t len,
                      bool increase) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    const uint64_t agblocks = geo_->AgBlocksFor(agno);
    if (len == 0) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "zero-length refcount adjustment")
          .Ag(agno)
          .Metadata("refcountbt");
    }
    if (static_cast<uint64_t>(agbno) + len > agblocks) {
      return XFS_ERR(XfsError::kOutOfRange,
                     "refcount adjustment leaves its AG")
          .Ag(agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }

    const uint64_t start = agbno;
    const uint64_t end = start + len;
    for (const RefcountRecord& rec : ag->records) {
      if (!rec.cow()) continue;
      if (rec.key.startblock < end && start < rec.EndBlock()) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "shared adjustment overlaps CoW staging")
            .Ag(agno)
            .Actual(agbno)
            .Metadata("refcountbt");
      }
    }
    std::vector<RefcountRecord> work;
    for (const RefcountRecord& rec : ag->records) {
      if (rec.cow()) {
        // CoW staging ranges are a separate domain; shared adjustments never
        // touch them.  Carry them through unchanged.
        work.push_back(rec);
        continue;
      }
      if (rec.EndBlock() <= start || rec.key.startblock >= end) {
        work.push_back(rec);
        continue;
      }
      // Split at the adjustment boundaries.
      if (rec.key.startblock < start) {
        RefcountRecord left = rec;
        left.blockcount = static_cast<uint32_t>(start - rec.key.startblock);
        work.push_back(left);
      }
      const uint64_t seg_start = std::max<uint64_t>(rec.key.startblock, start);
      const uint64_t seg_end = std::min<uint64_t>(rec.EndBlock(), end);
      RefcountRecord middle = rec;
      middle.key.startblock = static_cast<uint32_t>(seg_start);
      middle.blockcount = static_cast<uint32_t>(seg_end - seg_start);
      work.push_back(middle);
      if (rec.EndBlock() > end) {
        RefcountRecord right = rec;
        right.key.startblock = static_cast<uint32_t>(end);
        right.blockcount =
            static_cast<uint32_t>(rec.EndBlock() - end);
        work.push_back(right);
      }
    }

    // Process the range, synthesizing implied refcount-1 segments in holes.
    std::vector<RefcountRecord> result;
    std::vector<RefcountRecord> freed;
    uint64_t cursor = start;
    for (const RefcountRecord& rec : work) {
      if (rec.key.startblock >= end) {
        // remaining outside range; handled after the loop
      } else if (rec.key.startblock < start) {
        result.push_back(rec);
        continue;
      } else if (rec.key.startblock > cursor && cursor < end) {
        // Hole before this record inside [start,end).
        RefcountRecord hole;
        hole.key.domain = RefcountDomain::kShared;
        hole.key.startblock = static_cast<uint32_t>(cursor);
        hole.blockcount =
            static_cast<uint32_t>(std::min<uint64_t>(rec.key.startblock, end) -
                                  cursor);
        hole.refcount = increase ? 2 : 1;  // 1 + delta
        if (hole.refcount > 1) {
          result.push_back(hole);
        } else {
          freed.push_back(hole);
        }
        cursor = static_cast<uint64_t>(hole.key.startblock) + hole.blockcount;
      }
      if (rec.key.startblock >= end) {
        result.push_back(rec);
        continue;
      }
      if (rec.key.startblock == cursor && cursor < end) {
        RefcountRecord middle = rec;
        uint32_t new_ref = middle.refcount;
        if (increase) {
          if (new_ref != kRefcountMax) ++new_ref;
        } else if (new_ref != kRefcountMax) {
          --new_ref;
        }
        ++stats_->shared_increase;
        if (new_ref > 1) {
          middle.refcount = new_ref;
          result.push_back(middle);
        } else if (new_ref == 0) {
          freed.push_back(middle);
        }
        cursor = middle.EndBlock();
      }
    }
    if (cursor < end) {
      RefcountRecord hole;
      hole.key.domain = RefcountDomain::kShared;
      hole.key.startblock = static_cast<uint32_t>(cursor);
      hole.blockcount = static_cast<uint32_t>(end - cursor);
      hole.refcount = increase ? 2 : 1;
      if (hole.refcount > 1) {
        result.push_back(hole);
      } else {
        freed.push_back(hole);
      }
    }

    Status s = ApplyDiff(agno, result);
    if (!s.ok()) return s;
    for (const RefcountRecord& f : freed) {
      s = hooks_.free_extent(agno, f.key.startblock, f.blockcount);
      if (!s.ok()) return s;
      ++stats_->freed_ranges;
    }
    return OkStatus();
  }

  Status Increase(uint32_t agno, uint32_t agbno, uint32_t len) {
    Status s = AdjustShared(agno, agbno, len, true);
    if (s.ok()) ++stats_->shared_increase;
    return s;
  }

  Status Decrease(uint32_t agno, uint32_t agbno, uint32_t len) {
    Status s = AdjustShared(agno, agbno, len, false);
    if (s.ok()) ++stats_->shared_decrease;
    return s;
  }

  Status CowAlloc(uint32_t agno, uint32_t agbno, uint32_t len) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    Status s = RangeCheckKey(agno, agbno, len);
    if (!s.ok()) return s;
    RefcountRecord rec;
    rec.key.domain = RefcountDomain::kCow;
    rec.key.startblock = agbno;
    rec.blockcount = len;
    rec.refcount = 1;
    for (const RefcountRecord& other : ag->records) {
      if (other.key.startblock < rec.EndBlock() &&
          agbno < other.EndBlock()) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "CoW staging range overlaps an existing refcount")
            .Ag(agno)
            .Actual(agbno)
            .Metadata("refcountbt");
      }
    }
    std::vector<RefcountRecord> next = ag->records;
    next.push_back(rec);
    s = ApplyDiff(agno, std::move(next));
    if (s.ok()) ++stats_->cow_alloc;
    return s;
  }

  Status CowFree(uint32_t agno, uint32_t agbno, uint32_t len) {
    Ag* ag = FindAg(agno);
    if (ag == nullptr) return AgMissingStatus(agno);
    Status s = RangeCheckKey(agno, agbno, len);
    if (!s.ok()) return s;
    const uint64_t start = agbno;
    const uint64_t end = start + len;
    // A CoW free may split the first/last record at the requested boundaries
    // (xfs_refcount_split_extent), but every block inside the range must be
    // covered by staging records.  A gap would mean freeing blocks that were
    // never reserved; refuse that as corruption.
    bool covered = false;
    uint64_t cursor = start;
    std::vector<RefcountRecord> next;
    for (const RefcountRecord& rec : ag->records) {
      if (!rec.cow()) {
        next.push_back(rec);
        continue;
      }
      if (rec.EndBlock() <= start || rec.key.startblock >= end) {
        next.push_back(rec);
        continue;
      }
      const uint64_t seg_start =
          std::max<uint64_t>(rec.key.startblock, start);
      const uint64_t seg_end = std::min<uint64_t>(rec.EndBlock(), end);
      if (!covered) {
        if (seg_start != start) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "CoW staging removal starts in an uncovered gap")
              .Ag(agno)
              .Actual(seg_start)
              .Metadata("refcountbt");
        }
        covered = true;
      } else if (seg_start != cursor) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "CoW staging removal has an uncovered gap")
            .Ag(agno)
            .Actual(static_cast<uint32_t>(cursor))
            .Metadata("refcountbt");
      }
      cursor = seg_end;
      if (rec.key.startblock < start) {
        RefcountRecord left = rec;
        left.blockcount = static_cast<uint32_t>(start - rec.key.startblock);
        next.push_back(left);
      }
      if (rec.EndBlock() > end) {
        RefcountRecord right = rec;
        right.key.startblock = static_cast<uint32_t>(end);
        right.blockcount = static_cast<uint32_t>(rec.EndBlock() - end);
        next.push_back(right);
      }
    }
    if (!covered || cursor != end) {
      return XFS_ERR(XfsError::kNotFound,
                     "CoW staging range is not fully reserved")
          .Ag(agno)
          .Actual(static_cast<uint32_t>(cursor))
          .Metadata("refcountbt");
    }
    s = ApplyDiff(agno, std::move(next));
    if (s.ok()) ++stats_->cow_free;
    return s;
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
  const std::vector<RefcountRecord>& Records(uint32_t agno) const {
    static const std::vector<RefcountRecord> kEmpty;
    const Ag* ag = FindAg(agno);
    return ag == nullptr ? kEmpty : ag->records;
  }

  uint32_t ReserveBlocks(uint32_t agno) const {
    const Ag* ag = FindAg(agno);
    const uint32_t height = ag == nullptr ? 1 : ag->level;
    const uint32_t maxlevels = bt::kMaxTraversalLevels;
    const uint32_t usable = std::min<uint32_t>(height + 2, maxlevels);
    return std::max<uint32_t>(2 * usable, 4);
  }

  Status Commit() {
    for (auto& entry : ags_) {
      Ag& ag = entry.second;
      Status verified = Verify(ag.agno);
      if (!verified.ok()) return verified;
      for (auto& node_entry : ag.nodes) {
        Node& node = node_entry.second;
        if (!node.dirty) continue;
        Status s = hooks_.publish_block(ag.agno, node_entry.first,
                                        node.data.data(), node.data.size());
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
      return XFS_ERR(XfsError::kCorruptBtree, "refcount root is null")
          .Ag(agno)
          .Metadata("refcountbt");
    }
    std::set<uint32_t> visited;
    std::map<uint16_t, uint32_t> last_at_level;
    std::vector<RefcountRecord> observed;
    uint32_t low = 0;
    Status s = VerifyVisit(*ag, ag->root,
                           static_cast<uint16_t>(ag->level - 1), true,
                           &visited, &last_at_level, &observed, &low);
    if (!s.ok()) return s;
    if (visited.size() != ag->blocks) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcount block count disagrees with the tree")
          .Ag(agno)
          .Expected(ag->blocks)
          .Actual(visited.size())
          .Metadata("refcountbt");
    }
    if (observed.size() != ag->records.size()) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcount record list disagrees with the tree")
          .Ag(agno)
          .Expected(ag->records.size())
          .Actual(observed.size())
          .Metadata("refcountbt");
    }
    for (size_t i = 0; i < observed.size(); ++i) {
      if (!SameRecord(observed[i], ag->records[i])) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "refcount record list disagrees with the tree")
            .Ag(agno)
            .Actual(i)
            .Metadata("refcountbt");
      }
    }
    for (const auto& entry : last_at_level) {
      const Node& node = ag->nodes.at(entry.second);
      if (RightSib(node) != kNullAgBlock) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "rightmost refcount block has a right sibling")
            .Ag(agno)
            .Actual(entry.second)
            .Metadata("refcountbt");
      }
    }
    return OkStatus();
  }

 private:
  static Result<RefcountRecord> AgMissing(uint32_t agno) {
    return XFS_ERR(XfsError::kInternal, "refcount AG is not loaded")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  static Status AgMissingStatus(uint32_t agno) {
    return XFS_ERR(XfsError::kInternal, "refcount AG is not loaded")
        .Ag(agno)
        .Metadata("refcountbt");
  }

  Ag* FindAg(uint32_t agno) {
    auto it = ags_.find(agno);
    return it == ags_.end() ? nullptr : &it->second;
  }
  const Ag* FindAg(uint32_t agno) const {
    auto it = ags_.find(agno);
    return it == ags_.end() ? nullptr : &it->second;
  }

  Status RangeCheckKey(uint32_t agno, uint32_t agbno, uint32_t len) const {
    if (len == 0) {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "zero-length refcount range")
          .Ag(agno)
          .Metadata("refcountbt");
    }
    if (static_cast<uint64_t>(agbno) + len > geo_->AgBlocksFor(agno)) {
      return XFS_ERR(XfsError::kOutOfRange, "refcount range leaves its AG")
          .Ag(agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    return OkStatus();
  }

  Status RangeCheck(const RefcountRecord& rec, uint32_t agno) const {
    Status s = RangeCheckKey(agno, rec.key.startblock, rec.blockcount);
    if (!s.ok()) return s;
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

  uint32_t max_leaf() const {
    return static_cast<uint32_t>((geo_->block_size - kHeader) / kRecSize);
  }
  uint32_t max_node() const {
    return static_cast<uint32_t>((geo_->block_size - kHeader) /
                                 (kKeySize + kPtrSize));
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

  RefcountRecord LeafRec(const Node& node, size_t i) const {
    return ParseRecord(node.data.data() + kHeader + i * kRecSize);
  }
  void SetLeafRec(Node& node, size_t i, const RefcountRecord& r) {
    StoreRecord(node.data.data() + kHeader + i * kRecSize, r);
    node.dirty = true;
  }
  uint32_t NodeLow(const Node& node, size_t i) const {
    return GetBE32(node.data.data(), kHeader + i * kKeySize);
  }
  uint32_t NodePtr(const Node& node, size_t i) const {
    const size_t base =
        kHeader + static_cast<size_t>(max_node()) * kKeySize;
    return GetBE32(node.data.data(), base + i * kPtrSize);
  }
  void SetNodeEntry(Node& node, size_t i, uint32_t low, uint32_t ptr) {
    PutBE32(node.data.data(), kHeader + i * kKeySize, low);
    const size_t base =
        kHeader + static_cast<size_t>(max_node()) * kKeySize;
    PutBE32(node.data.data(), base + i * kPtrSize, ptr);
    node.dirty = true;
  }

  Status ReadNode(uint32_t agno, uint32_t agbno, Node* out) const {
    const uint64_t agblocks = geo_->AgBlocksFor(agno);
    if (agbno == kNullAgBlock || agbno >= agblocks) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcount block out of range")
          .Ag(agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    Node node;
    node.data.assign(geo_->block_size, 0);
    const uint64_t offset = geo_->FsbToByte(geo_->AgFsb(agno, agbno));
    if (!device_->Read(offset, node.data.data(), node.data.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to read refcount block")
          .Ag(agno)
          .Actual(agbno)
          .Offset(offset)
          .Metadata("refcountbt");
    }
    if (GetBE32(node.data.data(), bt::kMagic) != ondisk::kRefcountCrcMagic) {
      return XFS_ERR(XfsError::kBadMagic, "refcount block magic mismatch")
          .Ag(agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    node.level = GetBE16(node.data.data(), bt::kLevel);
    *out = std::move(node);
    return OkStatus();
  }

  Status InitNode(Ag& ag, uint32_t agbno, uint16_t level, uint16_t numrecs) {
    auto it = ag.nodes.find(agbno);
    if (it == ag.nodes.end()) {
      return XFS_ERR(XfsError::kInternal,
                     "initializing an unloaded refcount block")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    Node& node = it->second;
    node.level = level;
    node.data.assign(geo_->block_size, 0);
    PutBE32(node.data.data(), bt::kMagic, ondisk::kRefcountCrcMagic);
    PutBE16(node.data.data(), bt::kLevel, level);
    PutBE16(node.data.data(), bt::kNumrecs, numrecs);
    PutBE32(node.data.data(), bt::kSLeftsib, kNullAgBlock);
    PutBE32(node.data.data(), bt::kSRightSib, kNullAgBlock);
    PutBE64(node.data.data(), bt::kSBlkno,
            geo_->FsbToByte(geo_->AgFsb(ag.agno, agbno)) / 512);
    PutBE64(node.data.data(), bt::kSLsn, 0);
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
                     "refcount block allocation returned an invalid block")
          .Ag(ag.agno)
          .Actual(bno)
          .Metadata("refcountbt");
    }
    if (ag.nodes.count(bno) != 0) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcount block allocation returned a live tree block")
          .Ag(ag.agno)
          .Actual(bno)
          .Metadata("refcountbt");
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
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcount block count underflow")
          .Ag(ag.agno)
          .Metadata("refcountbt");
    }
    --ag.blocks;
    return OkStatus();
  }

  size_t LowerBound(const Ag& ag, const RefcountKey& key) const {
    return static_cast<size_t>(
        std::lower_bound(ag.records.begin(), ag.records.end(), key,
                         [](const RefcountRecord& r, const RefcountKey& k) {
                           return r.key.Compare(k) < 0;
                         }) -
        ag.records.begin());
  }

  uint32_t ComputeLow(const Ag& ag, uint32_t agbno) const {
    const Node& node = NodeAt(ag, agbno);
    if (node.level == 0) {
      if (NumRecs(node) == 0) return 0;
      return LeafRec(node, 0).key.Encoded();
    }
    return NodeLow(node, 0);
  }

  void RefreshEntry(Ag& ag, uint32_t parent, size_t index) {
    Node& p = NodeAt(ag, parent);
    const uint32_t child = NodePtr(p, index);
    SetNodeEntry(p, index, ComputeLow(ag, child), child);
  }

  void RefreshAncestors(Ag& ag, const std::vector<Frame>& path) {
    for (size_t i = path.size(); i > 0; --i) {
      const Frame& f = path[i - 1];
      RefreshEntry(ag, f.parent, f.index);
    }
  }

  Status Descend(Ag& ag, const RefcountKey& key, std::vector<Frame>* path,
                 uint32_t* leaf) {
    uint32_t cur = ag.root;
    if (ag.nodes.count(cur) == 0) {
      return XFS_ERR(XfsError::kInternal, "refcount root block missing")
          .Ag(ag.agno)
          .Actual(cur)
          .Metadata("refcountbt");
    }
    while (NodeAt(ag, cur).level > 0) {
      Node& node = NodeAt(ag, cur);
      const uint16_t n = NumRecs(node);
      if (n == 0) {
        return XFS_ERR(XfsError::kCorruptBtree, "empty refcount node")
            .Ag(ag.agno)
            .Actual(cur)
            .Metadata("refcountbt");
      }
      const uint32_t encoded = key.Encoded();
      size_t chosen = 0;
      for (size_t i = 0; i < n; ++i) {
        if (NodeLow(node, i) <= encoded) {
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
                       "refcount child block missing")
            .Ag(ag.agno)
            .Actual(cur)
            .Metadata("refcountbt");
      }
    }
    *leaf = cur;
    return OkStatus();
  }

  Status TreeInsert(Ag& ag, const RefcountRecord& rec) {
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
                       "refcount key already present in the leaf")
            .Ag(ag.agno)
            .Actual(rec.key.startblock)
            .Metadata("refcountbt");
      }
      for (size_t i = n; i > pos; --i) {
        SetLeafRec(node, i, LeafRec(node, i - 1));
      }
      SetLeafRec(node, pos, rec);
      SetNumRecs(node, static_cast<uint16_t>(n + 1));
      RefreshAncestors(ag, path);
      return OkStatus();
    }

    std::vector<RefcountRecord> entries;
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
    s = InitNode(ag, right, 0,
                 static_cast<uint16_t>(entries.size() - left_count));
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

    const uint32_t left_low = ComputeLow(ag, leaf);
    const uint32_t right_low = ComputeLow(ag, right);
    return InsertChild(ag, path, leaf, left_low, right, right_low);
  }

  Status InsertChild(Ag& ag, std::vector<Frame>& path, uint32_t left,
                     uint32_t left_low, uint32_t right, uint32_t right_low) {
    if (path.empty()) {
      uint32_t new_root = kNullAgBlock;
      Status s = AllocTreeBlock(ag, &new_root);
      if (!s.ok()) return s;
      s = InitNode(ag, new_root, static_cast<uint16_t>(ag.level), 2);
      if (!s.ok()) return s;
      Node& root = NodeAt(ag, new_root);
      SetNodeEntry(root, 0, left_low, left);
      SetNodeEntry(root, 1, right_low, right);
      ag.root = new_root;
      ++ag.level;
      ++stats_->root_grow;
      return OkStatus();
    }

    Frame frame = path.back();
    path.pop_back();
    Node& parent = NodeAt(ag, frame.parent);
    if (frame.index >= NumRecs(parent)) {
      return XFS_ERR(XfsError::kInternal, "refcount parent index is stale")
          .Ag(ag.agno)
          .Actual(frame.parent)
          .Metadata("refcountbt");
    }
    SetNodeEntry(parent, frame.index, left_low, left);
    const uint16_t pn = NumRecs(parent);
    if (pn < max_node()) {
      const size_t insert_at = frame.index + 1;
      for (size_t i = pn; i > insert_at; --i) {
        SetNodeEntry(parent, i, NodeLow(parent, i - 1),
                     NodePtr(parent, i - 1));
      }
      SetNodeEntry(parent, insert_at, right_low, right);
      SetNumRecs(parent, static_cast<uint16_t>(pn + 1));
      RefreshAncestors(ag, path);
      return OkStatus();
    }

    std::vector<std::pair<uint32_t, uint32_t>> entries;
    entries.reserve(static_cast<size_t>(pn) + 1);
    const size_t insert_at = frame.index + 1;
    for (size_t i = 0; i < insert_at; ++i) {
      entries.push_back({NodeLow(parent, i), NodePtr(parent, i)});
    }
    entries.push_back({right_low, right});
    for (size_t i = insert_at; i < pn; ++i) {
      entries.push_back({NodeLow(parent, i), NodePtr(parent, i)});
    }
    const size_t left_count = entries.size() / 2;
    uint32_t right_node = kNullAgBlock;
    Status s = AllocTreeBlock(ag, &right_node);
    if (!s.ok()) return s;
    s = InitNode(ag, right_node, parent.level,
                 static_cast<uint16_t>(entries.size() - left_count));
    if (!s.ok()) return s;
    {
      Node& lb = NodeAt(ag, frame.parent);
      Node& rb = NodeAt(ag, right_node);
      SetNumRecs(lb, static_cast<uint16_t>(left_count));
      for (size_t i = 0; i < left_count; ++i) {
        SetNodeEntry(lb, i, entries[i].first, entries[i].second);
      }
      for (size_t i = left_count; i < entries.size(); ++i) {
        SetNodeEntry(rb, i - left_count, entries[i].first, entries[i].second);
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

    const uint32_t nl = ComputeLow(ag, frame.parent);
    const uint32_t rl = ComputeLow(ag, right_node);
    return InsertChild(ag, path, frame.parent, nl, right_node, rl);
  }

  Status TreeReplace(Ag& ag, const RefcountKey& key,
                     const RefcountRecord& rec) {
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
                   "refcount key not found for in-place update")
        .Ag(ag.agno)
        .Actual(key.startblock)
        .Metadata("refcountbt");
  }

  Status TreeDelete(Ag& ag, const RefcountKey& key) {
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
      return XFS_ERR(XfsError::kNotFound,
                     "refcount key not found in leaf")
          .Ag(ag.agno)
          .Actual(key.startblock)
          .Metadata("refcountbt");
    }
    for (size_t i = pos; i + 1 < n; ++i) {
      SetLeafRec(node, i, LeafRec(node, i + 1));
    }
    SetNumRecs(node, static_cast<uint16_t>(n - 1));

    if (leaf == ag.root) return OkStatus();  // empty root leaf is legal
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
        return XFS_ERR(XfsError::kInternal, "refcount parent index is stale")
            .Ag(ag.agno)
            .Actual(frame.parent)
            .Metadata("refcountbt");
      }

      // A root with a single child collapses to that child, even if the child
      // is now underfull; a root may legally hold any nonzero record count.
      if (path.size() == 1 && pn == 1) {
        if (cur_level == 0 && NumRecs(cur_node) == 0) {
          // Deleting the last key from a two-level tree leaves the child
          // empty.  The empty leaf becomes the new root.
        } else if (cur_level > 0 && NumRecs(cur_node) == 0) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "refcount internal child lost all records")
              .Ag(ag.agno)
              .Actual(frame.child)
              .Metadata("refcountbt");
        }
        const uint32_t old_root = ag.root;
        ag.root = frame.child;
        --ag.level;
        SetLeftSib(NodeAt(ag, frame.child), kNullAgBlock);
        SetRightSib(NodeAt(ag, frame.child), kNullAgBlock);
        Status s = FreeTreeBlock(ag, old_root);
        if (!s.ok()) return s;
        ++stats_->root_collapse;
        if (ag.level > 1 && NumRecs(NodeAt(ag, ag.root)) == 1) {
          return CollapseRoot(ag);
        }
        return OkStatus();
      }

      const uint32_t left =
          frame.index > 0 ? NodePtr(parent, frame.index - 1) : kNullAgBlock;
      const uint32_t right = frame.index + 1 < pn
                                 ? NodePtr(parent, frame.index + 1)
                                 : kNullAgBlock;

      if (left != kNullAgBlock &&
          NumRecs(NodeAt(ag, left)) > minr) {
        BorrowLast(ag, left, frame.child);
        if (cur_level == 0) {
          ++stats_->borrow_leaf;
        } else {
          ++stats_->borrow_node;
        }
        if (frame.index > 0) RefreshEntry(ag, frame.parent, frame.index - 1);
        RefreshEntry(ag, frame.parent, frame.index);
        path.pop_back();
        RefreshAncestors(ag, path);
        break;
      }
      if (right != kNullAgBlock &&
          NumRecs(NodeAt(ag, right)) > minr) {
        BorrowFirst(ag, right, frame.child);
        if (cur_level == 0) {
          ++stats_->borrow_leaf;
        } else {
          ++stats_->borrow_node;
        }
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
                         "refcount sibling merge would exceed a block")
              .Ag(ag.agno)
              .Actual(frame.child)
              .Metadata("refcountbt");
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
        RefreshEntry(ag, frame.parent, frame.index - 1);
        path.pop_back();
        if (path.empty()) {
          if (NumRecs(parent) == 1 && ag.level > 1) {
            return CollapseRoot(ag);
          }
          return OkStatus();
        }
        if (NumRecs(parent) == 0) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "refcount node lost all its children")
              .Ag(ag.agno)
              .Actual(frame.parent)
              .Metadata("refcountbt");
        }
        continue;
      }

      if (right != kNullAgBlock) {
        Node& cb = NodeAt(ag, frame.child);
        Node& rb = NodeAt(ag, right);
        if (static_cast<uint32_t>(NumRecs(cb)) + NumRecs(rb) > maxr) {
          return XFS_ERR(XfsError::kUnsupportedFeature,
                         "refcount sibling merge would exceed a block")
              .Ag(ag.agno)
              .Actual(frame.child)
              .Metadata("refcountbt");
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
        RefreshEntry(ag, frame.parent, frame.index);
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
                     "refcount block underflow has no sibling to rebalance")
          .Ag(ag.agno)
          .Actual(frame.child)
          .Metadata("refcountbt");
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
        SetNodeEntry(dst, dn + i, NodeLow(src, i), NodePtr(src, i));
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
        SetNodeEntry(dst, i, NodeLow(dst, i - 1), NodePtr(dst, i - 1));
      }
      SetNodeEntry(dst, 0, NodeLow(src, sn - 1), NodePtr(src, sn - 1));
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
      SetNodeEntry(dst, dn, NodeLow(src, 0), NodePtr(src, 0));
      for (uint16_t i = 0; i + 1 < sn; ++i) {
        SetNodeEntry(src, i, NodeLow(src, i + 1), NodePtr(src, i + 1));
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
                   NodePtr(parent, i + 1));
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
                     "refcount root child is missing during collapse")
          .Ag(ag.agno)
          .Actual(child)
          .Metadata("refcountbt");
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
                     std::vector<RefcountRecord>* observed,
                     uint32_t* low_out) const {
    if (agbno == kNullAgBlock || agbno >= geo_->AgBlocksFor(ag.agno)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcount child pointer out of range")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    if (!visited->insert(agbno).second) {
      return XFS_ERR(XfsError::kCorruptBtree, "refcount tree cycle")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    auto it = ag.nodes.find(agbno);
    if (it == ag.nodes.end()) {
      return XFS_ERR(XfsError::kCorruptBtree, "refcount block missing")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    const Node& node = it->second;
    if (node.level != expected_level) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcount block level mismatch")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    const uint16_t n = NumRecs(node);
    const uint32_t maxr = node.level == 0 ? max_leaf() : max_node();
    if (n > maxr) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcount block record count out of range")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    if (n == 0 && !(root && node.level == 0)) {
      return XFS_ERR(XfsError::kCorruptBtree, "refcount block is empty")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    if (!root && n < maxr / 2) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "non-root refcount block is below half full")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    const uint32_t leftsib = LeftSib(node);
    auto last = last_at_level->find(node.level);
    if (last == last_at_level->end()) {
      if (leftsib != kNullAgBlock) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "leftmost refcount block has a left sibling")
            .Ag(ag.agno)
            .Actual(agbno)
            .Metadata("refcountbt");
      }
    } else if (last->second != leftsib ||
               RightSib(ag.nodes.at(last->second)) != agbno) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "refcount sibling link mismatch")
          .Ag(ag.agno)
          .Actual(agbno)
          .Metadata("refcountbt");
    }
    (*last_at_level)[node.level] = agbno;

    if (node.level == 0) {
      *low_out = n == 0 ? 0 : LeafRec(node, 0).key.Encoded();
      for (uint16_t i = 0; i < n; ++i) {
        const RefcountRecord rec = LeafRec(node, i);
        Status s = RangeCheck(rec, ag.agno);
        if (!s.ok()) return s;
        if (!observed->empty() &&
            observed->back().key.Compare(rec.key) >= 0) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "refcount records are not strictly ordered")
              .Ag(ag.agno)
              .Actual(rec.key.startblock)
              .Metadata("refcountbt");
        }
        for (const RefcountRecord& other : *observed) {
          if (other.key.startblock < rec.EndBlock() &&
              rec.key.startblock < other.EndBlock()) {
            return XFS_ERR(XfsError::kCorruptBtree,
                           "refcount records overlap physically")
                .Ag(ag.agno)
                .Actual(rec.key.startblock)
                .Metadata("refcountbt");
          }
        }
        observed->push_back(rec);
      }
      return OkStatus();
    }

    bool first = true;
    uint32_t previous_low = 0;
    for (uint16_t i = 0; i < n; ++i) {
      const uint32_t low = NodeLow(node, i);
      if (!first && previous_low >= low) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "refcount node keys are out of order")
            .Ag(ag.agno)
            .Actual(agbno)
            .Metadata("refcountbt");
      }
      previous_low = low;
      const uint32_t child = NodePtr(node, i);
      uint32_t child_low = 0;
      Status s = VerifyVisit(ag, child, static_cast<uint16_t>(node.level - 1),
                             false, visited, last_at_level, observed,
                             &child_low);
      if (!s.ok()) return s;
      if (child_low != low) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "refcount parent key does not equal its child")
            .Ag(ag.agno)
            .Actual(agbno)
            .Metadata("refcountbt");
      }
      if (first) {
        *low_out = low;
        first = false;
      }
    }
    return OkStatus();
  }

  BlockDevice* device_;
  const XfsGeometry* geo_;
  Hooks hooks_;
  RefcountEditStats* stats_;
  std::map<uint32_t, Ag> ags_;
};

// ---------------------------------------------------------------------------
// Public facade
// ---------------------------------------------------------------------------

RefcountEditor::RefcountEditor(BlockDevice* device, const XfsGeometry* geo,
                               Hooks hooks)
    : impl_(std::make_unique<Impl>(device, geo, std::move(hooks), &stats_)) {}

RefcountEditor::~RefcountEditor() = default;

Status RefcountEditor::Load(uint32_t agno, uint32_t root, uint32_t level,
                            uint32_t blocks) {
  return impl_->Load(agno, root, level, blocks);
}
bool RefcountEditor::IsLoaded(uint32_t agno) const {
  return impl_->IsLoaded(agno);
}
Result<RefcountRecord> RefcountEditor::LookupEq(
    uint32_t agno, const RefcountKey& key) const {
  return impl_->LookupEq(agno, key);
}
Result<RefcountRecord> RefcountEditor::LookupLE(
    uint32_t agno, const RefcountKey& key) const {
  return impl_->LookupLE(agno, key);
}
Result<RefcountRecord> RefcountEditor::LookupGE(
    uint32_t agno, const RefcountKey& key) const {
  return impl_->LookupGE(agno, key);
}
bool RefcountEditor::OverlapsShared(uint32_t agno, uint32_t agbno,
                                    uint32_t len) const {
  return impl_->OverlapsShared(agno, agbno, len);
}
bool RefcountEditor::FindShared(uint32_t agno, uint32_t agbno, uint32_t len,
                                RefcountRecord* out) const {
  return impl_->FindShared(agno, agbno, len, out);
}
bool RefcountEditor::FindCow(uint32_t agno, uint32_t agbno, uint32_t len,
                             RefcountRecord* out) const {
  return impl_->FindCow(agno, agbno, len, out);
}
Status RefcountEditor::InsertExact(uint32_t agno, const RefcountRecord& rec) {
  return impl_->InsertExact(agno, rec);
}
Status RefcountEditor::DeleteExact(uint32_t agno, const RefcountKey& key) {
  return impl_->DeleteExact(agno, key);
}
Status RefcountEditor::UpdateExact(uint32_t agno,
                                   const RefcountRecord& old_rec,
                                   const RefcountRecord& new_rec) {
  return impl_->UpdateExact(agno, old_rec, new_rec);
}
Status RefcountEditor::Increase(uint32_t agno, uint32_t agbno, uint32_t len) {
  return impl_->Increase(agno, agbno, len);
}
Status RefcountEditor::Decrease(uint32_t agno, uint32_t agbno, uint32_t len) {
  return impl_->Decrease(agno, agbno, len);
}
Status RefcountEditor::CowAlloc(uint32_t agno, uint32_t agbno, uint32_t len) {
  return impl_->CowAlloc(agno, agbno, len);
}
Status RefcountEditor::CowFree(uint32_t agno, uint32_t agbno, uint32_t len) {
  return impl_->CowFree(agno, agbno, len);
}
uint32_t RefcountEditor::Root(uint32_t agno) const {
  return impl_->Root(agno);
}
uint32_t RefcountEditor::Level(uint32_t agno) const {
  return impl_->Level(agno);
}
uint32_t RefcountEditor::Blocks(uint32_t agno) const {
  return impl_->Blocks(agno);
}
const std::vector<RefcountRecord>& RefcountEditor::Records(
    uint32_t agno) const {
  return impl_->Records(agno);
}
uint32_t RefcountEditor::ReserveBlocks(uint32_t agno) const {
  return impl_->ReserveBlocks(agno);
}
Status RefcountEditor::Commit() {
  Status status = impl_->Commit();
  if (status.ok() && (stats_.split_leaf || stats_.split_node ||
                      stats_.merge_leaf || stats_.merge_node ||
                      stats_.borrow_leaf || stats_.borrow_node ||
                      stats_.root_grow || stats_.root_collapse)) {
    XFS_INFO("refcount edits: split_leaf="
             << stats_.split_leaf << " split_node=" << stats_.split_node
             << " merge_leaf=" << stats_.merge_leaf
             << " merge_node=" << stats_.merge_node
             << " borrow_leaf=" << stats_.borrow_leaf
             << " borrow_node=" << stats_.borrow_node
             << " root_grow=" << stats_.root_grow
             << " root_collapse=" << stats_.root_collapse);
  }
  return status;
}
Status RefcountEditor::Verify(uint32_t agno) const {
  return impl_->Verify(agno);
}

}  // namespace xfs
