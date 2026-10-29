// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Owner: AGF counters, AGFL ring and free-record list.
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "xfs/ag.h"
#include "xfs/btree.h"
#include "xfs/endian.h"
#include "write/alloc/alloc_internal.h"

namespace xfs {
namespace alloc_detail {

uint32_t AgState::Contribution() const {
  uint32_t btreeblks = static_cast<uint32_t>(bno->block_count() - 1) +
                       static_cast<uint32_t>(cnt->block_count() - 1);
  if (owner->rmap_enabled()) {
    const uint32_t blocks = owner->RmapBlocks(agno);
    if (blocks > 0) btreeblks += blocks - 1;
  }
  return freeblks + flcount + btreeblks;
}

Status AgState::Load(Allocator* allocator, uint32_t agno_in) {
  owner = allocator;
  agno = agno_in;
  const XfsGeometry* geo = owner->geo();
  length = geo->AgBlocksFor(agno);
  agfl_size = AgflSize(*geo);

  const uint64_t ag_byte = geo->AgByteOffset(agno);
  agf_buf.assign(geo->sector_size, 0);
  if (!owner->device()->Read(ag_byte + AgfByteOffset(*geo), agf_buf.data(),
                             agf_buf.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read AGF")
        .Ag(agno)
        .Metadata("AGF");
  }
  const uint8_t* af = agf_buf.data();
  if (GetBE32(af, agf::kMagic) != ondisk::kAgfMagic) {
    return XFS_ERR(XfsError::kBadMagic, "AGF magic mismatch")
        .Ag(agno)
        .Metadata("AGF");
  }
  if (GetBE32(af, agf::kVersionnum) != agf::kVersion) {
    return XFS_ERR(XfsError::kBadVersion, "AGF version mismatch")
        .Ag(agno)
        .Metadata("AGF");
  }
  if (GetBE32(af, agf::kSeqno) != agno) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF seqno mismatch")
        .Ag(agno)
        .Metadata("AGF");
  }
  if (GetBE32(af, agf::kLength) != length) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF length mismatch")
        .Ag(agno)
        .Metadata("AGF");
  }
  const uint32_t bno_root = GetBE32(af, agf::kBnoRoot);
  const uint32_t cnt_root = GetBE32(af, agf::kCntRoot);
  const uint32_t bno_level = GetBE32(af, agf::kBnoLevel);
  const uint32_t cnt_level = GetBE32(af, agf::kCntLevel);
  if (!geo->AgbnoValid(agno, bno_root) || !geo->AgbnoValid(agno, cnt_root)) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF btree root out of range")
        .Ag(agno)
        .Metadata("AGF");
  }
  flfirst = GetBE32(af, agf::kFlfirst);
  fllast = GetBE32(af, agf::kFllast);
  flcount = GetBE32(af, agf::kFlcount);
  if (flcount > agfl_size || flfirst >= agfl_size || fllast >= agfl_size) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF freelist out of range")
        .Ag(agno)
        .Metadata("AGF");
  }
  freeblks = GetBE32(af, agf::kFreeblks);
  if (freeblks > length) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF freeblks out of range")
        .Ag(agno)
        .Metadata("AGF");
  }
  orig_freeblks = freeblks;
  orig_flcount = flcount;
  orig_btreeblks = GetBE32(af, agf::kBtreeblks);
  rmap_root = GetBE32(af, agf::kRmapRoot);
  rmap_level = GetBE32(af, agf::kRmapLevel);
  rmap_blocks = GetBE32(af, agf::kRmapBlocks);
  if (geo->features.rmapbt) {
    if (rmap_root == kNullAgBlock || !geo->AgbnoValid(agno, rmap_root) ||
        rmap_level < 1 || rmap_level > bt::kMaxTraversalLevels ||
        rmap_blocks == 0 || rmap_blocks > length) {
      return XFS_ERR(XfsError::kCorruptAgHeader,
                     "AGF rmap root/level/blocks is invalid")
          .Ag(agno)
          .Actual(rmap_root)
          .Metadata("AGF");
    }
  }
  refcount_root = GetBE32(af, agf::kRefcountRoot);
  refcount_level = GetBE32(af, agf::kRefcountLevel);
  refcount_blocks = GetBE32(af, agf::kRefcountBlocks);
  if (geo->features.reflink) {
    if (refcount_root == kNullAgBlock || !geo->AgbnoValid(agno, refcount_root) ||
        refcount_level < 1 || refcount_level > bt::kMaxTraversalLevels ||
        refcount_blocks == 0 || refcount_blocks > length) {
      return XFS_ERR(XfsError::kCorruptAgHeader,
                     "AGF refcount root/level/blocks is invalid")
          .Ag(agno)
          .Actual(refcount_root)
          .Metadata("AGF");
    }
  }

  bno = std::make_unique<AllocTree>(owner, agno, TreeKind::kBno, "bnobt");
  cnt = std::make_unique<AllocTree>(owner, agno, TreeKind::kCnt, "cntbt");
  Status s = bno->Load(bno_root, bno_level);
  if (!s.ok()) return s;
  s = cnt->Load(cnt_root, cnt_level);
  if (!s.ok()) return s;

  recs.clear();
  s = bno->CollectRecords(&recs);
  if (!s.ok()) return s;
  std::vector<FreeRec> cnt_recs;
  s = cnt->CollectRecords(&cnt_recs);
  if (!s.ok()) return s;
  if (cnt_recs.size() != recs.size()) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "bnobt and cntbt record counts differ")
        .Ag(agno)
        .Metadata("free space");
  }
  std::sort(cnt_recs.begin(), cnt_recs.end(),
            [](const FreeRec& a, const FreeRec& b) {
              if (a.start != b.start) return a.start < b.start;
              return a.len < b.len;
            });
  for (size_t i = 0; i < recs.size(); ++i) {
    if (recs[i].start != cnt_recs[i].start || recs[i].len != cnt_recs[i].len) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "bnobt and cntbt records differ")
          .Ag(agno)
          .Metadata("free space")
          .Actual(recs[i].start);
    }
  }
  {
    uint64_t sum = 0;
    for (const FreeRec& r : recs) sum += r.len;
    if (sum != freeblks) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "free space records do not sum to agf_freeblks")
          .Ag(agno)
          .Metadata("AGF")
          .Expected(freeblks)
          .Actual(sum);
    }
  }
  RecomputeLongest();

  agfl_buf.assign(geo->sector_size, 0);
  if (!owner->device()->Read(ag_byte + AgflByteOffset(*geo), agfl_buf.data(),
                             agfl_buf.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read AGFL")
        .Ag(agno)
        .Metadata("AGFL");
  }
  if (GetBE32(agfl_buf.data(), agfl::kMagic) != ondisk::kAgflMagic) {
    return XFS_ERR(XfsError::kBadMagic, "AGFL magic mismatch")
        .Ag(agno)
        .Metadata("AGFL");
  }
  if (GetBE32(agfl_buf.data(), agfl::kSeqno) != agno) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGFL seqno mismatch")
        .Ag(agno)
        .Metadata("AGFL");
  }
  return OkStatus();
}

void AgState::RecomputeLongest() {
  longest = 0;
  for (const FreeRec& r : recs) longest = std::max(longest, r.len);
}

Status AgState::InsertRec(const FreeRec& rec) {
  if (rec.len == 0) {
    return XFS_ERR(XfsError::kInternal, "empty free extent")
        .Ag(agno)
        .Metadata("free space");
  }
  auto it = std::lower_bound(recs.begin(), recs.end(), rec,
                             [](const FreeRec& a, const FreeRec& b) {
                               return a.start < b.start;
                             });
  if (it != recs.begin()) {
    const FreeRec& prev = *(it - 1);
    if (prev.start + prev.len > rec.start) {
      return XFS_ERR(XfsError::kCorruptBtree, "free extent overlaps previous")
          .Ag(agno)
          .Metadata("free space")
          .Actual(rec.start);
    }
  }
  if (it != recs.end() && rec.start + rec.len > it->start) {
    return XFS_ERR(XfsError::kCorruptBtree, "free extent overlaps next")
        .Ag(agno)
        .Metadata("free space")
        .Actual(rec.start);
  }
  Status s = bno->Insert(rec);
  if (!s.ok()) return s;
  s = cnt->Insert(rec);
  if (!s.ok()) return s;
  recs.insert(it, rec);
  agf_dirty = true;
  return OkStatus();
}

Status AgState::RemoveRec(const FreeRec& rec) {
  auto it = std::find_if(recs.begin(), recs.end(), [&](const FreeRec& r) {
    return r.start == rec.start && r.len == rec.len;
  });
  if (it == recs.end()) {
    return XFS_ERR(XfsError::kInternal, "free extent not in the record list")
        .Ag(agno)
        .Metadata("free space")
        .Actual(rec.start);
  }
  Status s = bno->Remove(rec);
  if (!s.ok()) return s;
  s = cnt->Remove(rec);
  if (!s.ok()) return s;
  recs.erase(it);
  agf_dirty = true;
  return OkStatus();
}

Status AgState::GetTreeBlock(uint32_t* agbno) {
  if (flcount == 0) {
    if (owner->rmap_editing()) {
      // The reservation failed earlier; never recurse into rmap mapping while
      // the rmap tree is already being edited.
      return XFS_ERR(XfsError::kNoSpace,
                     "AGFL reservation exhausted during an rmap edit")
          .Ag(agno)
          .Metadata("AGFL");
    }
    Status s = owner->EnsureAgflInternal(agno, 1);
    if (!s.ok()) return s;
  }
  if (flcount == 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "AGFL is empty and cannot be refilled")
        .Ag(agno)
        .Metadata("AGFL");
  }
  const uint32_t agb = GetBE32(agfl_buf.data(), kAgflHeaderSize + flfirst * 4);
  if (agb >= length) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGFL entry out of range")
        .Ag(agno)
        .Metadata("AGFL")
        .Actual(agb);
  }
  flfirst = (flfirst + 1) % agfl_size;
  flcount--;
  agfl_dirty = true;
  agf_dirty = true;
  *agbno = agb;
  return OkStatus();
}

Status AgState::PutTreeBlock(uint32_t agbno) {
  if (agbno >= length) {
    return XFS_ERR(XfsError::kInternal, "freeing an out of range AGFL block")
        .Ag(agno)
        .Metadata("AGFL")
        .Actual(agbno);
  }
  if (flcount >= agfl_size) {
    // Never recurse into OWN_AG unmap while the rmap editor is rebalancing.
    // Ownership stays OWN_AG until Commit drains the overflow, before any
    // tree/header publication or free-space counter calculation.
    deferred_tree_blocks.push_back(agbno);
    agf_dirty = true;
    return OkStatus();
  }
  fllast = (fllast + 1) % agfl_size;
  PutBE32(agfl_buf.data(), kAgflHeaderSize + fllast * 4, agbno);
  flcount++;
  agfl_dirty = true;
  agf_dirty = true;
  return OkStatus();
}

Status AgState::DrainDeferredTreeBlocks() {
  while (!deferred_tree_blocks.empty()) {
    if (flcount >= agfl_size) {
      Status s = DrainAgfl(1);
      if (!s.ok()) return s;
      // Unmapping the drained block can itself shrink rmap and return more
      // tree blocks. Iterate to a fixed point rather than recurse.
      continue;
    }
    const uint32_t agbno = deferred_tree_blocks.back();
    deferred_tree_blocks.pop_back();
    Status s = PutTreeBlock(agbno);
    if (!s.ok()) return s;
  }
  return OkStatus();
}

Status AgState::RefillAgflRaw(uint32_t want, std::vector<FreeRec>* added) {
  if (refilling) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "AGFL refill would recurse into itself")
        .Ag(agno)
        .Metadata("AGFL");
  }
  if (want > agfl_size) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "AGFL refill target exceeds the ring size")
        .Ag(agno)
        .Metadata("AGFL");
  }
  refilling = true;
  Status result = OkStatus();
  while (flcount < want) {
    const FreeRec* best = nullptr;
    for (const FreeRec& r : recs) {
      if (r.len >= 1 && (best == nullptr || r.len > best->len)) best = &r;
    }
    if (best == nullptr) {
      result = XFS_ERR(XfsError::kNoSpace,
                       "no free space available to refill the AGFL")
                   .Ag(agno)
                   .Metadata("AGFL");
      break;
    }
    const FreeRec old = *best;
    const uint32_t take = std::min<uint32_t>(want - flcount, old.len);
    if (take > agfl_size - flcount) {
      result = XFS_ERR(XfsError::kUnsupportedFeature,
                       "AGFL refill would overflow the ring")
                   .Ag(agno)
                   .Metadata("AGFL");
      break;
    }
    result = RemoveRec(old);
    if (!result.ok()) break;
    if (old.len > take) {
      result = InsertRec(FreeRec{old.start + take, old.len - take});
      if (!result.ok()) break;
    }
    freeblks -= take;
    for (uint32_t i = 0; i < take; ++i) {
      result = PutTreeBlock(old.start + i);
      if (!result.ok()) break;
    }
    if (!result.ok()) break;
    if (added != nullptr) added->push_back(FreeRec{old.start, take});
  }
  refilling = false;
  // Refill removes blocks from the free trees even when no data allocation
  // follows (e.g. RUI completion). Keep the AGF maximum in sync with recs;
  // callers must not have to perform a later Allocate/Free to refresh it.
  RecomputeLongest();
  return result;
}

Status AgState::DrainAgfl(uint32_t count) {
  while (count > 0 && flcount > 0) {
    const uint32_t agb =
        GetBE32(agfl_buf.data(), kAgflHeaderSize + flfirst * 4);
    if (agb >= length) {
      return XFS_ERR(XfsError::kCorruptAgHeader, "AGFL entry out of range")
          .Ag(agno)
          .Actual(agb)
          .Metadata("AGFL");
    }
    // Remove it from the active ring before editing OWN_AG: an unmap can
    // split rmap and allocate a tree block, which must not pop this same block
    // while it is being returned to ordinary free space.
    flfirst = (flfirst + 1) % agfl_size;
    flcount--;
    agfl_dirty = true;
    agf_dirty = true;
    if (owner->rmap_enabled()) {
      Status s = owner->UnmapMetadata(agno, agb, 1, kRmapOwnerAg);
      if (!s.ok()) return s;
    }

    // Return the block to ordinary free space, merging both neighbours.
    FreeRec left;
    bool have_left = false;
    FreeRec right;
    bool have_right = false;
    for (const FreeRec& r : recs) {
      if (r.start + r.len == agb) {
        left = r;
        have_left = true;
      }
      if (r.start == agb + 1) {
        right = r;
        have_right = true;
      }
    }
    Status s = OkStatus();
    if (have_left && have_right) {
      s = RemoveRec(left);
      if (s.ok()) s = RemoveRec(right);
      if (s.ok()) {
        s = InsertRec(FreeRec{left.start, left.len + 1 + right.len});
      }
    } else if (have_left) {
      s = RemoveRec(left);
      if (s.ok()) s = InsertRec(FreeRec{left.start, left.len + 1});
    } else if (have_right) {
      s = RemoveRec(right);
      if (s.ok()) s = InsertRec(FreeRec{agb, 1 + right.len});
    } else {
      s = InsertRec(FreeRec{agb, 1});
    }
    if (!s.ok()) return s;
    ++freeblks;
    --count;
  }
  RecomputeLongest();
  return OkStatus();
}

Status AgState::AllocateBestFit(uint32_t want, const RmapOwner* rmap_owner,
                                Allocation* out) {
  if (this->owner->rmap_enabled() && rmap_owner == nullptr) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmapbt allocation requires an owner")
        .Ag(agno)
        .Metadata("allocator");
  }
  const FreeRec* best = nullptr;
  for (const FreeRec& r : recs) {
    if (r.len < want) continue;
    if (best == nullptr || r.len < best->len ||
        (r.len == best->len && r.start < best->start)) {
      best = &r;
    }
  }
  if (best == nullptr) {
    return XFS_ERR(XfsError::kNoSpace, "AG has no free extent that large")
        .Ag(agno)
        .Metadata("free space")
        .Actual(want);
  }
  const FreeRec old = *best;
  Status s = RemoveRec(old);
  if (!s.ok()) return s;
  if (old.len > want) {
    s = InsertRec(FreeRec{old.start + want, old.len - want});
    if (!s.ok()) return s;
  }
  freeblks -= want;
  RecomputeLongest();
  agf_dirty = true;
  if (rmap_owner != nullptr) {
    const Status map_status =
        this->owner->MapOwned(agno, old.start, want, *rmap_owner);
    if (!map_status.ok()) return map_status;
  }
  out->fsb = this->owner->geo()->AgFsb(agno, old.start);
  out->len = want;
  return OkStatus();
}

Status AgState::AllocateAligned(uint32_t want, uint32_t align,
                                const RmapOwner* rmap_owner, Allocation* out,
                                uint32_t min_agbno, uint32_t max_agbno) {
  if (this->owner->rmap_enabled() && rmap_owner == nullptr) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmapbt allocation requires an owner")
        .Ag(agno)
        .Metadata("allocator");
  }
  if (align <= 1 && min_agbno == 0 && max_agbno >= length) {
    return AllocateBestFit(want, rmap_owner, out);
  }
  if (align <= 1) align = 1;
  // Best fit among the free extents that contain an aligned run of `want`
  // blocks inside [min_agbno, max_agbno]. The candidate with the smallest
  // extent wins, then the one whose aligned start is closest to the extent
  // start, then the lowest start.
  const FreeRec* best = nullptr;
  uint32_t best_off = 0;
  uint32_t best_slack = 0;
  for (const FreeRec& r : recs) {
    if (r.len < want) continue;
    const uint64_t range_end = std::min<uint64_t>(
        static_cast<uint64_t>(r.start) + r.len, max_agbno);
    if (range_end < r.start + want) continue;
    uint32_t off = (r.start + align - 1) & ~(align - 1);
    if (off < r.start) continue;  // overflow cannot happen inside an AG
    if (off < min_agbno) {
      off = (min_agbno + align - 1) & ~(align - 1);
    }
    if (off < r.start || off > range_end ||
        want > range_end - off) {
      continue;
    }
    const uint32_t slack = off - r.start;
    if (best == nullptr || r.len < best->len ||
        (r.len == best->len &&
         (slack < best_slack || (slack == best_slack && r.start < best->start)))) {
      best = &r;
      best_off = off;
      best_slack = slack;
    }
  }
  if (best == nullptr) {
    return XFS_ERR(XfsError::kNoSpace,
                   "AG has no free extent with an aligned run that large")
        .Ag(agno)
        .Metadata("free space")
        .Actual(want);
  }
  const FreeRec old = *best;
  Status s = RemoveRec(old);
  if (!s.ok()) return s;
  if (best_off > old.start) {
    s = InsertRec(FreeRec{old.start, best_off - old.start});
    if (!s.ok()) return s;
  }
  if (old.start + old.len > best_off + want) {
    s = InsertRec(FreeRec{best_off + want, old.start + old.len - (best_off + want)});
    if (!s.ok()) return s;
  }
  freeblks -= want;
  RecomputeLongest();
  agf_dirty = true;
  if (rmap_owner != nullptr) {
    Status mapped = this->owner->MapOwned(agno, best_off, want, *rmap_owner);
    if (!mapped.ok()) return mapped;
  }
  out->fsb = this->owner->geo()->AgFsb(agno, best_off);
  out->len = want;
  return OkStatus();
}

// ---------------------------------------------------------------------------
// Allocator
// ---------------------------------------------------------------------------

}  // namespace alloc_detail
}  // namespace xfs
