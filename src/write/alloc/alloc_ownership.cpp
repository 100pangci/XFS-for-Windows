// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Owner: owner-aware release/CoW and refcount/rmap hooks.
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include "xfs/ag.h"
#include "xfs/btree.h"
#include "xfs/endian.h"
#include "xfs/perf_stats.h"
#include "xfs/refcount_editor.h"
#include "write/alloc/alloc_internal.h"

namespace xfs {
namespace alloc_detail {

uint32_t Allocator::RmapBlocks(uint32_t agno) const {
  if (!rmap_) return 0;
  return rmap_->Blocks(agno);
}

Status Allocator::LoadRefcount(uint32_t agno) {
  if (!reflink_enabled()) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "filesystem does not have reflink enabled")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  AgState* ag = nullptr;
  Status s = LoadAg(agno, &ag);
  if (!s.ok()) return s;
  if (!ag->refcount_loaded) {
    if (PerfStats::Get().enabled()) {
      PerfStats::Get().refcount_tree_loads.fetch_add(1,
                                                   std::memory_order_relaxed);
    }
    s = refcount_->Load(agno, ag->refcount_root, ag->refcount_level,
                        ag->refcount_blocks);
    if (!s.ok()) return s;
    ag->refcount_loaded = true;
  }
  return OkStatus();
}

Status Allocator::MapOwned(uint32_t agno, uint32_t agbno, uint32_t len,
                           const RmapOwner& owner) {
  if (!rmap_enabled()) return OkStatus();
  if (rmap_editing_) {
    return XFS_ERR(XfsError::kInternal,
                   "recursive rmap ownership update")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  rmap_editing_ = true;
  Status s = rmap_->AllocateExtent(agno, agbno, len, owner);
  rmap_editing_ = false;
  if (s.ok()) {
    auto it = ags_.find(agno);
    if (it != ags_.end()) it->second->agf_dirty = true;
  }
  return s;
}

Status Allocator::UnmapOwned(uint32_t agno, uint32_t agbno, uint32_t len,
                             const RmapOwner& owner, bool unknown_owner,
                             bool no_overlap_ok) {
  if (!rmap_enabled()) return OkStatus();
  if (rmap_editing_) {
    return XFS_ERR(XfsError::kInternal,
                   "recursive rmap ownership update")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  rmap_editing_ = true;
  Status s = rmap_->FreeExtent(agno, agbno, len, owner, unknown_owner,
                                no_overlap_ok);
  rmap_editing_ = false;
  if (s.ok()) {
    auto it = ags_.find(agno);
    if (it != ags_.end()) it->second->agf_dirty = true;
  }
  return s;
}

Status Allocator::ConvertOwnedRmap(uint32_t agno, uint32_t agbno,
                                   uint32_t len, const RmapOwner& owner,
                                   bool old_unwritten) {
  if (!rmap_enabled()) return OkStatus();
  if (rmap_editing_) {
    return XFS_ERR(XfsError::kInternal,
                   "recursive rmap ownership update")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  rmap_editing_ = true;
  Status s = rmap_->Convert(agno, agbno, len, owner, old_unwritten);
  rmap_editing_ = false;
  if (s.ok()) {
    auto it = ags_.find(agno);
    if (it != ags_.end()) it->second->agf_dirty = true;
  }
  return s;
}

Status Allocator::MapMetadata(uint32_t agno, uint32_t agbno, uint32_t len,
                              uint64_t owner) {
  if (!rmap_enabled()) return OkStatus();
  if (rmap_editing_) {
    return XFS_ERR(XfsError::kInternal,
                   "recursive metadata ownership update")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  rmap_editing_ = true;
  Status s = rmap_->AllocateMetadata(agno, agbno, len, owner);
  rmap_editing_ = false;
  if (s.ok()) {
    auto it = ags_.find(agno);
    if (it != ags_.end()) it->second->agf_dirty = true;
  }
  return s;
}

Status Allocator::UnmapMetadata(uint32_t agno, uint32_t agbno, uint32_t len,
                                uint64_t owner) {
  if (!rmap_enabled()) return OkStatus();
  if (rmap_editing_) {
    return XFS_ERR(XfsError::kInternal,
                   "recursive metadata ownership update")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  rmap_editing_ = true;
  Status s = rmap_->FreeMetadata(agno, agbno, len, owner);
  rmap_editing_ = false;
  if (s.ok()) {
    auto it = ags_.find(agno);
    if (it != ags_.end()) it->second->agf_dirty = true;
  }
  return s;
}

Status Allocator::AllocateRmapBlock(uint32_t agno, uint32_t* agbno) {
  auto it = ags_.find(agno);
  if (it == ags_.end()) {
    return XFS_ERR(XfsError::kInternal,
                   "rmap tree block allocation before the AG is loaded")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  return it->second->GetTreeBlock(agbno);
}

Status Allocator::FreeRmapBlock(uint32_t agno, uint32_t agbno) {
  auto it = ags_.find(agno);
  if (it == ags_.end()) {
    return XFS_ERR(XfsError::kInternal,
                   "rmap tree block free before the AG is loaded")
        .Ag(agno)
        .Metadata("rmapbt");
  }
  return it->second->PutTreeBlock(agbno);
}

Status Allocator::AllocateRefcountBlock(uint32_t agno, uint32_t* agbno) {
  auto it = ags_.find(agno);
  if (it == ags_.end()) {
    return XFS_ERR(XfsError::kInternal,
                   "refcount tree block allocation before the AG is loaded")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  const RmapOwner owner = RmapOwner::Metadata(kRmapOwnerRefc);
  Allocation allocation;
  Status s = it->second->AllocateBestFit(1, &owner, &allocation);
  if (!s.ok()) return s;
  if (allocation.len != 1) {
    return XFS_ERR(XfsError::kInternal,
                   "refcount block allocation returned more than one block")
        .Ag(agno)
        .Actual(allocation.len)
        .Metadata("refcountbt");
  }
  *agbno = static_cast<uint32_t>(allocation.fsb &
                                 ((1ULL << geo_->agblklog) - 1));
  ags_[agno]->agf_dirty = true;
  return OkStatus();
}

Status Allocator::FreeRefcountBlock(uint32_t agno, uint32_t agbno) {
  if (ags_.count(agno) == 0) {
    return XFS_ERR(XfsError::kInternal,
                   "refcount tree block free before the AG is loaded")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  pending_refcount_frees_[agno].push_back(agbno);
  ags_[agno]->agf_dirty = true;
  return OkStatus();
}

Status Allocator::FreePendingRefcountBlocks() {
  for (auto& entry : pending_refcount_frees_) {
    const uint32_t agno = entry.first;
    std::vector<uint32_t> blocks = std::move(entry.second);
    entry.second.clear();
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
    for (uint32_t agbno : blocks) {
      const uint64_t fsb = geo_->AgFsb(agno, agbno);
      const RmapOwner owner = RmapOwner::Metadata(kRmapOwnerRefc);
      Status s = FreeBlocksInternal(fsb, 1, &owner, false);
      if (!s.ok()) return s;
    }
  }
  pending_refcount_frees_.clear();
  return OkStatus();
}

Status Allocator::EnsureAgflInternal(uint32_t agno, uint32_t want) {
  AgState* ag = nullptr;
  Status s = LoadAg(agno, &ag);
  if (!s.ok()) return s;
  if (!rmap_enabled()) {
    return ag->RefillAgflRaw(want, nullptr);
  }
  // Reserve for the current rmap height plus the caller's request.  Mapping
  // the newly-added ranges can itself split the rmap tree; those splits take
  // blocks from the AGFL, so the refill target must already include the
  // worst-case reserve.  The loop handles blocks consumed by that split.
  for (uint32_t attempt = 0; attempt < 8; ++attempt) {
    const uint32_t reserve =
        std::max<uint32_t>(rmap_->ReserveBlocks(agno) + 2, want);
    if (ag->flcount >= reserve) return OkStatus();
    std::vector<FreeRec> added;
    s = ag->RefillAgflRaw(reserve, &added);
    if (!s.ok()) return s;
    for (const FreeRec& rec : added) {
      s = MapMetadata(agno, rec.start, rec.len, kRmapOwnerAg);
      if (!s.ok()) return s;
    }
    if (ag->flcount >= reserve) return OkStatus();
  }
  return XFS_ERR(XfsError::kNoSpace,
                 "could not establish the rmap AGFL reservation")
      .Ag(agno)
      .Metadata("AGFL");
}

Status Allocator::ReserveForRmap(uint32_t agno) {
  if (!rmap_enabled()) return OkStatus();
  return EnsureAgflInternal(agno, rmap_->ReserveBlocks(agno) + 2);
}


Status Allocator::RefcountFreeCallback(uint32_t agno, uint32_t agbno,
                                       uint32_t len) {
  if (refcount_free_unknown_) {
    const uint64_t fsb = geo_->AgFsb(agno, agbno);
    return FreeBlocksInternal(fsb, len, nullptr, true);
  }
  return FreeRaw(agno, agbno, len);
}

Status Allocator::RefcountIncreaseInternal(uint32_t agno, uint32_t agbno,
                                           uint32_t len) {
  if (!reflink_enabled()) return OkStatus();
  Status s = LoadRefcount(agno);
  if (!s.ok()) return s;
  if (refcount_editing_) {
    return XFS_ERR(XfsError::kInternal,
                   "recursive refcount ownership update")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  refcount_editing_ = true;
  s = refcount_->Increase(agno, agbno, len);
  refcount_editing_ = false;
  return s;
}

Status Allocator::RefcountDecreaseInternal(uint32_t agno, uint32_t agbno,
                                           uint32_t len, bool unmap_unknown,
                                           bool free_raw) {
  if (!reflink_enabled()) {
    return free_raw ? FreeRaw(agno, agbno, len) : OkStatus();
  }
  Status s = LoadRefcount(agno);
  if (!s.ok()) return s;
  if (refcount_editing_) {
    return XFS_ERR(XfsError::kInternal,
                   "recursive refcount ownership update")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  // The range may span several AGs?  Callers always pass one-AG ranges.
  refcount_editing_ = true;
  refcount_free_unknown_ = unmap_unknown;
  s = refcount_->Decrease(agno, agbno, len);
  refcount_free_unknown_ = false;
  refcount_editing_ = false;
  if (!s.ok()) return s;
  (void)free_raw;
  return OkStatus();
}

Status Allocator::CowAllocInternal(uint32_t agno, uint32_t agbno,
                                   uint32_t len) {
  if (!reflink_enabled()) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "CoW staging requires reflink")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  Status s = LoadRefcount(agno);
  if (!s.ok()) return s;
  if (refcount_editing_) {
    return XFS_ERR(XfsError::kInternal, "recursive CoW refcount update")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  refcount_editing_ = true;
  s = refcount_->CowAlloc(agno, agbno, len);
  refcount_editing_ = false;
  return s;
}

Status Allocator::CowFreeInternal(uint32_t agno, uint32_t agbno,
                                  uint32_t len) {
  if (!reflink_enabled()) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "CoW staging requires reflink")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  Status s = LoadRefcount(agno);
  if (!s.ok()) return s;
  if (refcount_editing_) {
    return XFS_ERR(XfsError::kInternal, "recursive CoW refcount update")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  refcount_editing_ = true;
  s = refcount_->CowFree(agno, agbno, len);
  refcount_editing_ = false;
  return s;
}

Status Allocator::ReleaseDataInternal(uint64_t fsb, uint32_t len,
                                      const RmapOwner& owner) {
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot release zero blocks")
        .Fsb(fsb)
        .Metadata("refcountbt");
  }
  if (!geo_->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "release range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("refcountbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo_->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo_->agblklog) - 1));
  AgState* ag = nullptr;
  Status s = LoadAg(agno, &ag);
  if (!s.ok()) return s;
  if (rmap_enabled()) {
    s = ReserveForRmap(agno);
    if (!s.ok()) return s;
    s = UnmapOwned(agno, agbno, len, owner, false);
    if (!s.ok()) return s;
  }
  if (!reflink_enabled()) {
    return FreeRaw(agno, agbno, len);
  }
  // The refcount callback frees ranges whose last reference disappeared.
  return RefcountDecreaseInternal(agno, agbno, len, false, true);
}

bool Allocator::FindSharedExtent(uint64_t fsb, uint32_t len,
                                 RefcountRecord* out) const {
  if (!reflink_enabled()) return false;
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo_->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo_->agblklog) - 1));
  auto it = ags_.find(agno);
  if (it == ags_.end() || !it->second->refcount_loaded) return false;
  return refcount_->FindShared(agno, agbno, len, out);
}

Status Allocator::ListCowStaging(uint32_t agno,
                                 std::vector<RefcountRecord>* out) const {
  out->clear();
  if (!reflink_enabled()) return OkStatus();
  auto it = ags_.find(agno);
  if (it == ags_.end() || !it->second->refcount_loaded) {
    return XFS_ERR(XfsError::kInternal,
                   "refcount AG is not loaded for CoW recovery")
        .Ag(agno)
        .Metadata("refcountbt");
  }
  for (const RefcountRecord& rec : refcount_->Records(agno)) {
    if (rec.cow()) out->push_back(rec);
  }
  return OkStatus();
}


}  // namespace alloc_detail
}  // namespace xfs
