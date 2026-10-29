// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Owner: bnobt/cntbt structure and ordering.
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

int CompareRec(TreeKind kind, const FreeRec& a, const FreeRec& b) {
  if (kind == TreeKind::kBno) {
    if (a.start != b.start) return a.start < b.start ? -1 : 1;
    if (a.len != b.len) return a.len < b.len ? -1 : 1;
    return 0;
  }
  if (a.len != b.len) return a.len < b.len ? -1 : 1;
  if (a.start != b.start) return a.start < b.start ? -1 : 1;
  return 0;
}

uint32_t AllocTree::owner_block_size() const { return owner_->block_size(); }

const uint8_t* AllocTree::Block(uint32_t agbno) const {
  auto it = blocks_.find(agbno);
  return it == blocks_.end() ? nullptr : it->second.data();
}

uint8_t* AllocTree::MutableBlock(uint32_t agbno) {
  auto it = blocks_.find(agbno);
  if (it == blocks_.end()) return nullptr;
  dirty_.insert(agbno);
  return it->second.data();
}

size_t AllocTree::ChooseChild(const uint8_t* b, const FreeRec& key) const {
  const uint32_t n = NumRecs(b);
  size_t chosen = 0;
  for (size_t i = 0; i < n; ++i) {
    if (CompareRec(kind_, KeyAt(b, i), key) <= 0) {
      chosen = i;
    } else {
      break;
    }
  }
  return chosen;
}

void AllocTree::FixKeysUpward(const std::vector<Frame>& path, uint32_t child) {
  uint32_t cur = child;
  FreeRec key = FirstKey(cur);
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    uint8_t* parent = MutableBlock(it->block);
    if (parent == nullptr || NumRecs(parent) == 0 ||
        it->index >= NumRecs(parent)) {
      break;
    }
    SetKeyAt(parent, it->index, key);
    cur = it->block;
    key = KeyAt(parent, 0);
  }
  (void)cur;
}

std::vector<Entry> AllocTree::ReadEntries(uint32_t agbno) const {
  const uint8_t* b = Block(agbno);
  const uint32_t n = NumRecs(b);
  const uint32_t level = GetBE16(b, bt::kLevel);
  std::vector<Entry> out;
  out.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    Entry entry;
    if (level == 0) {
      entry.key = RecAt(b, i);
    } else {
      entry.key = KeyAt(b, i);
      entry.ptr = PtrAt(b, i);
    }
    out.push_back(entry);
  }
  return out;
}

void AllocTree::WriteEntries(uint32_t agbno, uint32_t level,
                             const std::vector<Entry>& entries) {
  uint8_t* b = MutableBlock(agbno);
  SetNumRecs(b, static_cast<uint32_t>(entries.size()));
  for (size_t i = 0; i < entries.size(); ++i) {
    if (level == 0) {
      SetRecAt(b, i, entries[i].key);
    } else {
      SetKeyAt(b, i, entries[i].key);
      SetPtrAt(b, i, entries[i].ptr);
    }
  }
}

Status AllocTree::InitBlock(uint32_t agbno, uint32_t level, uint32_t numrecs) {
  const XfsGeometry* geo = owner_->geo();
  auto it = blocks_.find(agbno);
  if (it == blocks_.end()) {
    return XFS_ERR(XfsError::kInternal, "initializing an unloaded tree block")
        .Ag(agno_)
        .Metadata(name_)
        .Actual(agbno);
  }
  std::vector<uint8_t>& data = it->second;
  std::memset(data.data(), 0, data.size());
  PutBE32(data.data(), bt::kMagic,
          kind_ == TreeKind::kBno ? ondisk::kBtbCrcMagic
                                  : ondisk::kBtcCrcMagic);
  PutBE16(data.data(), bt::kLevel, static_cast<uint16_t>(level));
  PutBE16(data.data(), bt::kNumrecs, static_cast<uint16_t>(numrecs));
  PutBE32(data.data(), bt::kSLeftsib, kNullAgBlock);
  PutBE32(data.data(), bt::kSRightSib, kNullAgBlock);
  PutBE64(data.data(), bt::kSBlkno,
          owner_->FsbToByte(geo->AgFsb(agno_, agbno)) / 512);
  PutBE64(data.data(), bt::kSLsn, 0);  // stamped at commit
  std::memcpy(data.data() + bt::kSUuid, geo->meta_uuid.data(), 16);
  PutBE32(data.data(), bt::kSOwner, agno_);
  StoreLE32(data.data() + bt::kSCrc, 0);  // stamped at commit
  dirty_.insert(agbno);
  return OkStatus();
}

Status AllocTree::Load(uint32_t root, uint32_t level) {
  root_ = root;
  level_ = level;  // tree height: 1 = a single leaf root (AGF "levels")
  const XfsGeometry* geo = owner_->geo();
  if (level < 1 || level > bt::kMaxTraversalLevels) {
    return XFS_ERR(XfsError::kCorruptBtree, "free space btree level out of range")
        .Ag(agno_)
        .Metadata(name_)
        .Actual(level);
  }
  std::vector<uint32_t> stack{root};
  std::set<uint32_t> seen;
  while (!stack.empty()) {
    const uint32_t agbno = stack.back();
    stack.pop_back();
    if (agbno == kNullAgBlock || agbno >= geo->AgBlocksFor(agno_)) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "free space btree pointer out of range")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(agbno);
    }
    if (!seen.insert(agbno).second) {
      return XFS_ERR(XfsError::kCorruptBtree, "free space btree cycle")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(agbno);
    }
    if (seen.size() > kMaxTreeBlocks) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "free space btree has more blocks than this build can edit")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(seen.size());
    }
    std::vector<uint8_t> data(geo->block_size, 0);
    const uint64_t offset = owner_->FsbToByte(geo->AgFsb(agno_, agbno));
    if (!owner_->device()->Read(offset, data.data(), data.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to read free space btree block")
          .Ag(agno_)
          .Offset(offset)
          .Metadata(name_);
    }
    const uint32_t magic = GetBE32(data.data(), bt::kMagic);
    const uint32_t expected = kind_ == TreeKind::kBno ? ondisk::kBtbCrcMagic
                                                      : ondisk::kBtcCrcMagic;
    if (magic != expected) {
      return XFS_ERR(XfsError::kBadMagic, "free space btree magic mismatch")
          .Ag(agno_)
          .Metadata(name_)
          .Expected(expected)
          .Actual(magic);
    }
    const uint32_t blk_level = GetBE16(data.data(), bt::kLevel);
    const uint32_t numrecs = GetBE16(data.data(), bt::kNumrecs);
    if (blk_level > bt::kMaxTraversalLevels) {
      return XFS_ERR(XfsError::kCorruptBtree, "free space btree level too deep")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(blk_level);
    }
    const uint32_t maxrecs = blk_level == 0 ? max_leaf() : max_node();
    const bool empty_root_leaf =
        numrecs == 0 && blk_level == 0 && agbno == root;
    if ((numrecs == 0 && !empty_root_leaf) || numrecs > maxrecs) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "free space btree record count out of range")
          .Ag(agno_)
          .Metadata(name_)
          .Expected(maxrecs)
          .Actual(numrecs);
    }
    if (blk_level > 0) {
      for (uint32_t i = 0; i < numrecs; ++i) {
        stack.push_back(GetBE32(data.data(), kShortHeader + max_node() * kKeySize +
                                                i * kPtrSize));
      }
    }
    blocks_.emplace(agbno, std::move(data));
  }
  if (LevelOf(root_) != level_ - 1) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "free space btree root level does not match the AGF")
        .Ag(agno_)
        .Metadata(name_)
        .Expected(level_ - 1)
        .Actual(LevelOf(root_));
  }
  uint32_t cur = root_;
  while (LevelOf(cur) > 0) cur = PtrAt(Block(cur), 0);
  if (LeftSib(Block(cur)) != kNullAgBlock) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "leftmost free space btree leaf has a left sibling")
        .Ag(agno_)
        .Metadata(name_);
  }
  return OkStatus();
}

Status AllocTree::CollectRecords(std::vector<FreeRec>* out) const {
  uint32_t cur = root_;
  while (LevelOf(cur) > 0) cur = PtrAt(Block(cur), 0);
  std::set<uint32_t> seen;
  FreeRec prev;
  bool first = true;
  while (cur != kNullAgBlock) {
    if (!seen.insert(cur).second) {
      return XFS_ERR(XfsError::kCorruptBtree, "free space btree leaf cycle")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(cur);
    }
    const uint8_t* b = Block(cur);
    if (b == nullptr) {
      return XFS_ERR(XfsError::kCorruptBtree, "free space btree leaf missing")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(cur);
    }
    const uint32_t n = NumRecs(b);
    for (uint32_t i = 0; i < n; ++i) {
      const FreeRec r = RecAt(b, i);
      if (r.len == 0 || r.start >= owner_->geo()->AgBlocksFor(agno_) ||
          r.len > owner_->geo()->AgBlocksFor(agno_) - r.start) {
        return XFS_ERR(XfsError::kCorruptBtree, "invalid free space record")
            .Ag(agno_)
            .Metadata(name_)
            .Actual(r.start);
      }
      if (!first) {
        if (CompareRec(kind_, prev, r) >= 0) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "free space records out of order")
              .Ag(agno_)
              .Metadata(name_)
              .Actual(r.start);
        }
        // Extents must be disjoint. Only the bnobt is ordered by start block,
        // so only there are adjacent records comparable this way (the cntbt is
        // ordered by length and its records cover the same extents).
        if (kind_ == TreeKind::kBno && r.start < prev.start + prev.len) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "free space records overlap")
              .Ag(agno_)
              .Metadata(name_)
              .Actual(r.start);
        }
      }
      first = false;
      prev = r;
      out->push_back(r);
    }
    cur = GetBE32(b, bt::kSRightSib);
    if (cur != kNullAgBlock && Block(cur) == nullptr) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "free space btree sibling not loaded")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(cur);
    }
  }
  return OkStatus();
}

Status AllocTree::SplitEntries(uint32_t left, uint32_t level,
                               const std::vector<Entry>& entries,
                               uint32_t* right_out, FreeRec* right_key) {
  // entries has max+1 records: the left half stays, the right half moves into
  // a newly allocated block.
  const size_t left_count = entries.size() / 2;
  const size_t right_count = entries.size() - left_count;
  uint32_t right = kNullAgBlock;
  Status s = owner_->Ag(agno_)->GetTreeBlock(&right);
  if (!s.ok()) return s;
  blocks_.emplace(right, std::vector<uint8_t>(owner_->block_size(), 0));
  s = InitBlock(right, level, static_cast<uint32_t>(right_count));
  if (!s.ok()) return s;
  std::vector<Entry> left_entries(entries.begin(),
                                  entries.begin() +
                                      static_cast<ptrdiff_t>(left_count));
  std::vector<Entry> right_entries(entries.begin() +
                                       static_cast<ptrdiff_t>(left_count),
                                   entries.end());
  WriteEntries(left, level, left_entries);
  WriteEntries(right, level, right_entries);
  // Sibling links.
  const uint32_t old_right = RightSib(Block(left));
  uint8_t* rb = MutableBlock(right);
  SetRightSib(rb, old_right);
  SetLeftSib(rb, left);
  uint8_t* lb = MutableBlock(left);
  SetRightSib(lb, right);
  if (old_right != kNullAgBlock) {
    uint8_t* orb = MutableBlock(old_right);
    if (orb != nullptr) SetLeftSib(orb, right);
  }
  *right_out = right;
  *right_key = right_entries.empty() ? FreeRec{} : right_entries[0].key;
  return OkStatus();
}

Status AllocTree::NewRoot(uint32_t left, uint32_t right) {
  uint32_t new_root = kNullAgBlock;
  Status s = owner_->Ag(agno_)->GetTreeBlock(&new_root);
  if (!s.ok()) return s;
  blocks_.emplace(new_root, std::vector<uint8_t>(owner_->block_size(), 0));
  s = InitBlock(new_root, level_, 2);
  if (!s.ok()) return s;
  uint8_t* b = blocks_[new_root].data();
  SetKeyAt(b, 0, FirstKey(left));
  SetKeyAt(b, 1, FirstKey(right));
  SetPtrAt(b, 0, left);
  SetPtrAt(b, 1, right);
  root_ = new_root;
  level_ += 1;
  return OkStatus();
}

Status AllocTree::Insert(const FreeRec& rec) {
  if (rec.len == 0) {
    return XFS_ERR(XfsError::kInternal, "inserting an empty free extent")
        .Ag(agno_)
        .Metadata(name_);
  }
  // Descend to the leaf.
  std::vector<Frame> path;
  uint32_t cur = root_;
  uint32_t lvl = level_ - 1;  // root block level
  while (lvl > 0) {
    const uint8_t* b = Block(cur);
    if (b == nullptr) {
      return XFS_ERR(XfsError::kInternal, "free space btree block lost")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(cur);
    }
    const size_t idx = ChooseChild(b, rec);
    path.push_back({cur, idx});
    cur = PtrAt(b, idx);
    lvl--;
  }
  uint8_t* leaf = MutableBlock(cur);
  if (leaf == nullptr) {
    return XFS_ERR(XfsError::kInternal, "free space btree leaf lost")
        .Ag(agno_)
        .Metadata(name_)
        .Actual(cur);
  }
  const uint32_t n = NumRecs(leaf);
  size_t pos = 0;
  while (pos < n && CompareRec(kind_, RecAt(leaf, pos), rec) < 0) ++pos;
  if (pos < n && CompareRec(kind_, RecAt(leaf, pos), rec) == 0) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "free space record already present")
        .Ag(agno_)
        .Metadata(name_)
        .Actual(rec.start);
  }

  uint32_t child_block = cur;
  FreeRec child_key = rec;
  if (n < max_leaf()) {
    // Room: shift the tail up and store the record.
    for (size_t i = n; i > pos; --i) SetRecAt(leaf, i, RecAt(leaf, i - 1));
    SetRecAt(leaf, pos, rec);
    SetNumRecs(leaf, n + 1);
    if (pos == 0) FixKeysUpward(path, cur);
    return OkStatus();
  }
  // Full leaf: distribute the max+1 records over this leaf and a new right
  // sibling, then push the new child up the path.
  std::vector<Entry> entries;
  entries.reserve(n + 1);
  for (size_t i = 0; i < pos; ++i) entries.push_back({RecAt(leaf, i), 0});
  entries.push_back({rec, 0});
  for (size_t i = pos; i < n; ++i) entries.push_back({RecAt(leaf, i), 0});
  uint32_t right = kNullAgBlock;
  FreeRec right_key;
  Status s = SplitEntries(cur, 0, entries, &right, &right_key);
  if (!s.ok()) return s;
  if (pos == 0) FixKeysUpward(path, cur);
  child_block = right;
  child_key = right_key;

  // Push the new child into the parent chain, splitting nodes as needed.
  while (true) {
    if (path.empty()) return NewRoot(root_, child_block);
    Frame frame = path.back();
    path.pop_back();
    uint8_t* parent = MutableBlock(frame.block);
    const uint32_t level = LevelOf(frame.block);
    const uint32_t pn = NumRecs(parent);
    const size_t insert_pos = frame.index + 1;
    if (pn < max_node()) {
      for (size_t i = pn; i > insert_pos; --i) {
        SetKeyAt(parent, i, KeyAt(parent, i - 1));
        SetPtrAt(parent, i, PtrAt(parent, i - 1));
      }
      SetKeyAt(parent, insert_pos, child_key);
      SetPtrAt(parent, insert_pos, child_block);
      SetNumRecs(parent, pn + 1);
      return OkStatus();
    }
    std::vector<Entry> parent_entries;
    parent_entries.reserve(pn + 1);
    for (size_t i = 0; i < insert_pos; ++i) {
      parent_entries.push_back({KeyAt(parent, i), PtrAt(parent, i)});
    }
    parent_entries.push_back({child_key, child_block});
    for (size_t i = insert_pos; i < pn; ++i) {
      parent_entries.push_back({KeyAt(parent, i), PtrAt(parent, i)});
    }
    uint32_t parent_right = kNullAgBlock;
    FreeRec parent_right_key;
    const Status split_status =
        SplitEntries(frame.block, level, parent_entries, &parent_right,
                     &parent_right_key);
    if (!split_status.ok()) return split_status;
    child_block = parent_right;
    child_key = parent_right_key;
  }
}

Status AllocTree::Remove(const FreeRec& rec) {
  std::vector<Frame> path;
  uint32_t cur = root_;
  uint32_t lvl = level_ - 1;  // root block level
  while (lvl > 0) {
    const uint8_t* b = Block(cur);
    if (b == nullptr) {
      return XFS_ERR(XfsError::kInternal, "free space btree block lost")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(cur);
    }
    const size_t idx = ChooseChild(b, rec);
    path.push_back({cur, idx});
    cur = PtrAt(b, idx);
    lvl--;
  }
  uint8_t* leaf = MutableBlock(cur);
  if (leaf == nullptr) {
    return XFS_ERR(XfsError::kInternal, "free space btree leaf lost")
        .Ag(agno_)
        .Metadata(name_)
        .Actual(cur);
  }
  const uint32_t n = NumRecs(leaf);
  size_t pos = n;
  for (size_t i = 0; i < n; ++i) {
    if (CompareRec(kind_, RecAt(leaf, i), rec) == 0) {
      pos = i;
      break;
    }
  }
  if (pos == n) {
    return XFS_ERR(XfsError::kCorruptBtree, "free space record not found")
        .Ag(agno_)
        .Metadata(name_)
        .Actual(rec.start);
  }
  for (size_t i = pos; i + 1 < n; ++i) SetRecAt(leaf, i, RecAt(leaf, i + 1));
  SetNumRecs(leaf, n - 1);
  if (pos == 0 && n - 1 > 0) FixKeysUpward(path, cur);

  // Rebalance: every non-root block keeps at least half of its maximum.
  while (cur != root_) {
    const uint32_t clvl = LevelOf(cur);
    const uint32_t n_cur = NumRecs(cur);
    const uint32_t minr = (clvl == 0 ? max_leaf() : max_node()) / 2;
    if (n_cur >= minr) break;
    Frame frame = path.back();
    uint8_t* parent = MutableBlock(frame.block);
    const size_t idx = frame.index;
    const uint32_t pn = NumRecs(parent);
    const uint32_t left = idx > 0 ? PtrAt(parent, idx - 1) : kNullAgBlock;
    const uint32_t right =
        idx + 1 < pn ? PtrAt(parent, idx + 1) : kNullAgBlock;
    const uint32_t maxr = (clvl == 0 ? max_leaf() : max_node());
    bool merged = false;

    if (left == kNullAgBlock && right == kNullAgBlock && path.size() == 1 &&
        pn == 1) {
      // The only child of the root: nothing to merge with. This is legal (the
      // root may keep a single child).
      break;
    }
    if (left != kNullAgBlock && NumRecs(Block(left)) + n_cur <= maxr) {
      std::vector<Entry> children = ReadEntries(cur);
      uint8_t* lb = MutableBlock(left);
      const uint32_t ln = NumRecs(lb);
      SetNumRecs(lb, ln + static_cast<uint32_t>(children.size()));
      for (size_t i = 0; i < children.size(); ++i) {
        if (clvl == 0) {
          SetRecAt(lb, ln + i, children[i].key);
        } else {
          SetKeyAt(lb, ln + i, children[i].key);
          SetPtrAt(lb, ln + i, children[i].ptr);
        }
      }
      const uint32_t rr = RightSib(Block(cur));
      SetRightSib(lb, rr);
      if (rr != kNullAgBlock) {
        uint8_t* rb = MutableBlock(rr);
        if (rb != nullptr) SetLeftSib(rb, left);
      }
      Status s = owner_->Ag(agno_)->PutTreeBlock(cur);
      if (!s.ok()) return s;
      blocks_.erase(cur);
      dirty_.erase(cur);
      // Remove the parent's entry for the erased block.
      const uint32_t pk = NumRecs(parent);
      for (size_t i = idx; i + 1 < pk; ++i) {
        SetKeyAt(parent, i, KeyAt(parent, i + 1));
        SetPtrAt(parent, i, PtrAt(parent, i + 1));
      }
      SetNumRecs(parent, pk - 1);
      merged = true;
    } else if (right != kNullAgBlock &&
               NumRecs(Block(right)) + n_cur <= maxr) {
      std::vector<Entry> children = ReadEntries(right);
      uint8_t* cb = MutableBlock(cur);
      const uint32_t cn = NumRecs(cb);
      SetNumRecs(cb, cn + static_cast<uint32_t>(children.size()));
      for (size_t i = 0; i < children.size(); ++i) {
        if (clvl == 0) {
          SetRecAt(cb, cn + i, children[i].key);
        } else {
          SetKeyAt(cb, cn + i, children[i].key);
          SetPtrAt(cb, cn + i, children[i].ptr);
        }
      }
      const uint32_t rr = RightSib(Block(right));
      SetRightSib(cb, rr);
      if (rr != kNullAgBlock) {
        uint8_t* rb = MutableBlock(rr);
        if (rb != nullptr) SetLeftSib(rb, cur);
      }
      Status s = owner_->Ag(agno_)->PutTreeBlock(right);
      if (!s.ok()) return s;
      blocks_.erase(right);
      dirty_.erase(right);
      const uint32_t pk = NumRecs(parent);
      for (size_t i = idx + 1; i + 1 < pk; ++i) {
        SetKeyAt(parent, i, KeyAt(parent, i + 1));
        SetPtrAt(parent, i, PtrAt(parent, i + 1));
      }
      SetNumRecs(parent, pk - 1);
      merged = true;
    } else if (left != kNullAgBlock && NumRecs(Block(left)) > minr) {
      // Borrow the last entry of the left sibling.
      uint8_t* lb = MutableBlock(left);
      uint8_t* cb = MutableBlock(cur);
      const uint32_t ln = NumRecs(lb);
      if (clvl == 0) {
        const FreeRec moved = RecAt(lb, ln - 1);
        SetNumRecs(lb, ln - 1);
        for (size_t i = NumRecs(cb); i > 0; --i) SetRecAt(cb, i, RecAt(cb, i - 1));
        SetRecAt(cb, 0, moved);
        SetNumRecs(cb, NumRecs(cb) + 1);
      } else {
        const FreeRec moved_key = KeyAt(lb, ln - 1);
        const uint32_t moved_ptr = PtrAt(lb, ln - 1);
        SetNumRecs(lb, ln - 1);
        for (size_t i = NumRecs(cb); i > 0; --i) {
          SetKeyAt(cb, i, KeyAt(cb, i - 1));
          SetPtrAt(cb, i, PtrAt(cb, i - 1));
        }
        SetKeyAt(cb, 0, moved_key);
        SetPtrAt(cb, 0, moved_ptr);
        SetNumRecs(cb, NumRecs(cb) + 1);
      }
      SetKeyAt(parent, idx, KeyAt(cb, 0));
      if (idx == 0) {
        std::vector<Frame> ppath(path.begin(), path.end() - 1);
        FixKeysUpward(ppath, frame.block);
      }
      break;
    } else if (right != kNullAgBlock && NumRecs(Block(right)) > minr) {
      uint8_t* rb = MutableBlock(right);
      uint8_t* cb = MutableBlock(cur);
      const uint32_t cn = NumRecs(cb);
      if (clvl == 0) {
        const FreeRec moved = RecAt(rb, 0);
        for (size_t i = 0; i + 1 < NumRecs(rb); ++i) {
          SetRecAt(rb, i, RecAt(rb, i + 1));
        }
        SetNumRecs(rb, NumRecs(rb) - 1);
        SetRecAt(cb, cn, moved);
        SetNumRecs(cb, cn + 1);
      } else {
        const FreeRec moved_key = KeyAt(rb, 0);
        const uint32_t moved_ptr = PtrAt(rb, 0);
        for (size_t i = 0; i + 1 < NumRecs(rb); ++i) {
          SetKeyAt(rb, i, KeyAt(rb, i + 1));
          SetPtrAt(rb, i, PtrAt(rb, i + 1));
        }
        SetNumRecs(rb, NumRecs(rb) - 1);
        SetKeyAt(cb, cn, moved_key);
        SetPtrAt(cb, cn, moved_ptr);
        SetNumRecs(cb, cn + 1);
      }
      SetKeyAt(parent, idx + 1, KeyAt(rb, 0));
      break;
    } else {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "cannot rebalance a free space btree block")
          .Ag(agno_)
          .Metadata(name_)
          .Actual(cur);
    }

    if (merged) {
      path.pop_back();
      cur = frame.block;
    }
  }

  // A root with a single leaf child collapses into the root block.
  if (level_ > 1 && NumRecs(root_) == 1) {
    uint8_t* root_block = MutableBlock(root_);
    const uint32_t child = PtrAt(root_block, 0);
    if (LevelOf(child) != 0) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "cannot collapse a multi-level free space btree")
          .Ag(agno_)
          .Metadata(name_);
    }
    const std::vector<Entry> children = ReadEntries(child);
    PutBE32(root_block, bt::kMagic,
            kind_ == TreeKind::kBno ? ondisk::kBtbCrcMagic
                                    : ondisk::kBtcCrcMagic);
    PutBE16(root_block, bt::kLevel, 0);
    PutBE32(root_block, bt::kSLeftsib, kNullAgBlock);
    PutBE32(root_block, bt::kSRightSib, kNullAgBlock);
    SetNumRecs(root_block, static_cast<uint32_t>(children.size()));
    for (size_t i = 0; i < children.size(); ++i) {
      SetRecAt(root_block, i, children[i].key);
    }
    Status s = owner_->Ag(agno_)->PutTreeBlock(child);
    if (!s.ok()) return s;
    blocks_.erase(child);
    dirty_.erase(child);
    level_ = 1;
  }
  return OkStatus();
}

}  // namespace alloc_detail
}  // namespace xfs
