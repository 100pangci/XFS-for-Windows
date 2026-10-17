// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - standard XFS btree (xfs_btree_block format) support.
//
// Used for AG metadata btrees (bnobt/cntbt/inobt/finobt, short pointers) and
// the inode data-fork bmap btree (long pointers). Directory da-btrees use a
// different on-disk format and are handled in directory.cpp.
//
// All traversal is bounded:
//   - hard depth cap (kMaxTraversalLevels and per-tree values)
//   - visited-block set (cycle detection)
//   - numrecs bounded by the block geometry
//   - pointers validated against the caller's address space
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "xfs/error.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"
#include "xfs/uuid.h"

namespace xfs {

enum class BtreeFormat { kShort, kLong };

// Description of one tree type for verification purposes.
struct BtreeSpec {
  const char* name = "btree";
  BtreeFormat format = BtreeFormat::kShort;
  uint32_t block_size = 0;
  size_t key_size = 0;
  size_t ptr_size = 0;
  size_t rec_size = 0;
  uint32_t magic = 0;              // expected magic (v5 crc magics)
  uint16_t max_depth = ondisk::btree::kMaxTraversalLevels;
  uint64_t null_ptr = 0;           // "no sibling/child" value
  uint64_t expected_daddr = 0;     // on-disk bb_blkno value (daddr units)
  uint64_t expected_owner = 0;     // e.g. AG number, inode number
  size_t owner_size = 0;           // 4 for short format, 8 for long
  Uuid uuid{};
  uint64_t sb_lsn = 0;
  bool verify_identity = true;     // magic/crc/uuid/blkno/owner/lsn
  // When set, computes the expected bb_blkno (daddr units) for a pointer;
  // used to verify block identity per block (bmbt / AG trees).
  std::function<bool(uint64_t ptr, uint64_t* daddr)> ptr_to_daddr;
};

struct BtreeBlockMeta {
  uint32_t magic = 0;
  uint16_t level = 0;
  uint16_t numrecs = 0;
  uint64_t leftsib = 0;
  uint64_t rightsib = 0;
  bool long_format = false;
  size_t header_size = 0;
  size_t maxrecs = 0;  // for this block's level (leaf vs node)
};

// Parses + verifies one btree block. `len` must equal spec.block_size.
Status VerifyBtreeBlock(const uint8_t* buf, size_t len, const BtreeSpec& spec,
                        BtreeBlockMeta* out);

// Returns the child pointer at `index` (0-based) of an internal block.
uint64_t BtreeChildPtr(const uint8_t* buf, const BtreeBlockMeta& meta,
                       const BtreeSpec& spec, size_t index);
// Key value at `index` (first key field interpreted as u64 for both trees:
// startblock<<32|blockcount for alloc, startino, startoff for bmbt).
uint64_t BtreeKeyValue(const uint8_t* buf, const BtreeBlockMeta& meta,
                       const BtreeSpec& spec, size_t index);
// Leaf record address.
const uint8_t* BtreeLeafRec(const uint8_t* buf, const BtreeBlockMeta& meta,
                            const BtreeSpec& spec, size_t index);

struct BtreeWalkCallbacks {
  // Reads a block addressed by its pointer. Root is just another block.
  std::function<bool(uint64_t ptr, std::vector<uint8_t>* buf)> read_block;
  // Bounds check for a pointer before reading.
  std::function<bool(uint64_t ptr)> ptr_in_range;
  // Called for every verified block, in tree order (parents before children).
  std::function<Status(uint64_t ptr, const BtreeBlockMeta& meta,
                       const uint8_t* buf)>
      visit;
  // If set and returns false for a level, that subtree is not descended into.
  std::function<bool(uint16_t level)> descend;
  // If false, sibling cross-checks are relaxed (for trees where siblings are
  // not maintained, e.g. some metadata btrees).
  bool check_siblings = true;
};

// Walks the whole tree starting at `root`, verifying every block.
Status WalkBtree(const BtreeSpec& spec, uint64_t root,
                 const BtreeWalkCallbacks& callbacks);

// Walks down a single path selecting the child whose key is <= target
// (largest key not greater than target), reporting each visited block.
Status WalkBtreePath(const BtreeSpec& spec, uint64_t root, uint64_t target_key,
                     const BtreeWalkCallbacks& callbacks);

// Convenience: max records in a leaf/node block of this geometry.
size_t BtreeMaxRecords(const BtreeSpec& spec, bool leaf);

}  // namespace xfs
