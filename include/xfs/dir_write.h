// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - directory mutation (RW-3).
//
// A directory is stored one of four ways; the writer handles all four and the
// conversions between them, following fs/xfs/libxfs/xfs_dir2_{sf,block,leaf,
// node,data}.c and the da btree helpers:
//
//   shortform  entries in the inode literal area
//   block      one data block with an embedded leaf array and tail
//   leaf       data blocks + one leaf1 block (bests table) at the leaf offset
//   node       data blocks + leafn blocks + free blocks + a da btree
//
// Invariants maintained here (checked by the kernel verifiers and xfs_repair):
//
//   - every data block is a chain of entries and unused records with no gaps,
//     exact 8-byte tags, and a bestfree table that is exactly the three
//     largest freespace runs (recomputed after every change, like
//     xfs_dir2_data_freescan);
//   - leaf entries are sorted by hash; removed entries are marked stale
//     (address = NULLDATAPTR) and stale entries are compacted before a split;
//   - the leaf chain (forw/back) is consistent and da btree keys are the
//     *last* hash value of their child (xfs_da3_fixhashpath semantics);
//   - for the node format a free block (XFS_DIR2_FREE_SPACE) holds the bests
//     table for a range of data blocks: firstdb = free_max_bests * index,
//     nvalid/nused and one entry per data block (NULLDATAOFF = no block);
//   - `di_size` is the size of the *data* space only
//     (xfs_dir2_grow_inode: leaf/free blocks do not move the size) and a
//     block-format directory maps exactly one block (xfs_dir2_format).
//
// Every allocation goes through the caller's BlockAllocator and every fsblock
// through the caller's DataForkEditor, so the directory's extent map is
// updated (and converted to/from btree format) with the RW-2 machinery.
// Nothing is written before the transaction commits.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/directory.h"
#include "xfs/error.h"
#include "xfs/inode.h"
#include "xfs/superblock.h"
#include "xfs/transaction.h"

namespace xfs {

class BlockAllocator;
class DataForkEditor;

struct DirEntryWrite {
  std::string name;  // raw XFS name bytes
  uint64_t ino = 0;
  uint8_t ftype = 0;  // XFS_DIR3_FT_*
};

class DirectoryEditor {
 public:
  DirectoryEditor(XfsTransaction* txn, BlockAllocator* alloc,
                  BlockDevice* device, const XfsGeometry* geo,
                  DataForkEditor* fork);
  ~DirectoryEditor();
  DirectoryEditor(const DirectoryEditor&) = delete;
  DirectoryEditor& operator=(const DirectoryEditor&) = delete;

  // Rejects things this build must not touch *before* anything is modified.
  static Status CheckMutable(const XfsInode& inode);

  // Loads the directory into memory from a parsed inode. The inode's data
  // fork must already be loaded into the DataForkEditor.
  Status Load(const XfsInode& inode);

  // Inserts one entry (the name must not exist yet).
  Status Insert(const DirEntryWrite& entry);

  // Removes one entry and returns it.
  Result<XfsDirEntry> Remove(std::string_view name);

  // Updates the directory's parent reference (".."): the shortform header
  // field or the ".." dirent in the data blocks. Used by rmdir, which
  // repoints an unlinked child directory at the root like xfs_remove.
  Status RepointDotDot(uint64_t parent_ino);

  bool dirty() const { return dirty_; }
  XfsDirFormat format() const { return fmt_; }
  // Directory data-space size after the edits (the caller stores it in
  // di_size).
  uint64_t data_size() const { return data_size_; }

  // Applies the directory's data fork to a mutable copy of the raw inode
  // image: shortform bytes (XFS_ILOG_DDATA) or the extent/btree fork through
  // the DataForkEditor (XFS_ILOG_DEXT/DBROOT), and writes di_size.
  Status FinalizeInode(std::vector<uint8_t>* raw_inode, uint32_t* ilog_fields,
                       std::vector<uint8_t>* fork_region);
  // Number of data blocks the directory maps (leaf/node formats).
  uint32_t data_blocks() const {
    return static_cast<uint32_t>(data_size_ >> block_log_);
  }

  // Queues every dirty directory block as an XFS_LI_BUF item (blkno/uuid/
  // owner/CRC set here, LSN stamped by AddMetadataBuffer).
  Status LogBlocks();

 private:
  // One stored entry with its position in the data space. Used by the
  // shortform and block formats; sorted by offset.
  struct PlacedEntry {
    DirEntryWrite e;
    uint32_t offset = 0;
  };

  struct LeafEnt {
    uint32_t hash = 0;
    uint32_t addr = 0;  // 0 = stale
  };
  struct LeafBlock {
    uint32_t dablk = 0;
    uint32_t forw = 0;
    uint32_t back = 0;
    std::vector<LeafEnt> ents;
    bool dirty = false;
  };
  struct NodeEnt {
    uint32_t hash = 0;    // last hash of the child
    uint32_t before = 0;  // child dablk
  };
  struct NodeBlock {
    uint32_t dablk = 0;
    uint16_t level = 0;
    uint32_t forw = 0;
    uint32_t back = 0;
    std::vector<NodeEnt> ents;
    bool dirty = false;
  };
  struct FreeBlock {
    uint32_t dablk = 0;
    std::vector<uint16_t> bests;  // NULLDATAOFF = no data block
    bool dirty = false;
  };

  // ---- format detection / loading ----
  Status LoadShortform(const XfsInode& inode);
  Status LoadBlock();
  Status LoadLeaf();
  Status LoadNode();
  Status ReadDirBlock(uint64_t db, std::vector<uint8_t>* buf) const;
  uint64_t FsbForFileBlock(uint64_t file_block) const;

  // ---- shortform / block (logical entries) ----
  bool SfWide(uint64_t extra_ino) const;
  size_t SfHeaderSizeFor(bool wide) const;
  size_t SfEntrySizeFor(size_t namelen, bool wide) const;
  size_t DataEntrySize(size_t namelen) const;
  // Finds where a new entry should go in `placed_` and returns the offset.
  // `first_offset` is 96 for shortform and 64 for the block format.
  uint32_t PlaceEntry(size_t entsize, uint32_t first_offset, bool shortform,
                      bool wide) const;
  Status InsertPlaced(const DirEntryWrite& entry, uint32_t first_offset,
                      bool shortform);
  Status RemovePlaced(std::string_view name, XfsDirEntry* removed);
  void BuildShortformImage(std::vector<uint8_t>* image) const;
  void BuildBlockImage(std::vector<uint8_t>* image) const;
  void BuildDataBlockFromPlaced(std::vector<uint8_t>* image,
                                uint64_t fsb) const;
  void BuildLeafFromPlaced(LeafBlock* leaf) const;
  Status BlockToLeaf();

  // ---- data block primitives (operate on a 4096-byte image) ----
  uint32_t DataEnd(const std::vector<uint8_t>& buf) const;
  Status DataUseFree(std::vector<uint8_t>* buf, uint32_t end, uint32_t off,
                     uint32_t len);
  Status DataMakeFree(std::vector<uint8_t>* buf, uint32_t end, uint32_t off,
                      uint32_t len);
  void DataScanBestfree(std::vector<uint8_t>* buf, uint32_t end) const;
  uint16_t DataBestFree(const std::vector<uint8_t>& buf) const;
  Status DataInsertEntry(std::vector<uint8_t>* buf, uint32_t end,
                         const DirEntryWrite& entry, uint32_t* off_out);
  Status DataRemoveEntry(std::vector<uint8_t>* buf, uint32_t end,
                         uint32_t off, bool* empty_out);
  void InitDataBlock(std::vector<uint8_t>* buf, uint64_t blkno_daddr,
                     uint64_t owner) const;
  void InitLeafBlock(std::vector<uint8_t>* buf, uint64_t blkno_daddr,
                     uint64_t owner, uint16_t magic) const;
  void InitFreeBlock(std::vector<uint8_t>* buf, uint64_t blkno_daddr,
                     uint64_t owner) const;
  void InitNodeBlock(std::vector<uint8_t>* buf, uint64_t blkno_daddr,
                     uint64_t owner, uint16_t level) const;

  // ---- leaf / node data blocks and bests ----
  Result<uint32_t> PickDataBlock(uint32_t hash, size_t need);
  Status GrowDataBlock(uint32_t* db_out);
  Status DropDataBlockIfEmpty(uint32_t db, bool empty);
  uint16_t BestsFor(uint32_t db) const;

  // ---- leaf format ----
  Status InsertLeaf(const DirEntryWrite& entry);  // kNoSpace = convert
  Status RemoveLeaf(std::string_view name, XfsDirEntry* removed);
  Status LeafToNode();
  Status AddLeafEntry(LeafBlock* leaf, size_t index, uint32_t hash,
                      uint32_t addr);
  void CompactLeaf(LeafBlock* leaf);
  LeafBlock* FindLeafRoot() { return &leaf_blocks_[root_dablk_]; }

  // ---- node format ----
  Status InsertNode(const DirEntryWrite& entry);
  Status RemoveNode(std::string_view name, XfsDirEntry* removed);
  LeafBlock* LeafForHash(uint32_t hash);
  Status SplitLeaf(LeafBlock* leaf, std::vector<LeafEnt> middle);
  Status RebuildNodeTree();
  Status RebuildFreeBlocks();
  Status NodeToLeaf1();
  uint32_t NextLeafDablk() const;
  uint32_t NextNodeDablk() const;

  // ---- block allocation ----
  Result<uint64_t> AllocDirBlock(uint64_t file_block);
  Status FreeDirBlock(uint64_t file_block);

  // ---- logging ----
  Status LogDataBlock(uint32_t db);
  Status LogLeafBlock(const LeafBlock& leaf);
  Status LogFreeBlock(const FreeBlock& fb);
  Status LogNodeBlock(const NodeBlock& node);
  Status LogBlockImage(uint64_t db, std::vector<uint8_t> image,
                       uint16_t blf_type, size_t lsn_off, size_t crc_off);

  XfsTransaction* txn_;
  BlockAllocator* alloc_;
  BlockDevice* device_;
  const XfsGeometry* geo_;
  DataForkEditor* fork_;

  XfsDirFormat fmt_ = XfsDirFormat::kShortform;
  bool dirty_ = false;
  uint64_t ino_ = 0;
  uint32_t block_log_ = 0;
  uint32_t dir_block_size_ = 0;
  uint32_t leaf_max_ents_ = 0;
  uint32_t node_max_ents_ = 0;
  uint32_t free_max_bests_ = 0;
  uint32_t root_dablk_ = 0;

  // shortform / block
  std::vector<PlacedEntry> placed_;
  uint64_t sf_parent_ = 0;
  std::vector<uint8_t> block_buf_;
  bool block_dirty_ = false;

  // leaf / node
  std::map<uint32_t, std::vector<uint8_t>> data_blocks_;
  std::set<uint32_t> data_dirty_;
  std::map<uint32_t, uint16_t> bests_;  // db -> longest free run
  std::map<uint32_t, LeafBlock> leaf_blocks_;
  std::map<uint32_t, NodeBlock> node_blocks_;
  std::map<uint32_t, FreeBlock> free_blocks_;
  uint64_t data_size_ = 0;
};

}  // namespace xfs
