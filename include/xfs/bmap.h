// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - data fork (extent map) mutation for an existing inode.
//
// RW-2 scope: an existing regular file's data fork can grow and shrink.
//
//   - extent format: records packed into the inode's literal area;
//   - btree format: a bmap btree whose root lives in the inode fork
//     (level >= 1) and whose blocks are regular bmbt blocks.
//
// Invariants maintained here (from xfs_bmap.c / xfs_bmap_btree.c / xfs_btree.c):
//
//   - records are ordered by startoff, never overlap, and adjacent records
//     that can be merged (same state, contiguous on-disk blocks) are merged;
//   - a fork is in btree format exactly while the extent count does not fit
//     the literal area, XFS_IFORK_MAXEXT = fork_bytes / 16. Linux refuses to
//     mount a btree fork with fewer extents ("corrupt inode"), so the
//     conversion happens in both directions;
//   - di_nblocks counts the data extents and the bmbt blocks;
//   - every non-root bmbt block keeps at least half of the maximum records;
//   - the in-fork root is stored in the on-disk bmdr layout; the logged
//     XFS_ILOG_DBROOT region is the in-core form (72-byte header + records).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/extent.h"
#include "xfs/inode.h"
#include "xfs/superblock.h"
#include "xfs/transaction.h"

namespace xfs {

class BlockAllocator;

class DataForkEditor {
 public:
  // `attr_fork` selects the second inode fork.  The bmap tree format is
  // identical; only the rmap owner flags (ATTR_FORK / BMBT+ATTR_FORK) and the
  // free path (no refcount sharing) differ.  RW-6D uses this mode for the
  // attribute fork's extent map and BMBT.
  DataForkEditor(XfsTransaction* txn, BlockAllocator* alloc,
                 BlockDevice* device, const XfsGeometry* geo,
                 bool attr_fork = false);
  ~DataForkEditor();
  DataForkEditor(const DataForkEditor&) = delete;
  DataForkEditor& operator=(const DataForkEditor&) = delete;

  // Builds the in-memory tree from the parsed inode (the extents are read via
  // LoadDataForkExtents; btree blocks are read from the device).
  Status Load(const XfsInode& inode);

  // The AG the inode lives in (allocation hint).
  uint32_t preferred_agno() const;

  // Inserts one real extent (written or unwritten) at [startoff, startoff +
  // blockcount). The range must be a hole; contiguous neighbours with the same
  // state and contiguous blocks are merged.
  Status InsertExtent(const XfsExtent& extent);

  // Removes file blocks [first_block, end_block) from the map: extents fully
  // covered are freed, an extent spanning `first_block` is split and its lower
  // part stays.
  Status RemoveRange(uint64_t first_block, uint64_t end_block);

  // When true, RemoveRange also frees the removed data blocks through the
  // allocator and adjusts di_nblocks (truncate). When false it only changes the
  // mapping, which is what an unwritten -> written conversion needs.
  void SetFreeRemovedBlocks(bool free_blocks) { free_removed_ = free_blocks; }

  // Splits a partially covered extent at `first_block` without touching
  // anything else (used by truncate before RemoveRange).
  int64_t nblocks_delta() const { return nblocks_delta_; }
  // Accounts for data blocks allocated outside the editor (the caller
  // allocated them through the BlockAllocator).
  void AddNblocks(int64_t delta) { nblocks_delta_ += delta; }
  uint32_t nextents() const { return nextents_; }
  uint8_t format() const { return format_; }
  uint32_t fork_bytes() const { return fork_bytes_; }
  const std::vector<XfsExtent>& extents() const { return extents_; }

  // Changes the size of the fork image emitted by Finalize.  Used when a
  // forkoff change gives the data fork more or less literal space, or when
  // the attribute fork is resized.  The extent/tree contents are unchanged;
  // if the in-fork btree root no longer fits the new size, a real bmbt block
  // is allocated and the root becomes a level+1 node over it (the kernel's
  // xfs_btree_new_iroot path).
  Status SetForkBytes(uint32_t bytes);

  // Finalize normally converts a btree fork with few extents back to the
  // extents format.  When expanding a data fork because the attr fork shrank,
  // Linux keeps the btree format; callers that need that behaviour clear this
  // flag.  The default remains the RW-2 behaviour (collapse when possible).
  void SetAllowCollapse(bool allow) { allow_collapse_ = allow; }

  // Applies all edits to the fork image inside `raw_inode` (a mutable copy of
  // the inode the caller passed to Load), queues the modified bmbt blocks as
  // buffer log items and returns the data fork log region:
  //   ilog_fields: XFS_ILOG_DEXT (records) or XFS_ILOG_DBROOT (btree root);
  //   fork_region: the packed records or the in-core btree root block.
  Status Finalize(std::vector<uint8_t>* raw_inode, uint32_t* ilog_fields,
                  std::vector<uint8_t>* fork_region);

 private:
  // One btree entry: a leaf extent, or a node's key/pointer pair.
  struct Entry {
    XfsExtent rec;     // valid at level 0; startoff is the key at all levels
    uint64_t ptr = 0;  // child fsb, valid at level > 0
  };
  // Either the in-fork root or one on-disk bmbt block, both kept in the same
  // in-memory shape (header + keys + pointers, leaf records at level 0).
  struct Node {
    std::vector<uint8_t> data;
    uint64_t fsb = 0;  // 0 for the root
    bool is_root = false;
    bool dirty = false;
  };
  struct Frame {
    Node* node = nullptr;
    size_t index = 0;  // 0-based child index
  };

  uint16_t NodeLevel(const Node& node) const;
  uint16_t NodeNumRecs(const Node& node) const;
  void SetNodeNumRecs(Node& node, uint16_t numrecs);
  uint32_t NodeMaxRecs(const Node& node) const;
  uint32_t NodeMinRecs(const Node& node) const;
  size_t KeyOffset(const Node& node) const;
  size_t PtrOffset(const Node& node) const;
  uint64_t NodeKey(const Node& node, size_t i) const;
  void SetNodeKey(Node& node, size_t i, uint64_t startoff);
  uint64_t NodePtr(const Node& node, size_t i) const;
  void SetNodePtr(Node& node, size_t i, uint64_t fsb);
  XfsExtent NodeLeafRec(const Node& node, size_t i) const;
  void SetNodeLeafRec(Node& node, size_t i, const XfsExtent& rec);
  uint64_t FirstKey(const Node& node) const;
  size_t ChooseChild(const Node& node, uint64_t startoff) const;
  std::vector<Entry> ReadEntries(const Node& node) const;
  void WriteEntries(Node& node, const std::vector<Entry>& entries);

  Node* FindNode(uint64_t fsb);
  Status ReadBlock(uint64_t fsb, Node* out);
  Status AllocNode(uint64_t fsb, uint16_t level, uint16_t numrecs, Node** out);
  Status AllocBmbtBlock(uint64_t* fsb);
  Status FreeBmbtBlock(uint64_t fsb);

  // Tree edits (the tree is valid after every one of them).
  Status TreeInsert(const XfsExtent& rec);
  Status TreeRemove(const XfsExtent& rec);
  Status TreeUpdate(const XfsExtent& old_rec, const XfsExtent& new_rec);
  Status SplitLeaf(Node& leaf, const std::vector<Entry>& entries,
                   uint64_t* right_fsb, XfsExtent* right_key);
  Status SplitNode(Node& node, const std::vector<Entry>& entries,
                   uint64_t* right_fsb, XfsExtent* right_key);
  // xfs_btree_make_block_unfull(): before splitting a full block, try to move
  // one entry into the right or left sibling.  The path is the block's
  // ancestors (the block itself is not included).
  bool ShiftForInsertRight(const std::vector<Frame>& path, Node* node,
                           size_t* pos);
  bool ShiftForInsertLeft(const std::vector<Frame>& path, Node* node,
                          size_t* pos);
  Status GrowRoot(const std::vector<Entry>& entries,
                  uint16_t child_level);
  void RemoveEntry(Node& node, size_t pos);
  void FixKeysUpward(const std::vector<Frame>& path, const Node& child);
  void AppendEntries(Node& dst, const Node& src);
  void BorrowLast(Node& from, Node& to);
  void BorrowFirst(Node& from, Node& to);
  Status KillRootChild();
  Status ExtentsToBtree();
  Status CollapseToExtents();

  // Extent list helpers.
  Status ApplyInsert(const XfsExtent& rec);
  Status ApplyRemove(const XfsExtent& rec);
  Status ApplyUpdate(const XfsExtent& old_rec, const XfsExtent& new_rec);
  size_t FindExtentAt(uint64_t startoff) const;

  XfsTransaction* txn_;
  BlockAllocator* alloc_;
  BlockDevice* device_;
  const XfsGeometry* geo_;

  uint64_t ino_ = 0;
  bool attr_fork_ = false;
  uint32_t fork_bytes_ = 0;
  uint8_t format_ = 0;  // on-disk format at Load time, updated by conversions
  std::vector<XfsExtent> extents_;
  uint32_t nextents_ = 0;
  int64_t nblocks_delta_ = 0;
  bool free_removed_ = false;
  bool allow_collapse_ = true;
  bool loaded_ = false;

  Node root_;
  std::map<uint64_t, Node> blocks_;
};

}  // namespace xfs
