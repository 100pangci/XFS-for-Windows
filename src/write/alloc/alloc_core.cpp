// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Owner: AG loading, counters, superblock delta and Commit().
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "xfs/ag.h"
#include "xfs/btree.h"
#include "xfs/endian.h"
#include "xfs/log_format.h"
#include "xfs/perf_stats.h"
#include "xfs/refcount_editor.h"
#include "write/alloc/alloc_internal.h"

namespace xfs {
namespace alloc_detail {

Allocator::Allocator(XfsTransaction* txn, BlockDevice* device,
                     const XfsGeometry* geo)
    : txn_(txn), device_(device), geo_(geo) {
  if (geo_->features.rmapbt) {
    RmapEditor::Hooks hooks;
    hooks.alloc_block = [this](uint32_t agno, uint32_t* agbno) {
      return AllocateRmapBlock(agno, agbno);
    };
    hooks.free_block = [this](uint32_t agno, uint32_t agbno) {
      return FreeRmapBlock(agno, agbno);
    };
    hooks.publish_block = [this](uint32_t agno, uint32_t agbno,
                                 const uint8_t* data, size_t len) {
      std::vector<uint8_t> buffer(data, data + len);
      const uint64_t offset = FsbToByte(geo_->AgFsb(agno, agbno));
      return txn_->AddMetadataBuffer(offset, std::move(buffer), lf::kBlftBtree,
                                     bt::kSLsn, bt::kSCrc, "rmapbt");
    };
    rmap_ = std::make_unique<RmapEditor>(device_, geo_, std::move(hooks));
  }
  if (geo_->features.reflink) {
    RefcountEditor::Hooks hooks;
    hooks.alloc_block = [this](uint32_t agno, uint32_t* agbno) {
      return AllocateRefcountBlock(agno, agbno);
    };
    hooks.free_block = [this](uint32_t agno, uint32_t agbno) {
      return FreeRefcountBlock(agno, agbno);
    };
    hooks.publish_block = [this](uint32_t agno, uint32_t agbno,
                                 const uint8_t* data, size_t len) {
      std::vector<uint8_t> buffer(data, data + len);
      const uint64_t offset = FsbToByte(geo_->AgFsb(agno, agbno));
      return txn_->AddMetadataBuffer(offset, std::move(buffer), lf::kBlftBtree,
                                     bt::kSLsn, bt::kSCrc, "refcountbt");
    };
    hooks.free_extent = [this](uint32_t agno, uint32_t agbno, uint32_t len) {
      return RefcountFreeCallback(agno, agbno, len);
    };
    refcount_ = std::make_unique<RefcountEditor>(device_, geo_,
                                                 std::move(hooks));
  }
}

Status Allocator::LoadAg(uint32_t agno, AgState** out) {
  auto it = ags_.find(agno);
  if (it != ags_.end()) {
    *out = it->second.get();
    return OkStatus();
  }
  auto state = std::make_unique<AgState>();
  Status s = state->Load(this, agno);
  if (!s.ok()) return s;
  AgState* raw = state.get();
  ags_.emplace(agno, std::move(state));
  *out = raw;
  if (rmap_enabled() && !raw->rmap_loaded) {
    s = rmap_->Load(agno, raw->rmap_root, raw->rmap_level, raw->rmap_blocks);
    if (!s.ok()) {
      ags_.erase(agno);
      return s;
    }
    raw->rmap_loaded = true;
  }
  if (reflink_enabled() && !raw->refcount_loaded) {
    s = refcount_->Load(agno, raw->refcount_root, raw->refcount_level,
                        raw->refcount_blocks);
    if (!s.ok()) {
      ags_.erase(agno);
      return s;
    }
    raw->refcount_loaded = true;
  }
  return OkStatus();
}

Status Allocator::FreeBlocksInternal(uint64_t fsb, uint32_t len,
                                     const RmapOwner* owner,
                                     bool unknown_owner) {
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot free zero blocks")
        .Metadata("allocator");
  }
  if (!geo_->FsbRangeValid(fsb, len)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "free range is not inside one AG")
        .Fsb(fsb)
        .Actual(len)
        .Metadata("allocator");
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
    if (unknown_owner) {
      s = UnmapOwned(agno, agbno, len, RmapOwner{}, true);
    } else if (owner != nullptr) {
      s = UnmapOwned(agno, agbno, len, *owner, false);
    } else {
      return XFS_ERR(XfsError::kInvalidArgument,
                     "rmapbt free requires an owner")
          .Ag(agno)
          .Actual(agbno)
          .Metadata("allocator");
    }
    if (!s.ok()) return s;
  }

  for (const FreeRec& r : ag->recs) {
    if (agbno < r.start + r.len && r.start < agbno + len) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "freeing blocks that are already free")
          .Ag(agno)
          .Actual(agbno)
          .Metadata("free space");
    }
  }
  FreeRec left;
  bool have_left = false;
  FreeRec right;
  bool have_right = false;
  for (const FreeRec& r : ag->recs) {
    if (r.start + r.len == agbno) {
      left = r;
      have_left = true;
    }
    if (r.start == agbno + len) {
      right = r;
      have_right = true;
    }
  }
  if (have_left && have_right) {
    s = ag->RemoveRec(left);
    if (!s.ok()) return s;
    s = ag->RemoveRec(right);
    if (!s.ok()) return s;
    s = ag->InsertRec(FreeRec{left.start, left.len + len + right.len});
  } else if (have_left) {
    s = ag->RemoveRec(left);
    if (!s.ok()) return s;
    s = ag->InsertRec(FreeRec{left.start, left.len + len});
  } else if (have_right) {
    s = ag->RemoveRec(right);
    if (!s.ok()) return s;
    s = ag->InsertRec(FreeRec{agbno, len + right.len});
  } else {
    s = ag->InsertRec(FreeRec{agbno, len});
  }
  if (!s.ok()) return s;
  ag->freeblks += len;
  ag->RecomputeLongest();
  ag->agf_dirty = true;
  return OkStatus();
}

Status Allocator::FreeRaw(uint32_t agno, uint32_t agbno, uint32_t len) {
  if (len == 0) {
    return XFS_ERR(XfsError::kInvalidArgument, "cannot free zero blocks")
        .Ag(agno)
        .Metadata("allocator");
  }
  if (static_cast<uint64_t>(agbno) + len > geo_->AgBlocksFor(agno)) {
    return XFS_ERR(XfsError::kInvalidArgument,
                   "free range is not inside one AG")
        .Ag(agno)
        .Actual(len)
        .Metadata("allocator");
  }
  AgState* ag = nullptr;
  Status s = LoadAg(agno, &ag);
  if (!s.ok()) return s;
  for (const FreeRec& r : ag->recs) {
    if (agbno < r.start + r.len && r.start < agbno + len) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "freeing blocks that are already free")
          .Ag(agno)
          .Actual(agbno)
          .Metadata("free space");
    }
  }
  FreeRec left;
  bool have_left = false;
  FreeRec right;
  bool have_right = false;
  for (const FreeRec& r : ag->recs) {
    if (r.start + r.len == agbno) {
      left = r;
      have_left = true;
    }
    if (r.start == agbno + len) {
      right = r;
      have_right = true;
    }
  }
  if (have_left && have_right) {
    s = ag->RemoveRec(left);
    if (!s.ok()) return s;
    s = ag->RemoveRec(right);
    if (!s.ok()) return s;
    s = ag->InsertRec(FreeRec{left.start, left.len + len + right.len});
  } else if (have_left) {
    s = ag->RemoveRec(left);
    if (!s.ok()) return s;
    s = ag->InsertRec(FreeRec{left.start, left.len + len});
  } else if (have_right) {
    s = ag->RemoveRec(right);
    if (!s.ok()) return s;
    s = ag->InsertRec(FreeRec{agbno, len + right.len});
  } else {
    s = ag->InsertRec(FreeRec{agbno, len});
  }
  if (!s.ok()) return s;
  ag->freeblks += len;
  ag->RecomputeLongest();
  ag->agf_dirty = true;
  return OkStatus();
}

Status Allocator::AddBuffer(uint64_t offset, std::vector<uint8_t> data,
                            uint16_t type, size_t lsn_offset,
                            size_t crc_offset, const char* what) {
  OutBuffer buffer;
  buffer.offset = offset;
  buffer.data = std::move(data);
  buffer.type = type;
  buffer.lsn_offset = lsn_offset;
  buffer.crc_offset = crc_offset;
  buffer.what = what;
  auto it = buffers_.find(offset);
  if (it != buffers_.end()) {
    it->second = std::move(buffer);
  } else {
    buffers_.emplace(offset, std::move(buffer));
  }
  return OkStatus();
}

Status Allocator::Commit() {
  // Settle every transaction-local tree return before rmap buffers are
  // published. Draining can edit the free trees and rmap, including further
  // merges/returns, but never rebuilds either tree.
  for (auto& entry : ags_) {
    Status s = entry.second->DrainDeferredTreeBlocks();
    if (!s.ok()) return s;
  }
  // Refcountbt blocks freed by the editor are queued: free them only now, so
  // a subsequent tree split in the same transaction cannot reuse a block that
  // the editor still holds.  This edits the free trees and rmap.
  Status pending = FreePendingRefcountBlocks();
  if (!pending.ok()) return pending;
  if (refcount_) {
    // Publish refcount blocks before the AGF that points at their new root.
    Status s = refcount_->Commit();
    if (!s.ok()) return s;
  }
  if (rmap_) {
    // Publish rmap tree blocks before the AGF that points at their new root.
    // Both are part of the same buffer-logging transaction; publication order
    // only affects the record layout, not durability.
    Status s = rmap_->Commit();
    if (!s.ok()) return s;
  }
  for (auto& entry : ags_) {
    AgState& ag = *entry.second;

    if (!ag.counted) {
      const uint32_t old_contribution =
          ag.orig_freeblks + ag.orig_flcount + ag.orig_btreeblks;
      const uint32_t new_contribution = ag.Contribution();
      fdblocks_delta_ += static_cast<int64_t>(new_contribution) -
                         static_cast<int64_t>(old_contribution);
      ag.counted = true;
    }

    if (ag.agf_dirty || ag.agfl_dirty) {
      uint8_t* af = ag.agf_buf.data();
      PutBE32(af, agf::kFreeblks, ag.freeblks);
      PutBE32(af, agf::kLongest, ag.longest);
      PutBE32(af, agf::kFlfirst, ag.flfirst);
      PutBE32(af, agf::kFllast, ag.fllast);
      PutBE32(af, agf::kFlcount, ag.flcount);
      uint32_t btreeblks = static_cast<uint32_t>(ag.bno->block_count() - 1) +
                           static_cast<uint32_t>(ag.cnt->block_count() - 1);
      if (rmap_enabled()) {
        const uint32_t blocks = rmap_->Blocks(ag.agno);
        if (blocks > 0) btreeblks += blocks - 1;
        PutBE32(af, agf::kRmapRoot, rmap_->Root(ag.agno));
        PutBE32(af, agf::kRmapLevel, rmap_->Level(ag.agno));
        PutBE32(af, agf::kRmapBlocks, blocks);
      }
      if (reflink_enabled()) {
        // agf_btreeblks deliberately excludes refcountbt (xfs_ag.c); the
        // refcount tree has its own block counter.
        PutBE32(af, agf::kRefcountRoot, refcount_->Root(ag.agno));
        PutBE32(af, agf::kRefcountLevel, refcount_->Level(ag.agno));
        PutBE32(af, agf::kRefcountBlocks, refcount_->Blocks(ag.agno));
      }
      PutBE32(af, agf::kBtreeblks, btreeblks);
      PutBE32(af, agf::kBnoRoot, ag.bno->root());
      PutBE32(af, agf::kCntRoot, ag.cnt->root());
      PutBE32(af, agf::kBnoLevel, ag.bno->level());
      PutBE32(af, agf::kCntLevel, ag.cnt->level());
      const uint64_t offset =
          geo_->AgByteOffset(ag.agno) + AgfByteOffset(*geo_);
      Status s = AddBuffer(offset, ag.agf_buf, lf::kBlftAgf, agf::kLsn,
                           agf::kCrc, "AGF");
      if (!s.ok()) return s;
    }
    if (ag.agfl_dirty) {
      const uint64_t offset =
          geo_->AgByteOffset(ag.agno) + AgflByteOffset(*geo_);
      Status s = AddBuffer(offset, ag.agfl_buf, lf::kBlftAgfl, agfl::kLsn,
                           agfl::kCrc, "AGFL");
      if (!s.ok()) return s;
    }
    for (AllocTree* tree : {ag.bno.get(), ag.cnt.get()}) {
      for (uint32_t agbno : tree->dirty_blocks()) {
        const auto& blocks = tree->blocks();
        auto block_it = blocks.find(agbno);
        if (block_it == blocks.end()) continue;
        const uint64_t offset = FsbToByte(geo_->AgFsb(ag.agno, agbno));
        Status s = AddBuffer(offset, block_it->second, lf::kBlftBtree,
                             bt::kSLsn, bt::kSCrc, "free space btree");
        if (!s.ok()) return s;
      }
    }
  }

  if (fdblocks_delta_ != 0) {
    std::vector<uint8_t> sb_buf;
    const std::vector<uint8_t>* queued = txn_->QueuedBuffer(0);
    if (queued != nullptr) {
      sb_buf = *queued;
    } else {
      sb_buf.assign(geo_->sector_size, 0);
      if (!device_->Read(0, sb_buf.data(), sb_buf.size())) {
        return XFS_ERR(XfsError::kIoError, "failed to read the superblock")
            .Offset(0)
            .Metadata("superblock");
      }
    }
    const uint64_t current = GetBE64(sb_buf.data(), sb::kFdblocks);
    const int64_t updated = static_cast<int64_t>(current) + fdblocks_delta_;
    if (updated < 0 || static_cast<uint64_t>(updated) > geo_->dblocks) {
      return XFS_ERR(XfsError::kInternal,
                     "superblock free block counter would leave its range")
          .Metadata("superblock")
          .Expected(geo_->dblocks)
          .Actual(static_cast<uint64_t>(updated));
    }
    PutBE64(sb_buf.data(), sb::kFdblocks, static_cast<uint64_t>(updated));
    Status s = AddBuffer(0, std::move(sb_buf), lf::kBlftSb, sb::kLsn, sb::kCrc,
                         "superblock");
    if (!s.ok()) return s;
  }

  for (auto& entry : buffers_) {
    OutBuffer& buffer = entry.second;
    Status s = txn_->AddMetadataBuffer(
        buffer.offset, std::move(buffer.data), buffer.type, buffer.lsn_offset,
        buffer.crc_offset, buffer.what);
    if (!s.ok()) return s;
  }
  if (!txn_->record_fits()) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "allocation metadata does not fit in one log record")
        .Metadata("transaction");
  }
  return OkStatus();
}

}  // namespace alloc_detail
}  // namespace xfs
