// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Owner: the public allocation API only.
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
#include "xfs/concurrency_hooks.h"

namespace xfs {

#ifdef XFSWIN_CONCURRENCY_HOOKS
class AllocPhaseHook {
 public:
  AllocPhaseHook(const char* point, std::string detail)
      : point_(point), detail_(std::move(detail)) {
    XFS_CONC_SYNC(point_, detail_);
  }
  ~AllocPhaseHook() { XFS_CONC_SYNC("alloc.phase.after", detail_); }

 private:
  const char* point_;
  std::string detail_;
};
#define XFS_ALLOC_PHASE(name, detail) \
  ::xfs::AllocPhaseHook conc_phase((name), (detail))
#else
#define XFS_ALLOC_PHASE(name, detail) \
  do {                                \
  } while (0)
#endif

using alloc_detail::AgState;
using alloc_detail::Allocator;

class BlockAllocator::Impl {
 public:
  std::unique_ptr<Allocator> alloc;
};

BlockAllocator::BlockAllocator(XfsTransaction* txn, BlockDevice* device,
                               const XfsGeometry* geo)
    : impl_(std::make_unique<Impl>()) {
  impl_->alloc = std::make_unique<Allocator>(txn, device, geo);
}

BlockAllocator::~BlockAllocator() = default;

Status BlockAllocator::Allocate(uint32_t preferred_agno, uint32_t len,
                                Allocation* out) {
  XFS_ALLOC_PHASE("alloc.allocate", "agno=" + std::to_string(preferred_agno) + " len=" + std::to_string(len));
  if (impl_->alloc->rmap_enabled()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmapbt allocation requires an owner")
        .Metadata("allocator");
  }
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot allocate zero blocks")
        .Metadata("allocator");
  }
  if (preferred_agno >= impl_->alloc->geo()->agcount) preferred_agno = 0;
  const uint32_t agcount = impl_->alloc->geo()->agcount;
  for (uint32_t i = 0; i < agcount; ++i) {
    const uint32_t agno = (preferred_agno + i) % agcount;
    AgState* ag = nullptr;
    Status s = impl_->alloc->LoadAg(agno, &ag);
    if (!s.ok()) return s;
    s = ag->AllocateBestFit(len, nullptr, out);
    if (s.ok()) return s;
    if (s.error().code != XfsError::kNoSpace) return s;
  }
  return XFS_ERR(XfsError::kNoSpace, "no AG has a free extent that large")
      .Metadata("allocator")
      .Actual(len);
}

Status BlockAllocator::AllocateOwned(uint32_t preferred_agno, uint32_t len,
                                     const RmapOwner& owner, Allocation* out) {
  XFS_ALLOC_PHASE("alloc.allocate.owned", "agno=" + std::to_string(preferred_agno) + " len=" + std::to_string(len));
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot allocate zero blocks")
        .Metadata("allocator");
  }
  if (preferred_agno >= impl_->alloc->geo()->agcount) preferred_agno = 0;
  const uint32_t agcount = impl_->alloc->geo()->agcount;
  for (uint32_t i = 0; i < agcount; ++i) {
    const uint32_t agno = (preferred_agno + i) % agcount;
    Status s = impl_->alloc->ReserveForRmap(agno);
    if (!s.ok()) {
      if (s.error().code == XfsError::kNoSpace) continue;
      return s;
    }
    AgState* ag = nullptr;
    s = impl_->alloc->LoadAg(agno, &ag);
    if (!s.ok()) return s;
    s = ag->AllocateBestFit(len, &owner, out);
    if (s.ok()) return s;
    if (s.error().code != XfsError::kNoSpace) return s;
  }
  return XFS_ERR(XfsError::kNoSpace, "no AG has a free extent that large")
      .Metadata("allocator")
      .Actual(len);
}

Status BlockAllocator::AllocateFromAg(uint32_t agno, uint32_t len,
                                      uint32_t align, Allocation* out) {
  const XfsGeometry* geo = impl_->alloc->geo();
  if (agno >= geo->agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("allocator");
  }
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot allocate zero blocks")
        .Metadata("allocator");
  }
  if (align <= 1) align = 1;
  if ((align & (align - 1)) != 0) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "alignment must be a power of two")
        .Metadata("allocator")
        .Actual(align);
  }
  if (impl_->alloc->rmap_enabled()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmapbt allocation requires an owner")
        .Ag(agno)
        .Metadata("allocator");
  }
  AgState* ag = nullptr;
  Status s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  return ag->AllocateAligned(len, align, nullptr, out);
}

Status BlockAllocator::AllocateFromAgOwned(uint32_t agno, uint32_t len,
                                           uint32_t align,
                                           const RmapOwner& owner,
                                           Allocation* out) {
  const XfsGeometry* geo = impl_->alloc->geo();
  if (agno >= geo->agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("allocator");
  }
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot allocate zero blocks")
        .Metadata("allocator");
  }
  if (align <= 1) align = 1;
  if ((align & (align - 1)) != 0) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "alignment must be a power of two")
        .Metadata("allocator")
        .Actual(align);
  }
  Status s = impl_->alloc->ReserveForRmap(agno);
  if (!s.ok()) return s;
  AgState* ag = nullptr;
  s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  return ag->AllocateAligned(len, align, &owner, out);
}

Status BlockAllocator::AllocateFromAgRange(uint32_t agno, uint32_t len,
                                           uint32_t align, uint32_t min_agbno,
                                           uint32_t max_agbno,
                                           Allocation* out) {
  const XfsGeometry* geo = impl_->alloc->geo();
  if (agno >= geo->agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("allocator");
  }
  if (len == 0 || min_agbno >= max_agbno) {
    return XFS_ERR(XfsError::kInvalidArgument, "invalid allocation range")
        .Metadata("allocator");
  }
  if (align <= 1) align = 1;
  if ((align & (align - 1)) != 0) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "alignment must be a power of two")
        .Metadata("allocator")
        .Actual(align);
  }
  if (impl_->alloc->rmap_enabled()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmapbt allocation requires an owner")
        .Ag(agno)
        .Metadata("allocator");
  }
  AgState* ag = nullptr;
  Status s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  return ag->AllocateAligned(len, align, nullptr, out, min_agbno, max_agbno);
}

Status BlockAllocator::AllocateFromAgRangeOwned(
    uint32_t agno, uint32_t len, uint32_t align, uint32_t min_agbno,
    uint32_t max_agbno, const RmapOwner& owner, Allocation* out) {
  const XfsGeometry* geo = impl_->alloc->geo();
  if (agno >= geo->agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("allocator");
  }
  if (len == 0 || min_agbno >= max_agbno) {
    return XFS_ERR(XfsError::kInvalidArgument, "invalid allocation range")
        .Metadata("allocator");
  }
  if (align <= 1) align = 1;
  if ((align & (align - 1)) != 0) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "alignment must be a power of two")
        .Metadata("allocator")
        .Actual(align);
  }
  Status s = impl_->alloc->ReserveForRmap(agno);
  if (!s.ok()) return s;
  AgState* ag = nullptr;
  s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  return ag->AllocateAligned(len, align, &owner, out, min_agbno, max_agbno);
}

Status BlockAllocator::Free(uint64_t fsb, uint32_t len) {
  XFS_ALLOC_PHASE("alloc.free", "fsb=" + std::to_string(fsb) + " len=" + std::to_string(len));
  if (impl_->alloc->rmap_enabled()) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmapbt free requires an owner")
        .Fsb(fsb)
        .Metadata("allocator");
  }
  return impl_->alloc->FreeBlocksInternal(fsb, len, nullptr, false);
}

Status BlockAllocator::FreeOwned(uint64_t fsb, uint32_t len,
                                 const RmapOwner& owner) {
  XFS_ALLOC_PHASE("alloc.free.owned", "fsb=" + std::to_string(fsb) + " len=" + std::to_string(len));
  return impl_->alloc->FreeBlocksInternal(fsb, len, &owner, false);
}

Status BlockAllocator::FreeUnknown(uint64_t fsb, uint32_t len) {
  XFS_ALLOC_PHASE("alloc.free.unknown", "fsb=" + std::to_string(fsb) + " len=" + std::to_string(len));
  if (!impl_->alloc->rmap_enabled()) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "OWN_UNKNOWN free requires rmapbt")
        .Fsb(fsb)
        .Metadata("allocator");
  }
  return impl_->alloc->FreeBlocksInternal(fsb, len, nullptr, true);
}

Status BlockAllocator::InvalidateMetadata(uint64_t fsb, uint32_t len,
                                          const char* what) {
  return impl_->alloc->InvalidateMetadata(fsb, len, what);
}

Status BlockAllocator::ReleaseData(uint64_t fsb, uint32_t len,
                                   const RmapOwner& owner) {
  XFS_ALLOC_PHASE("alloc.release", "fsb=" + std::to_string(fsb) + " len=" + std::to_string(len));
  return impl_->alloc->ReleaseDataInternal(fsb, len, owner);
}

Status BlockAllocator::AllocateCowStaging(uint32_t preferred_agno,
                                          uint32_t len, Allocation* out) {
  XFS_ALLOC_PHASE("alloc.cow.staging", "len=" + std::to_string(len));
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot allocate zero blocks")
        .Metadata("allocator");
  }
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->features.reflink) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "CoW staging requires reflink")
        .Metadata("refcountbt");
  }
  if (preferred_agno >= geo->agcount) preferred_agno = 0;
  for (uint32_t i = 0; i < geo->agcount; ++i) {
    const uint32_t agno = (preferred_agno + i) % geo->agcount;
    Status s = impl_->alloc->ReserveForRmap(agno);
    if (!s.ok()) {
      if (s.error().code == XfsError::kNoSpace) continue;
      return s;
    }
    AgState* ag = nullptr;
    s = impl_->alloc->LoadAg(agno, &ag);
    if (!s.ok()) return s;
    const RmapOwner owner = RmapOwner::Metadata(kRmapOwnerCow);
    s = ag->AllocateBestFit(len, &owner, out);
    if (!s.ok()) {
      if (s.error().code == XfsError::kNoSpace) continue;
      return s;
    }
    const uint32_t agbno =
        static_cast<uint32_t>(out->fsb & ((1ULL << geo->agblklog) - 1));
    s = impl_->alloc->CowAllocInternal(agno, agbno, out->len);
    if (!s.ok()) return s;
    return OkStatus();
  }
  return XFS_ERR(XfsError::kNoSpace, "no AG has a free extent that large")
      .Metadata("allocator")
      .Actual(len);
}

Status BlockAllocator::CommitCowStaging(uint64_t fsb, uint32_t len,
                                        const RmapOwner& final_owner) {
  XFS_ALLOC_PHASE("alloc.cow.commit", "fsb=" + std::to_string(fsb) + " len=" + std::to_string(len));
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->features.reflink) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "CoW staging requires reflink")
        .Fsb(fsb)
        .Metadata("refcountbt");
  }
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "CoW staging range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("refcountbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  Status s = impl_->alloc->CowFreeInternal(agno, agbno, len);
  if (!s.ok()) return s;
  if (impl_->alloc->rmap_enabled()) {
    s = impl_->alloc->UnmapOwned(
        agno, agbno, len, RmapOwner::Metadata(kRmapOwnerCow), false);
    if (!s.ok()) return s;
    s = impl_->alloc->MapOwned(agno, agbno, len, final_owner);
    if (!s.ok()) return s;
  }
  return OkStatus();
}

Status BlockAllocator::ReleaseCowStaging(uint64_t fsb, uint32_t len) {
  XFS_ALLOC_PHASE("alloc.cow.release", "fsb=" + std::to_string(fsb) + " len=" + std::to_string(len));
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->features.reflink) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "CoW staging requires reflink")
        .Fsb(fsb)
        .Metadata("refcountbt");
  }
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "CoW staging range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("refcountbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  Status s = impl_->alloc->CowFreeInternal(agno, agbno, len);
  if (!s.ok()) return s;
  if (impl_->alloc->rmap_enabled()) {
    s = impl_->alloc->UnmapOwned(
        agno, agbno, len, RmapOwner::Metadata(kRmapOwnerCow), false);
    if (!s.ok()) return s;
  }
  return impl_->alloc->FreeRaw(agno, agbno, len);
}

Status BlockAllocator::RefcountIncrease(uint64_t fsb, uint32_t len) {
  XFS_ALLOC_PHASE("alloc.refcount.increase", "fsb=" + std::to_string(fsb) + " len=" + std::to_string(len));
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "refcount increase range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("refcountbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  return impl_->alloc->RefcountIncreaseInternal(agno, agbno, len);
}

Status BlockAllocator::RefcountDecrease(uint64_t fsb, uint32_t len,
                                        bool unknown_owner) {
  XFS_ALLOC_PHASE("alloc.refcount.decrease", "fsb=" + std::to_string(fsb) + " len=" + std::to_string(len));
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "refcount decrease range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("refcountbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  return impl_->alloc->RefcountDecreaseInternal(agno, agbno, len,
                                                unknown_owner, true);
}

Status BlockAllocator::RefcountCowAlloc(uint64_t fsb, uint32_t len) {
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "CoW refcount range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("refcountbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  return impl_->alloc->CowAllocInternal(agno, agbno, len);
}

Status BlockAllocator::RefcountCowFree(uint64_t fsb, uint32_t len) {
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "CoW refcount range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("refcountbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  return impl_->alloc->CowFreeInternal(agno, agbno, len);
}

Status BlockAllocator::FindShared(uint64_t fsb, uint32_t len,
                                  RefcountRecord* out, bool* found) {
  *found = false;
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->features.reflink) return OkStatus();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "shared query range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("refcountbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  Status s = impl_->alloc->LoadRefcount(agno);
  if (!s.ok()) return s;
  *found = impl_->alloc->FindSharedExtent(fsb, len, out);
  return OkStatus();
}

Status BlockAllocator::LoadRefcountAg(uint32_t agno) {
  return impl_->alloc->LoadRefcount(agno);
}

Status BlockAllocator::ListCowStaging(uint32_t agno,
                                      std::vector<RefcountRecord>* out) {
  return impl_->alloc->ListCowStaging(agno, out);
}

Status BlockAllocator::ConvertOwned(uint64_t fsb, uint32_t len,
                                    const RmapOwner& owner,
                                    bool old_unwritten) {
  if (!impl_->alloc->rmap_enabled()) return OkStatus();
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "convert range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("allocator");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  Status s = impl_->alloc->ReserveForRmap(agno);
  if (!s.ok()) return s;
  AgState* ag = nullptr;
  s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  (void)ag;
  return impl_->alloc->ConvertOwnedRmap(agno, agbno, len, owner,
                                        old_unwritten);
}

Status BlockAllocator::RmapMap(uint64_t fsb, uint32_t len,
                               const RmapOwner& owner) {
  if (!impl_->alloc->rmap_enabled()) return OkStatus();
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmap map range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("rmapbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  Status s = impl_->alloc->ReserveForRmap(agno);
  if (!s.ok()) return s;
  AgState* ag = nullptr;
  s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  (void)ag;
  return impl_->alloc->MapOwned(agno, agbno, len, owner);
}

Status BlockAllocator::RmapUnmap(uint64_t fsb, uint32_t len,
                                 const RmapOwner& owner,
                                 bool no_overlap_ok) {
  if (!impl_->alloc->rmap_enabled()) return OkStatus();
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmap unmap range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("rmapbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  Status s = impl_->alloc->ReserveForRmap(agno);
  if (!s.ok()) return s;
  AgState* ag = nullptr;
  s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  (void)ag;
  return impl_->alloc->UnmapOwned(agno, agbno, len, owner, false,
                                  no_overlap_ok);
}

Status BlockAllocator::RmapConvert(uint64_t fsb, uint32_t len,
                                   const RmapOwner& owner,
                                   bool old_unwritten) {
  if (!impl_->alloc->rmap_enabled()) return OkStatus();
  const XfsGeometry* geo = impl_->alloc->geo();
  if (!geo->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "rmap convert range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("rmapbt");
  }
  const uint32_t agno = static_cast<uint32_t>(fsb >> geo->agblklog);
  const uint32_t agbno =
      static_cast<uint32_t>(fsb & ((1ULL << geo->agblklog) - 1));
  Status s = impl_->alloc->ReserveForRmap(agno);
  if (!s.ok()) return s;
  AgState* ag = nullptr;
  s = impl_->alloc->LoadAg(agno, &ag);
  if (!s.ok()) return s;
  (void)ag;
  return impl_->alloc->ConvertOwnedRmap(agno, agbno, len, owner,
                                        old_unwritten);
}

Status BlockAllocator::EnsureAgfl(uint32_t agno, uint32_t blocks) {
  XFS_ALLOC_PHASE("alloc.reserve.agfl", "agno=" + std::to_string(agno) + " blocks=" + std::to_string(blocks));
  const XfsGeometry* geo = impl_->alloc->geo();
  if (agno >= geo->agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("allocator");
  }
  return impl_->alloc->EnsureAgflInternal(agno, blocks);
}

Status BlockAllocator::Commit() { return impl_->alloc->Commit(); }

int64_t BlockAllocator::fdblocks_delta() const {
  return impl_->alloc->fdblocks_delta();
}

}  // namespace xfs
