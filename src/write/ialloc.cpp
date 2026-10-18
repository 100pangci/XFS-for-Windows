// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - inode allocator (RW-3).
//
// On-disk rules implemented here (verified against fs/xfs/libxfs and
// xfsprogs; see docs/INODE_ALLOCATOR.md):
//
//   - the inobt holds one record per 64-inode chunk: startino, freecount and
//     the 64-bit free mask (no sparse inodes: holemask/count do not exist and
//     the record is interpreted as startino / freecount be32 / free be64).
//   - allocating an inode clears its free bit and decrements freecount; the
//     AGI freecount and sb_ifree follow.
//   - a chunk that becomes completely free is removed from the inobt and its
//     blocks are freed, because this geometry stores at most 64 inodes per
//     block (xfs_difree_inobt). agi_count drops by 64 and agi_freecount by 63.
//   - a new chunk is a contiguous run of ialloc_blks blocks (64 inodes),
//     aligned like the kernel aligns chunks, inserted as a full record with
//     all inodes free, with agi_newino pointing at it.
//   - the new chunk's inode cluster is initialised by an XFS_LI_ICREATE log
//     item, exactly the kernel's mechanism: replay rebuilds the cluster from
//     the recorded geometry (magic, version, generation, di_ino, uuid, CRC).
#include "xfs/ialloc.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <vector>

#include "xfs/ag.h"
#include "xfs/btree.h"
#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/log_format.h"
#include "xfs/ondisk.h"

namespace xfs {
namespace {

namespace agi = ondisk::agi;
namespace bt = ondisk::btree;
namespace di = ondisk::inode;
namespace lf = logfmt;
namespace sb = ondisk::sb;

constexpr uint32_t kNullAgino = 0xffffffffu;
constexpr uint32_t kNullAgBlock = 0xffffffffu;
constexpr size_t kShortHeader = bt::kSHeaderLenV5;   // 56 bytes
constexpr size_t kKeySize = bt::kInobtKeySize;       // 4 bytes
constexpr size_t kPtrSize = bt::kInobtPtrSize;       // 4 bytes
constexpr size_t kRecSize = bt::kInobtRecSize;       // 16 bytes
constexpr size_t kMaxTreeBlocks = 4096;

uint64_t PopCount64(uint64_t v) {
  uint64_t n = 0;
  while (v) {
    v &= v - 1;
    ++n;
  }
  return n;
}

// A process-wide generation source, like the kernel's get_random_u32() for a
// new chunk. Random rather than sequential so a freed-then-reused inode
// number cannot be confused with its previous incarnation.
uint32_t NextInodeGeneration() {
  static std::mt19937 rng(std::random_device{}());
  return rng();
}

class InobtTree;

// One AG's inode allocator state: the AGI buffer plus the in-memory inobt.
struct AgiState {
  uint32_t agno = 0;
  std::vector<uint8_t> agi_buf;  // raw sector image
  bool dirty = false;

  uint32_t count = 0;
  uint32_t freecount = 0;
  uint32_t newino = kNullAgino;
  uint32_t orig_count = 0;
  uint32_t orig_freecount = 0;

  std::unique_ptr<InobtTree> inobt;
};

// ---------------------------------------------------------------------------
// The inobt btree of one AG, editable in memory.
// ---------------------------------------------------------------------------
class InobtTree {
 public:
  InobtTree(BlockDevice* device, BlockAllocator* alloc,
            const XfsGeometry* geo, uint32_t agno)
      : device_(device), alloc_(alloc), geo_(geo), agno_(agno) {}

  Status Load(uint32_t root, uint32_t level);
  // Iterates the records in key order.
  Status Collect(std::vector<InobtRecord>* out) const;
  // Exact lookup by startino.
  Result<InobtRecord> Find(uint32_t startino) const;
  // The record with the largest startino <= `startino`.
  Result<InobtRecord> FindLE(uint32_t startino) const;
  // Inserts a new record (startino must not exist yet). Handles leaf/interior
  // splits and root growth.
  Status Insert(const InobtRecord& rec);
  // Replaces the record with the same startino.
  Status Update(const InobtRecord& rec);
  // Removes a record (leaf merge/borrow and root collapse included).
  Status Remove(uint32_t startino);

  uint32_t root() const { return root_; }
  uint32_t level() const { return level_; }
  const std::set<uint32_t>& dirty_blocks() const { return dirty_; }
  const std::map<uint32_t, std::vector<uint8_t>>& blocks() const {
    return blocks_;
  }

 private:
  struct Frame {
    uint32_t block = 0;
    size_t index = 0;
  };
  struct Entry {
    uint32_t key = 0;   // startino
    uint32_t ptr = kNullAgBlock;
    InobtRecord rec;    // valid at level 0
  };

  uint32_t max_leaf() const {
    return static_cast<uint32_t>((geo_->block_size - kShortHeader) / kRecSize);
  }
  uint32_t max_node() const {
    return static_cast<uint32_t>((geo_->block_size - kShortHeader) /
                                 (kKeySize + kPtrSize));
  }
  uint8_t* MutableBlock(uint32_t agbno) {
    auto it = blocks_.find(agbno);
    if (it == blocks_.end()) return nullptr;
    dirty_.insert(agbno);
    return it->second.data();
  }
  const uint8_t* Block(uint32_t agbno) const {
    auto it = blocks_.find(agbno);
    return it == blocks_.end() ? nullptr : it->second.data();
  }
  uint32_t LevelOf(uint32_t agbno) const {
    return GetBE16(Block(agbno), bt::kLevel);
  }
  uint32_t NumRecs(uint32_t agbno) const {
    return GetBE16(Block(agbno), bt::kNumrecs);
  }
  uint32_t NumRecs(const uint8_t* b) const { return GetBE16(b, bt::kNumrecs); }
  void SetNumRecs(uint8_t* b, uint32_t n) {
    PutBE16(b, bt::kNumrecs, static_cast<uint16_t>(n));
  }
  uint32_t KeyAt(const uint8_t* b, size_t i) const {
    return GetBE32(b, kShortHeader + i * kKeySize);
  }
  void SetKeyAt(uint8_t* b, size_t i, uint32_t v) {
    PutBE32(b, kShortHeader + i * kKeySize, v);
  }
  uint32_t PtrAt(const uint8_t* b, size_t i) const {
    return GetBE32(b, kShortHeader + max_node() * kKeySize + i * kPtrSize);
  }
  void SetPtrAt(uint8_t* b, size_t i, uint32_t v) {
    PutBE32(b, kShortHeader + max_node() * kKeySize + i * kPtrSize, v);
  }
  InobtRecord RecAt(const uint8_t* b, size_t i) const {
    InobtRecord r;
    r.startino = GetBE32(b, kShortHeader + i * kRecSize + bt::kInobtRecStartino);
    r.freecount =
        GetBE32(b, kShortHeader + i * kRecSize + bt::kInobtRecFreecount);
    r.free = GetBE64(b, kShortHeader + i * kRecSize + bt::kInobtRecFree);
    return r;
  }
  void SetRecAt(uint8_t* b, size_t i, const InobtRecord& r) {
    PutBE32(b, kShortHeader + i * kRecSize + bt::kInobtRecStartino, r.startino);
    PutBE32(b, kShortHeader + i * kRecSize + bt::kInobtRecFreecount,
            r.freecount);
    PutBE64(b, kShortHeader + i * kRecSize + bt::kInobtRecFree, r.free);
  }
  uint32_t LeftSib(const uint8_t* b) const { return GetBE32(b, bt::kSLeftsib); }
  uint32_t RightSib(const uint8_t* b) const {
    return GetBE32(b, bt::kSRightSib);
  }
  void SetLeftSib(uint8_t* b, uint32_t v) { PutBE32(b, bt::kSLeftsib, v); }
  void SetRightSib(uint8_t* b, uint32_t v) { PutBE32(b, bt::kSRightSib, v); }

  uint32_t FirstKey(uint32_t agbno) const {
    const uint8_t* b = Block(agbno);
    return LevelOf(agbno) == 0 ? RecAt(b, 0).startino : KeyAt(b, 0);
  }
  size_t ChooseChild(const uint8_t* b, uint32_t key) const;
  void FixKeysUpward(const std::vector<Frame>& path, uint32_t child);
  std::vector<Entry> ReadEntries(uint32_t agbno) const;
  void WriteEntries(uint32_t agbno, uint32_t level,
                    const std::vector<Entry>& entries);
  Status InitBlock(uint32_t agbno, uint32_t level, uint32_t numrecs);
  Status AllocTreeBlock(uint32_t* agbno);
  Status FreeTreeBlock(uint32_t agbno);
  Status SplitEntries(uint32_t left, uint32_t level,
                      const std::vector<Entry>& entries, uint32_t* right_out,
                      uint32_t* right_key);
  Status NewRoot(uint32_t left, uint32_t right);
  Status Descend(uint32_t key, std::vector<Frame>* path, uint32_t* leaf) const;

  BlockDevice* device_;
  BlockAllocator* alloc_;
  const XfsGeometry* geo_;
  uint32_t agno_ = 0;
  uint32_t root_ = 0;
  uint32_t level_ = 1;
  std::map<uint32_t, std::vector<uint8_t>> blocks_;
  std::set<uint32_t> dirty_;
};

Status InobtTree::InitBlock(uint32_t agbno, uint32_t level, uint32_t numrecs) {
  auto it = blocks_.find(agbno);
  if (it == blocks_.end()) {
    return XFS_ERR(XfsError::kInternal, "initializing an unloaded inobt block")
        .Ag(agno_)
        .Metadata("inobt")
        .Actual(agbno);
  }
  std::vector<uint8_t>& data = it->second;
  std::memset(data.data(), 0, data.size());
  PutBE32(data.data(), bt::kMagic, ondisk::kIbtCrcMagic);
  PutBE16(data.data(), bt::kLevel, static_cast<uint16_t>(level));
  PutBE16(data.data(), bt::kNumrecs, static_cast<uint16_t>(numrecs));
  PutBE32(data.data(), bt::kSLeftsib, kNullAgBlock);
  PutBE32(data.data(), bt::kSRightSib, kNullAgBlock);
  PutBE64(data.data(), bt::kSBlkno,
          geo_->FsbToByte(geo_->AgFsb(agno_, agbno)) / 512);
  PutBE64(data.data(), bt::kSLsn, 0);  // stamped at commit
  std::memcpy(data.data() + bt::kSUuid, geo_->meta_uuid.data(), 16);
  PutBE32(data.data(), bt::kSOwner, agno_);
  StoreLE32(data.data() + bt::kSCrc, 0);  // stamped at commit
  dirty_.insert(agbno);
  return OkStatus();
}

Status InobtTree::AllocTreeBlock(uint32_t* agbno) {
  Allocation a;
  // Inobt blocks are regular blocks of this AG (xfs_btree_alloc_block); they
  // come from the block allocator, not from the AGFL.
  Status s = alloc_->AllocateFromAg(agno_, 1, 1, &a);
  if (!s.ok()) return s;
  const uint32_t agb = static_cast<uint32_t>(a.fsb & ((1ULL << geo_->agblklog) - 1));
  blocks_.emplace(agb, std::vector<uint8_t>(geo_->block_size, 0));
  *agbno = agb;
  return OkStatus();
}

Status InobtTree::FreeTreeBlock(uint32_t agbno) {
  const uint64_t fsb = geo_->AgFsb(agno_, agbno);
  Status s = alloc_->Free(fsb, 1);
  if (!s.ok()) return s;
  blocks_.erase(agbno);
  dirty_.erase(agbno);
  return OkStatus();
}

Status InobtTree::Load(uint32_t root, uint32_t level) {
  root_ = root;
  level_ = level;
  if (level < 1 || level > bt::kMaxTraversalLevels) {
    return XFS_ERR(XfsError::kCorruptBtree, "inobt level out of range")
        .Ag(agno_)
        .Metadata("inobt")
        .Actual(level);
  }
  std::vector<uint32_t> stack{root};
  std::set<uint32_t> seen;
  while (!stack.empty()) {
    const uint32_t agbno = stack.back();
    stack.pop_back();
    if (agbno == kNullAgBlock || !geo_->AgbnoValid(agno_, agbno)) {
      return XFS_ERR(XfsError::kCorruptBtree, "inobt pointer out of range")
          .Ag(agno_)
          .Metadata("inobt")
          .Actual(agbno);
    }
    if (!seen.insert(agbno).second) {
      return XFS_ERR(XfsError::kCorruptBtree, "inobt cycle")
          .Ag(agno_)
          .Metadata("inobt")
          .Actual(agbno);
    }
    if (seen.size() > kMaxTreeBlocks) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "inobt has more blocks than this build can edit")
          .Ag(agno_)
          .Metadata("inobt")
          .Actual(seen.size());
    }
    std::vector<uint8_t> data(geo_->block_size, 0);
    const uint64_t offset = geo_->FsbToByte(geo_->AgFsb(agno_, agbno));
    if (!device_->Read(offset, data.data(), data.size())) {
      return XFS_ERR(XfsError::kIoError, "failed to read inobt block")
          .Ag(agno_)
          .Offset(offset)
          .Metadata("inobt");
    }
    const uint32_t magic = GetBE32(data.data(), bt::kMagic);
    if (magic != ondisk::kIbtCrcMagic) {
      return XFS_ERR(XfsError::kBadMagic, "inobt magic mismatch")
          .Ag(agno_)
          .Metadata("inobt")
          .Expected(ondisk::kIbtCrcMagic)
          .Actual(magic);
    }
    const uint32_t blk_level = GetBE16(data.data(), bt::kLevel);
    const uint32_t numrecs = GetBE16(data.data(), bt::kNumrecs);
    if (blk_level > bt::kMaxTraversalLevels) {
      return XFS_ERR(XfsError::kCorruptBtree, "inobt level too deep")
          .Ag(agno_)
          .Metadata("inobt")
          .Actual(blk_level);
    }
    const uint32_t maxrecs = blk_level == 0 ? max_leaf() : max_node();
    const bool empty_root_leaf =
        numrecs == 0 && blk_level == 0 && agbno == root;
    if ((numrecs == 0 && !empty_root_leaf) || numrecs > maxrecs) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "inobt record count out of range")
          .Ag(agno_)
          .Metadata("inobt")
          .Expected(maxrecs)
          .Actual(numrecs);
    }
    if (blk_level > 0) {
      for (uint32_t i = 0; i < numrecs; ++i) {
        stack.push_back(PtrAt(data.data(), i));
      }
    }
    blocks_.emplace(agbno, std::move(data));
  }
  if (LevelOf(root_) != level_ - 1) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "inobt root level does not match the AGI")
        .Ag(agno_)
        .Metadata("inobt")
        .Expected(level_ - 1)
        .Actual(LevelOf(root_));
  }
  uint32_t cur = root_;
  while (LevelOf(cur) > 0) cur = PtrAt(Block(cur), 0);
  if (LeftSib(Block(cur)) != kNullAgBlock) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "leftmost inobt leaf has a left sibling")
        .Ag(agno_)
        .Metadata("inobt");
  }
  return OkStatus();
}

Status InobtTree::Collect(std::vector<InobtRecord>* out) const {
  uint32_t cur = root_;
  while (LevelOf(cur) > 0) cur = PtrAt(Block(cur), 0);
  std::set<uint32_t> seen;
  while (cur != kNullAgBlock) {
    if (!seen.insert(cur).second) {
      return XFS_ERR(XfsError::kCorruptBtree, "inobt leaf cycle")
          .Ag(agno_)
          .Metadata("inobt")
          .Actual(cur);
    }
    const uint8_t* b = Block(cur);
    if (b == nullptr) {
      return XFS_ERR(XfsError::kCorruptBtree, "inobt leaf missing")
          .Ag(agno_)
          .Metadata("inobt")
          .Actual(cur);
    }
    const uint32_t n = NumRecs(b);
    for (uint32_t i = 0; i < n; ++i) {
      const InobtRecord r = RecAt(b, i);
      if (r.startino + 64 > geo_->AgBlocksFor(agno_) * geo_->inopblock) {
        return XFS_ERR(XfsError::kCorruptBtree, "inobt record out of range")
            .Ag(agno_)
            .Metadata("inobt")
            .Actual(r.startino);
      }
      if (PopCount64(r.free) != r.freecount) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "inobt freecount does not match the free mask")
            .Ag(agno_)
            .Metadata("inobt")
            .Actual(r.startino);
      }
      if (!out->empty() && out->back().startino >= r.startino) {
        return XFS_ERR(XfsError::kCorruptBtree, "inobt records out of order")
            .Ag(agno_)
            .Metadata("inobt")
            .Actual(r.startino);
      }
      out->push_back(r);
    }
    cur = GetBE32(b, bt::kSRightSib);
    if (cur != kNullAgBlock && Block(cur) == nullptr) {
      return XFS_ERR(XfsError::kCorruptBtree, "inobt sibling not loaded")
          .Ag(agno_)
          .Metadata("inobt")
          .Actual(cur);
    }
  }
  return OkStatus();
}

Result<InobtRecord> InobtTree::Find(uint32_t startino) const {
  uint32_t cur = root_;
  while (LevelOf(cur) > 0) {
    const uint8_t* b = Block(cur);
    const uint32_t n = NumRecs(b);
    size_t chosen = 0;
    for (size_t i = 0; i < n; ++i) {
      if (KeyAt(b, i) <= startino) {
        chosen = i;
      } else {
        break;
      }
    }
    cur = PtrAt(b, chosen);
    if (cur == kNullAgBlock || Block(cur) == nullptr) {
      return XFS_ERR(XfsError::kCorruptBtree, "inobt pointer not loaded")
          .Ag(agno_)
          .Metadata("inobt")
          .Actual(cur);
    }
  }
  const uint8_t* b = Block(cur);
  const uint32_t n = NumRecs(b);
  for (uint32_t i = 0; i < n; ++i) {
    const InobtRecord r = RecAt(b, i);
    if (r.startino == startino) return r;
    if (r.startino > startino) break;
  }
  return XFS_ERR(XfsError::kNotFound, "inobt record not found")
      .Ag(agno_)
      .Metadata("inobt")
      .Actual(startino);
}

Status InobtTree::Descend(uint32_t key, std::vector<Frame>* path,
                          uint32_t* leaf) const {
  uint32_t cur = root_;
  uint32_t lvl = level_ - 1;
  while (lvl > 0) {
    const uint8_t* b = Block(cur);
    if (b == nullptr) {
      return XFS_ERR(XfsError::kInternal, "inobt block lost")
          .Ag(agno_)
          .Metadata("inobt")
          .Actual(cur);
    }
    const size_t idx = ChooseChild(b, key);
    path->push_back({cur, idx});
    cur = PtrAt(b, idx);
    lvl--;
  }
  *leaf = cur;
  return OkStatus();
}

size_t InobtTree::ChooseChild(const uint8_t* b, uint32_t key) const {
  const uint32_t n = NumRecs(b);
  size_t chosen = 0;
  for (size_t i = 0; i < n; ++i) {
    if (KeyAt(b, i) <= key) {
      chosen = i;
    } else {
      break;
    }
  }
  return chosen;
}

void InobtTree::FixKeysUpward(const std::vector<Frame>& path, uint32_t child) {
  uint32_t cur = child;
  uint32_t key = FirstKey(cur);
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

std::vector<InobtTree::Entry> InobtTree::ReadEntries(uint32_t agbno) const {
  const uint8_t* b = Block(agbno);
  const uint32_t n = NumRecs(b);
  const uint32_t level = GetBE16(b, bt::kLevel);
  std::vector<Entry> out;
  out.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    Entry entry;
    if (level == 0) {
      entry.rec = RecAt(b, i);
      entry.key = entry.rec.startino;
    } else {
      entry.key = KeyAt(b, i);
      entry.ptr = PtrAt(b, i);
    }
    out.push_back(entry);
  }
  return out;
}

void InobtTree::WriteEntries(uint32_t agbno, uint32_t level,
                             const std::vector<Entry>& entries) {
  uint8_t* b = MutableBlock(agbno);
  SetNumRecs(b, static_cast<uint32_t>(entries.size()));
  for (size_t i = 0; i < entries.size(); ++i) {
    if (level == 0) {
      SetRecAt(b, i, entries[i].rec);
    } else {
      SetKeyAt(b, i, entries[i].key);
      SetPtrAt(b, i, entries[i].ptr);
    }
  }
}

Status InobtTree::SplitEntries(uint32_t left, uint32_t level,
                               const std::vector<Entry>& entries,
                               uint32_t* right_out, uint32_t* right_key) {
  const size_t left_count = entries.size() / 2;
  const size_t right_count = entries.size() - left_count;
  uint32_t right = kNullAgBlock;
  Status s = AllocTreeBlock(&right);
  if (!s.ok()) return s;
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
  const Entry& first = entries[left_count];
  *right_key = level == 0 ? first.rec.startino : first.key;
  return OkStatus();
}

Status InobtTree::NewRoot(uint32_t left, uint32_t right) {
  uint32_t new_root = kNullAgBlock;
  Status s = AllocTreeBlock(&new_root);
  if (!s.ok()) return s;
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

Status InobtTree::Insert(const InobtRecord& rec) {
  std::vector<Frame> path;
  uint32_t cur = kNullAgBlock;
  Status s = Descend(rec.startino, &path, &cur);
  if (!s.ok()) return s;
  uint8_t* leaf = MutableBlock(cur);
  if (leaf == nullptr) {
    return XFS_ERR(XfsError::kInternal, "inobt leaf lost")
        .Ag(agno_)
        .Metadata("inobt")
        .Actual(cur);
  }
  const uint32_t n = NumRecs(leaf);
  size_t pos = 0;
  while (pos < n && RecAt(leaf, pos).startino < rec.startino) ++pos;
  if (pos < n && RecAt(leaf, pos).startino == rec.startino) {
    return XFS_ERR(XfsError::kAlreadyExists, "inobt record already present")
        .Ag(agno_)
        .Metadata("inobt")
        .Actual(rec.startino);
  }
  uint32_t child_block = cur;
  uint32_t child_key = rec.startino;
  if (n < max_leaf()) {
    for (size_t i = n; i > pos; --i) SetRecAt(leaf, i, RecAt(leaf, i - 1));
    SetRecAt(leaf, pos, rec);
    SetNumRecs(leaf, n + 1);
    if (pos == 0) FixKeysUpward(path, cur);
    return OkStatus();
  }
  std::vector<Entry> entries;
  entries.reserve(n + 1);
  for (size_t i = 0; i < pos; ++i) entries.push_back({0, 0, RecAt(leaf, i)});
  entries.push_back({0, 0, rec});
  for (size_t i = pos; i < n; ++i) entries.push_back({0, 0, RecAt(leaf, i)});
  uint32_t right = kNullAgBlock;
  uint32_t right_key = 0;
  s = SplitEntries(cur, 0, entries, &right, &right_key);
  if (!s.ok()) return s;
  if (pos == 0) FixKeysUpward(path, cur);
  child_block = right;
  child_key = right_key;

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
    std::vector<Entry> entries2;
    entries2.reserve(pn + 1);
    for (size_t i = 0; i < insert_pos; ++i) {
      entries2.push_back({KeyAt(parent, i), PtrAt(parent, i), {}});
    }
    entries2.push_back({child_key, child_block, {}});
    for (size_t i = insert_pos; i < pn; ++i) {
      entries2.push_back({KeyAt(parent, i), PtrAt(parent, i), {}});
    }
    uint32_t right2 = kNullAgBlock;
    uint32_t right_key2 = 0;
    s = SplitEntries(frame.block, level, entries2, &right2, &right_key2);
    if (!s.ok()) return s;
    child_block = right2;
    child_key = right_key2;
  }
}

Status InobtTree::Update(const InobtRecord& rec) {
  std::vector<Frame> path;
  uint32_t cur = kNullAgBlock;
  Status s = Descend(rec.startino, &path, &cur);
  if (!s.ok()) return s;
  uint8_t* leaf = MutableBlock(cur);
  if (leaf == nullptr) {
    return XFS_ERR(XfsError::kInternal, "inobt leaf lost")
        .Ag(agno_)
        .Metadata("inobt")
        .Actual(cur);
  }
  const uint32_t n = NumRecs(leaf);
  for (uint32_t i = 0; i < n; ++i) {
    if (RecAt(leaf, i).startino == rec.startino) {
      SetRecAt(leaf, i, rec);
      return OkStatus();
    }
  }
  return XFS_ERR(XfsError::kNotFound, "inobt record not found for update")
      .Ag(agno_)
      .Metadata("inobt")
      .Actual(rec.startino);
}

Result<InobtRecord> InobtTree::FindLE(uint32_t startino) const {
  uint32_t cur = root_;
  while (LevelOf(cur) > 0) {
    cur = PtrAt(Block(cur), ChooseChild(Block(cur), startino));
    if (cur == kNullAgBlock || Block(cur) == nullptr) {
      return XFS_ERR(XfsError::kCorruptBtree, "inobt pointer not loaded")
          .Ag(agno_)
          .Metadata("inobt")
          .Actual(cur);
    }
  }
  const uint8_t* b = Block(cur);
  const uint32_t n = NumRecs(b);
  for (uint32_t i = n; i > 0; --i) {
    const InobtRecord r = RecAt(b, i - 1);
    if (r.startino <= startino) return r;
  }
  return XFS_ERR(XfsError::kNotFound, "no inobt record at or below the inode")
      .Ag(agno_)
      .Metadata("inobt")
      .Actual(startino);
}

Status InobtTree::Remove(uint32_t startino) {
  std::vector<Frame> path;
  uint32_t cur = kNullAgBlock;
  Status s = Descend(startino, &path, &cur);
  if (!s.ok()) return s;
  uint8_t* leaf = MutableBlock(cur);
  if (leaf == nullptr) {
    return XFS_ERR(XfsError::kInternal, "inobt leaf lost")
        .Ag(agno_)
        .Metadata("inobt")
        .Actual(cur);
  }
  const uint32_t n = NumRecs(leaf);
  size_t pos = n;
  for (size_t i = 0; i < n; ++i) {
    if (RecAt(leaf, i).startino == startino) {
      pos = i;
      break;
    }
  }
  if (pos == n) {
    return XFS_ERR(XfsError::kNotFound, "inobt record not found for removal")
        .Ag(agno_)
        .Metadata("inobt")
        .Actual(startino);
  }
  for (size_t i = pos; i + 1 < n; ++i) SetRecAt(leaf, i, RecAt(leaf, i + 1));
  SetNumRecs(leaf, n - 1);
  if (pos == 0 && n - 1 > 0) FixKeysUpward(path, cur);

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
      break;  // the only child of the root: legal, nothing to merge with
    }
    if (left != kNullAgBlock && NumRecs(Block(left)) + n_cur <= maxr) {
      std::vector<Entry> children = ReadEntries(cur);
      uint8_t* lb = MutableBlock(left);
      const uint32_t ln = NumRecs(lb);
      SetNumRecs(lb, ln + static_cast<uint32_t>(children.size()));
      for (size_t i = 0; i < children.size(); ++i) {
        if (clvl == 0) {
          SetRecAt(lb, ln + i, children[i].rec);
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
      Status sr = FreeTreeBlock(cur);
      if (!sr.ok()) return sr;
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
          SetRecAt(cb, cn + i, children[i].rec);
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
      Status sr = FreeTreeBlock(right);
      if (!sr.ok()) return sr;
      const uint32_t pk = NumRecs(parent);
      for (size_t i = idx + 1; i + 1 < pk; ++i) {
        SetKeyAt(parent, i, KeyAt(parent, i + 1));
        SetPtrAt(parent, i, PtrAt(parent, i + 1));
      }
      SetNumRecs(parent, pk - 1);
      merged = true;
    } else if (left != kNullAgBlock && NumRecs(Block(left)) > minr) {
      uint8_t* lb = MutableBlock(left);
      uint8_t* cb = MutableBlock(cur);
      const uint32_t ln = NumRecs(lb);
      if (clvl == 0) {
        const InobtRecord moved = RecAt(lb, ln - 1);
        SetNumRecs(lb, ln - 1);
        for (size_t i = NumRecs(cb); i > 0; --i) {
          SetRecAt(cb, i, RecAt(cb, i - 1));
        }
        SetRecAt(cb, 0, moved);
        SetNumRecs(cb, NumRecs(cb) + 1);
      } else {
        const uint32_t moved_key = KeyAt(lb, ln - 1);
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
        const InobtRecord moved = RecAt(rb, 0);
        for (size_t i = 0; i + 1 < NumRecs(rb); ++i) {
          SetRecAt(rb, i, RecAt(rb, i + 1));
        }
        SetNumRecs(rb, NumRecs(rb) - 1);
        SetRecAt(cb, cn, moved);
        SetNumRecs(cb, cn + 1);
      } else {
        const uint32_t moved_key = KeyAt(rb, 0);
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
                     "cannot rebalance an inobt block")
          .Ag(agno_)
          .Metadata("inobt")
          .Actual(cur);
    }
    if (merged) {
      path.pop_back();
      cur = frame.block;
    }
  }

  if (level_ > 1 && NumRecs(Block(root_)) == 1) {
    uint8_t* root_block = MutableBlock(root_);
    const uint32_t child = PtrAt(root_block, 0);
    if (LevelOf(child) != 0) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     "cannot collapse a multi-level inobt")
          .Ag(agno_)
          .Metadata("inobt");
    }
    const std::vector<Entry> children = ReadEntries(child);
    PutBE32(root_block, bt::kMagic, ondisk::kIbtCrcMagic);
    PutBE16(root_block, bt::kLevel, 0);
    PutBE32(root_block, bt::kSLeftsib, kNullAgBlock);
    PutBE32(root_block, bt::kSRightSib, kNullAgBlock);
    SetNumRecs(root_block, static_cast<uint32_t>(children.size()));
    for (size_t i = 0; i < children.size(); ++i) {
      SetRecAt(root_block, i, children[i].rec);
    }
    Status sr = FreeTreeBlock(child);
    if (!sr.ok()) return sr;
    level_ = 1;
  }
  return OkStatus();
}

// ---------------------------------------------------------------------------
// InodeAllocator::Impl
// ---------------------------------------------------------------------------
class AllocImpl {
 public:
  AllocImpl(XfsTransaction* txn, BlockAllocator* alloc, BlockDevice* device,
            const XfsSuperblock* sb, const XfsGeometry* geo)
      : txn_(txn), alloc_(alloc), device_(device), geo_(geo) {
    igeo_ = MakeInodeGeometry(*sb, *geo);
  }

  Status AllocateInode(uint32_t preferred_agno, uint32_t preferred_agino,
                       uint64_t* ino_out);
  Status FreeInode(uint64_t ino);
  Result<uint32_t> UnlinkedBucketHead(uint32_t agno, uint32_t bucket);
  Status SetUnlinkedBucketHead(uint32_t agno, uint32_t bucket, uint32_t agino);
  Status Commit();

  int64_t icount_delta() const { return icount_delta_; }
  int64_t ifree_delta() const { return ifree_delta_; }

 private:
  Status LoadAg(uint32_t agno, AgiState** out);
  Status FindAllocationRecord(AgiState* ag, uint32_t preferred_agino,
                              InobtRecord* rec, uint32_t* offset);
  Status AllocateFromRecord(AgiState* ag, const InobtRecord& rec,
                            uint32_t offset);
  Status AllocateNewChunk(AgiState* ag, InobtRecord* rec_out,
                          uint32_t* offset_out);
  Status ApplySuperblockDeltas();
  Status AddAgiBuffer(AgiState* ag);

  XfsTransaction* txn_;
  BlockAllocator* alloc_;
  BlockDevice* device_;
  const XfsGeometry* geo_;
  XfsInodeGeometry igeo_;
  std::map<uint32_t, std::unique_ptr<AgiState>> ags_;
  int64_t icount_delta_ = 0;
  int64_t ifree_delta_ = 0;
};

Status AllocImpl::LoadAg(uint32_t agno, AgiState** out) {
  auto it = ags_.find(agno);
  if (it != ags_.end()) {
    *out = it->second.get();
    return OkStatus();
  }
  auto checked = ReadAgi(*device_, *geo_, agno);
  if (!checked.ok()) return checked.error();

  auto ag = std::make_unique<AgiState>();
  ag->agno = agno;
  ag->agi_buf.assign(geo_->sector_size, 0);
  const uint64_t offset = geo_->AgByteOffset(agno) + AgiByteOffset(*geo_);
  if (!device_->Read(offset, ag->agi_buf.data(), ag->agi_buf.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read AGI")
        .Ag(agno)
        .Offset(offset)
        .Metadata("AGI");
  }
  ag->count = checked.value().count;
  ag->freecount = checked.value().freecount;
  ag->newino = checked.value().newino;
  ag->orig_count = ag->count;
  ag->orig_freecount = ag->freecount;
  ag->inobt = std::make_unique<InobtTree>(device_, alloc_, geo_, agno);
  Status s = ag->inobt->Load(checked.value().root, checked.value().level);
  if (!s.ok()) return s;

  // Cross-check the AGI counters against the inobt records.
  std::vector<InobtRecord> recs;
  s = ag->inobt->Collect(&recs);
  if (!s.ok()) return s;
  uint64_t count = 0, freecount = 0;
  for (const InobtRecord& r : recs) {
    count += 64;
    freecount += r.freecount;
  }
  if (count != ag->count || freecount != ag->freecount) {
    return XFS_ERR(XfsError::kCorruptAgHeader,
                   "AGI inode counters do not match the inobt")
        .Ag(agno)
        .Metadata("AGI")
        .Expected(ag->count)
        .Actual(count);
  }

  AgiState* raw = ag.get();
  ags_.emplace(agno, std::move(ag));
  *out = raw;
  return OkStatus();
}

Status AllocImpl::FindAllocationRecord(AgiState* ag, uint32_t preferred_agino,
                                  InobtRecord* rec_out, uint32_t* offset_out) {
  std::vector<InobtRecord> recs;
  Status s = ag->inobt->Collect(&recs);
  if (!s.ok()) return s;
  auto has_free = [](const InobtRecord& r) { return r.freecount > 0; };
  // 1. The record containing the parent's agino (same AG), like
  //    xfs_dialloc_ag_inobt.
  size_t chosen = recs.size();
  if (preferred_agino != 0) {
    size_t containing = recs.size();
    for (size_t i = 0; i < recs.size(); ++i) {
      if (recs[i].startino <= preferred_agino &&
          preferred_agino < recs[i].startino + 64) {
        containing = i;
        break;
      }
    }
    if (containing != recs.size() && has_free(recs[containing])) {
      chosen = containing;
    } else if (containing != recs.size()) {
      // 2. Scan outward from the parent's chunk (searchdistance = 10).
      for (size_t d = 1; d < 10; ++d) {
        const bool left = containing >= d;
        const bool right = containing + d < recs.size();
        if (left && has_free(recs[containing - d])) {
          chosen = containing - d;
          break;
        }
        if (right && has_free(recs[containing + d])) {
          chosen = containing + d;
          break;
        }
      }
    }
  }
  // 3. Otherwise the first record with a free inode.
  if (chosen == recs.size()) {
    for (size_t i = 0; i < recs.size(); ++i) {
      if (has_free(recs[i])) {
        chosen = i;
        break;
      }
    }
  }
  if (chosen == recs.size()) {
    return XFS_ERR(XfsError::kNoSpace, "AG has no free inode")
        .Ag(ag->agno)
        .Metadata("inobt");
  }
  const InobtRecord& rec = recs[chosen];
  // xfs_inobt_first_free_inode: the lowest clear bit in the free mask.
  uint32_t offset = 0;
  while (offset < 64 && ((rec.free >> offset) & 1ULL) == 0) ++offset;
  if (offset == 64) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "inobt record freecount does not match its mask")
        .Ag(ag->agno)
        .Metadata("inobt")
        .Actual(rec.startino);
  }
  *rec_out = rec;
  *offset_out = offset;
  return OkStatus();
}

Status AllocImpl::AllocateFromRecord(AgiState* ag, const InobtRecord& rec,
                                uint32_t offset) {
  InobtRecord updated = rec;
  updated.free &= ~(1ULL << offset);
  updated.freecount--;
  Status s = ag->inobt->Update(updated);
  if (!s.ok()) return s;
  ag->freecount--;
  ag->dirty = true;
  --ifree_delta_;
  return OkStatus();
}

Status AllocImpl::AllocateNewChunk(AgiState* ag, InobtRecord* rec_out,
                              uint32_t* offset_out) {
  // Allocate the chunk blocks: ialloc_blks contiguous blocks, aligned like
  // xfs_ialloc_ag_alloc (cluster_align, which is sb_inoalignmt when the
  // filesystem has the align feature and the value is usable).
  Allocation alloc;
  Status s = alloc_->AllocateFromAg(ag->agno, igeo_.ialloc_blks,
                                    igeo_.cluster_align, &alloc);
  if (!s.ok()) return s;
  const uint32_t agbno =
      static_cast<uint32_t>(alloc.fsb & ((1ULL << geo_->agblklog) - 1));
  if (agbno == 0 ||
      agbno + igeo_.ialloc_blks > geo_->AgBlocksFor(ag->agno)) {
    return XFS_ERR(XfsError::kInternal, "chunk allocation out of range")
        .Ag(ag->agno)
        .Agbno(agbno)
        .Metadata("inobt");
  }
  const uint32_t startino = agbno << geo_->inopblog;

  // The icreate item makes replay rebuild the whole inode cluster from the
  // recorded geometry; the kernel's mechanism for a new chunk.
  const uint32_t gen = NextInodeGeneration();
  std::vector<uint8_t> icreate(lf::kIcreateLogSize, 0);
  StoreLE16(icreate.data() + lf::kIclType, lf::kItemIcreate);
  StoreLE16(icreate.data() + lf::kIclSize, 1);
  PutBE32(icreate.data(), lf::kIclAg, ag->agno);
  PutBE32(icreate.data(), lf::kIclAgbno, agbno);
  PutBE32(icreate.data(), lf::kIclCount, igeo_.ialloc_inos);
  PutBE32(icreate.data(), lf::kIclIsize, geo_->inode_size);
  PutBE32(icreate.data(), lf::kIclLength, igeo_.ialloc_blks);
  PutBE32(icreate.data(), lf::kIclGen, gen);
  txn_->AddLogOp(std::move(icreate));

  // Insert the full (holemask 0) chunk record, then take the first inode.
  uint32_t offset = 0;
  InobtRecord rec;
  rec.startino = startino;
  rec.free = ~0ULL;
  rec.freecount = 64;
  // First free inode of a fresh chunk is offset 0.
  rec.free &= ~(1ULL << offset);
  rec.freecount--;
  s = ag->inobt->Insert(rec);
  if (!s.ok()) return s;

  ag->count += 64;
  ag->freecount += 64;
  ag->newino = startino;
  icount_delta_ += 64;
  ifree_delta_ += 64;
  --ifree_delta_;  // the inode we are handing out
  ag->freecount--;
  ag->dirty = true;
  *rec_out = rec;
  *offset_out = offset;
  return OkStatus();
}

Status AllocImpl::AllocateInode(uint32_t preferred_agno, uint32_t preferred_agino,
                           uint64_t* ino_out) {
  if (geo_->agcount == 0) {
    return XFS_ERR(XfsError::kOutOfRange, "filesystem has no AGs")
        .Metadata("allocator");
  }
  if (preferred_agno >= geo_->agcount) preferred_agno = 0;

  // Existing free inodes first.
  for (uint32_t i = 0; i < geo_->agcount; ++i) {
    const uint32_t agno = (preferred_agno + i) % geo_->agcount;
    AgiState* ag = nullptr;
    Status s = LoadAg(agno, &ag);
    if (!s.ok()) return s;
    if (ag->freecount == 0) continue;
    InobtRecord rec;
    uint32_t offset = 0;
    s = FindAllocationRecord(ag, agno == preferred_agno ? preferred_agino : 0,
                             &rec, &offset);
    if (s.ok()) {
      s = AllocateFromRecord(ag, rec, offset);
      if (!s.ok()) return s;
      *ino_out = geo_->AginoToIno(agno, rec.startino + offset);
      return OkStatus();
    }
    if (s.error().code != XfsError::kNoSpace) return s;
  }

  // No free inode anywhere: allocate a new chunk.
  for (uint32_t i = 0; i < geo_->agcount; ++i) {
    const uint32_t agno = (preferred_agno + i) % geo_->agcount;
    AgiState* ag = nullptr;
    Status s = LoadAg(agno, &ag);
    if (!s.ok()) return s;
    InobtRecord rec;
    uint32_t offset = 0;
    s = AllocateNewChunk(ag, &rec, &offset);
    if (s.ok()) {
      *ino_out = geo_->AginoToIno(agno, rec.startino + offset);
      return OkStatus();
    }
    if (s.error().code != XfsError::kNoSpace) return s;
  }
  return XFS_ERR(XfsError::kNoSpace,
                 "no free inode and no space for a new inode chunk")
      .Metadata("allocator");
}

Status AllocImpl::FreeInode(uint64_t ino) {
  const uint32_t agno = geo_->InoAgno(ino);
  const uint32_t agino = geo_->InoAgino(ino);
  AgiState* ag = nullptr;
  Status s = LoadAg(agno, &ag);
  if (!s.ok()) return s;
  auto found = ag->inobt->FindLE(agino);
  if (!found.ok()) return found.error();
  InobtRecord rec = found.value();
  if (agino < rec.startino || agino >= rec.startino + 64) {
    return XFS_ERR(XfsError::kCorruptBtree,
                   "inode does not belong to the inobt record found")
        .Ino(ino)
        .Metadata("inobt")
        .Actual(rec.startino);
  }
  const uint32_t offset = agino - rec.startino;
  if ((rec.free >> offset) & 1ULL) {
    return XFS_ERR(XfsError::kCorruptInode, "freeing an already free inode")
        .Ino(ino)
        .Metadata("inobt");
  }
  const uint64_t new_free = rec.free | (1ULL << offset);

  if (new_free == ~0ULL && geo_->inopblock <= ondisk::kInodesPerChunk) {
    // Whole chunk free: remove it and release its blocks, like
    // xfs_difree_inobt + xfs_difree_inode_chunk.
    s = ag->inobt->Remove(rec.startino);
    if (!s.ok()) return s;
    ag->count -= 64;
    ag->freecount -= 63;
    icount_delta_ -= 64;
    ifree_delta_ -= 63;
    ag->dirty = true;
    const uint64_t chunk_fsb = geo_->AgFsb(agno, rec.startino >> geo_->inopblog);
    return alloc_->Free(chunk_fsb, igeo_.ialloc_blks);
  }

  InobtRecord updated = rec;
  updated.free = new_free;
  updated.freecount++;
  s = ag->inobt->Update(updated);
  if (!s.ok()) return s;
  ag->freecount++;
  ag->dirty = true;
  ++ifree_delta_;
  return OkStatus();
}

Result<uint32_t> AllocImpl::UnlinkedBucketHead(uint32_t agno, uint32_t bucket) {
  if (bucket >= agi::kUnlinkedBuckets) {
    return XFS_ERR(XfsError::kOutOfRange, "AGI unlinked bucket out of range")
        .Ag(agno)
        .Actual(bucket)
        .Metadata("AGI");
  }
  AgiState* ag = nullptr;
  Status s = LoadAg(agno, &ag);
  if (!s.ok()) return s.error();
  return GetBE32(ag->agi_buf.data(), agi::kUnlinked + bucket * 4);
}

Status AllocImpl::SetUnlinkedBucketHead(uint32_t agno, uint32_t bucket,
                                   uint32_t agino) {
  if (bucket >= agi::kUnlinkedBuckets) {
    return XFS_ERR(XfsError::kOutOfRange, "AGI unlinked bucket out of range")
        .Ag(agno)
        .Actual(bucket)
        .Metadata("AGI");
  }
  if (agino != kNullAgino &&
      agino >= geo_->AgBlocksFor(agno) * geo_->inopblock) {
    return XFS_ERR(XfsError::kOutOfRange, "unlinked agino out of range")
        .Ag(agno)
        .Actual(agino)
        .Metadata("AGI");
  }
  AgiState* ag = nullptr;
  Status s = LoadAg(agno, &ag);
  if (!s.ok()) return s;
  PutBE32(ag->agi_buf.data(), agi::kUnlinked + bucket * 4, agino);
  ag->dirty = true;
  return OkStatus();
}

Status AllocImpl::AddAgiBuffer(AgiState* ag) {
  if (ag->count > geo_->AgBlocksFor(ag->agno) * geo_->inopblock ||
      ag->freecount > ag->count) {
    return XFS_ERR(XfsError::kInternal,
                   "AGI inode counters would leave their range")
        .Ag(ag->agno)
        .Metadata("AGI")
        .Expected(ag->count)
        .Actual(ag->freecount);
  }
  uint8_t* b = ag->agi_buf.data();
  PutBE32(b, agi::kCount, ag->count);
  PutBE32(b, agi::kFreecount, ag->freecount);
  PutBE32(b, agi::kNewino, ag->newino);
  PutBE32(b, agi::kRoot, ag->inobt->root());
  PutBE32(b, agi::kLevel, ag->inobt->level());
  const uint64_t offset = geo_->AgByteOffset(ag->agno) + AgiByteOffset(*geo_);
  return txn_->AddMetadataBuffer(offset, ag->agi_buf, lf::kBlftAgi, agi::kLsn,
                                 agi::kCrc, "AGI");
}

Status AllocImpl::ApplySuperblockDeltas() {
  if (icount_delta_ == 0 && ifree_delta_ == 0) return OkStatus();
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
  const int64_t icount = GetBE64(sb_buf.data(), sb::kIcount);
  const int64_t ifree = GetBE64(sb_buf.data(), sb::kIfree);
  const int64_t new_icount = icount + icount_delta_;
  const int64_t new_ifree = ifree + ifree_delta_;
  if (new_icount < 0 ||
      static_cast<uint64_t>(new_icount) > geo_->dblocks * geo_->inopblock ||
      new_ifree < 0 || new_ifree > new_icount) {
    return XFS_ERR(XfsError::kInternal,
                   "superblock inode counters would leave their range")
        .Metadata("superblock")
        .Expected(static_cast<uint64_t>(new_icount))
        .Actual(static_cast<uint64_t>(new_ifree));
  }
  PutBE64(sb_buf.data(), sb::kIcount, static_cast<uint64_t>(new_icount));
  PutBE64(sb_buf.data(), sb::kIfree, static_cast<uint64_t>(new_ifree));
  return txn_->AddMetadataBuffer(0, std::move(sb_buf), lf::kBlftSb, sb::kLsn,
                                 sb::kCrc, "superblock");
}

Status AllocImpl::Commit() {
  for (auto& entry : ags_) {
    AgiState* ag = entry.second.get();
    if (ag->dirty) {
      Status s = AddAgiBuffer(ag);
      if (!s.ok()) return s;
    }
    for (uint32_t agbno : ag->inobt->dirty_blocks()) {
      const auto& blocks = ag->inobt->blocks();
      auto block_it = blocks.find(agbno);
      if (block_it == blocks.end()) continue;
      const uint64_t offset = geo_->FsbToByte(geo_->AgFsb(ag->agno, agbno));
      Status s = txn_->AddMetadataBuffer(offset, block_it->second,
                                         lf::kBlftBtree, bt::kSLsn, bt::kSCrc,
                                         "inobt");
      if (!s.ok()) return s;
    }
  }
  Status s = ApplySuperblockDeltas();
  if (!s.ok()) return s;
  if (!txn_->record_fits()) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "inode allocation metadata does not fit in one log record")
        .Metadata("transaction");
  }
  return OkStatus();
}

}  // namespace

XfsInodeGeometry MakeInodeGeometry(const XfsSuperblock& sb,
                                   const XfsGeometry& geo) {
  XfsInodeGeometry g;
  g.inodes_per_block = geo.inopblock;
  g.ialloc_inos = std::max<uint32_t>(ondisk::kInodesPerChunk, geo.inopblock);
  g.ialloc_blks = g.ialloc_inos >> geo.inopblog;

  // Inode cluster size: xfs_ialloc_setup_geometry.
  constexpr uint64_t kBigClusterSize = 8192;
  constexpr uint64_t kMinInodeSize = 256;
  uint64_t raw = kBigClusterSize;
  if (sb.IsV5()) {
    const uint64_t scaled = raw * (geo.inode_size / kMinInodeSize);
    if (sb.inoalignmt >= (scaled >> geo.block_log)) raw = scaled;
  }
  if (raw > geo.block_size) {
    g.blocks_per_cluster = static_cast<uint32_t>(raw >> geo.block_log);
  } else {
    g.blocks_per_cluster = 1;
  }
  g.inodes_per_cluster = g.blocks_per_cluster << geo.inopblog;

  // Cluster alignment: sb_inoalignmt when the align feature is set and the
  // value is usable, otherwise the kernel uses alignment 1.
  constexpr uint16_t kSbVersionAlignBit = 0x0010;
  const bool has_align = (sb.versionnum & kSbVersionAlignBit) != 0;
  g.cluster_align =
      (has_align && sb.inoalignmt >= g.blocks_per_cluster) ? sb.inoalignmt : 1;
  return g;
}

class InodeAllocator::Impl {
 public:
  std::unique_ptr<AllocImpl> impl;
};

InodeAllocator::InodeAllocator(XfsTransaction* txn, BlockAllocator* alloc,
                               BlockDevice* device, const XfsSuperblock* sb,
                               const XfsGeometry* geo)
    : impl_(std::make_unique<InodeAllocator::Impl>()) {
  impl_->impl = std::make_unique<AllocImpl>(txn, alloc, device, sb, geo);
}

InodeAllocator::~InodeAllocator() = default;

Status InodeAllocator::AllocateInode(uint32_t preferred_agno,
                                     uint32_t preferred_agino,
                                     uint64_t* ino_out) {
  return impl_->impl->AllocateInode(preferred_agno, preferred_agino, ino_out);
}

Status InodeAllocator::FreeInode(uint64_t ino) {
  return impl_->impl->FreeInode(ino);
}

Result<uint32_t> InodeAllocator::UnlinkedBucketHead(uint32_t agno,
                                                    uint32_t bucket) {
  return impl_->impl->UnlinkedBucketHead(agno, bucket);
}

Status InodeAllocator::SetUnlinkedBucketHead(uint32_t agno, uint32_t bucket,
                                             uint32_t agino) {
  return impl_->impl->SetUnlinkedBucketHead(agno, bucket, agino);
}

Status InodeAllocator::Commit() { return impl_->impl->Commit(); }

int64_t InodeAllocator::icount_delta() const {
  return impl_->impl->icount_delta();
}

int64_t InodeAllocator::ifree_delta() const {
  return impl_->impl->ifree_delta();
}

}  // namespace xfs
