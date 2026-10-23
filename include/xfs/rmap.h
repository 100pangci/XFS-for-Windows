// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - typed reverse mapping (rmapbt) reader.
//
// The on-disk rules are documented in docs/RMAPBT.md and were taken from the
// local Linux/xfsprogs reference trees. This header deliberately exposes
// *records and keys*, never raw node bytes: rmapbt internal entries carry a
// low/high key pair and its key order ignores the unwritten record attribute.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/superblock.h"

namespace xfs {

// On-disk special owner values (xfs_format.h). These are negative 64-bit
// values; the high bit separates them from inode numbers.
constexpr uint64_t kRmapOwnerNull = ~0ULL;       // growfs sentinel, never stored
constexpr uint64_t kRmapOwnerUnknown = ~1ULL;    // EFI recovery wildcard
constexpr uint64_t kRmapOwnerFs = ~2ULL;         // static AG headers
constexpr uint64_t kRmapOwnerLog = ~3ULL;        // internal log
constexpr uint64_t kRmapOwnerAg = ~4ULL;         // bno/cnt/rmap blocks
constexpr uint64_t kRmapOwnerInobt = ~5ULL;      // inobt/finobt blocks
constexpr uint64_t kRmapOwnerInodes = ~6ULL;     // real inode chunk backing
constexpr uint64_t kRmapOwnerRefc = ~7ULL;       // refcountbt (read-only)
constexpr uint64_t kRmapOwnerCow = ~8ULL;        // CoW staging (read-only)
constexpr uint64_t kRmapOwnerMin = ~9ULL;        // guard, not a real owner

constexpr bool RmapNonInodeOwner(uint64_t owner) {
  return (owner & (1ULL << 63)) != 0;
}

// Host-order record flags (XFS_RMAP_*). The on-disk offset word stores them
// in bits 63 (attr), 62 (bmbt) and 61 (unwritten).
constexpr uint32_t kRmapAttrFork = 1u << 0;
constexpr uint32_t kRmapBmbtBlock = 1u << 1;
constexpr uint32_t kRmapUnwritten = 1u << 2;
constexpr uint32_t kRmapKeyFlags = kRmapAttrFork | kRmapBmbtBlock;
constexpr uint32_t kRmapRecordFlags = kRmapUnwritten;
constexpr uint64_t kRmapOffsetMask = 0x003fffffffffffffULL;
constexpr uint64_t kRmapOffsetAttr = 1ULL << 63;
constexpr uint64_t kRmapOffsetBmbt = 1ULL << 62;
constexpr uint64_t kRmapOffsetUnwritten = 1ULL << 61;

struct RmapKey {
  uint32_t startblock = 0;
  uint64_t owner = 0;
  uint64_t offset = 0;  // logical fork offset, flags removed for comparison
  uint32_t flags = 0;   // kRmapKeyFlags only

  // Key order: physical start, owner, then packed offset. The unwritten bit
  // is never part of a key.
  int Compare(const RmapKey& other) const;
  bool operator<(const RmapKey& other) const { return Compare(other) < 0; }
  bool operator==(const RmapKey& other) const { return Compare(other) == 0; }
};

struct RmapRecord {
  RmapKey key;
  uint32_t blockcount = 0;  // nonzero, within one AG

  uint64_t EndBlock() const {
    return static_cast<uint64_t>(key.startblock) + blockcount;
  }
  bool unwritten() const { return (key.flags & kRmapUnwritten) != 0; }
};

// A fully verified per-AG tree. `records` is in on-disk key order.
struct RmapTree {
  uint32_t agno = 0;
  uint32_t root = 0;
  uint32_t level = 0;      // AGF height, 1 = leaf root
  uint32_t blocks = 0;     // reachable blocks including the root
  std::vector<RmapRecord> records;

  // Exact key lookup. EndBlock/blockcount are intentionally not part of the
  // key; see docs/RMAPBT.md.
  Result<RmapRecord> LookupEq(const RmapKey& key) const;
  // Largest key <= `key` (XFS_LOOKUP_LE).
  Result<RmapRecord> LookupLE(const RmapKey& key) const;
  // Smallest key >= `key` (XFS_LOOKUP_GE).
  Result<RmapRecord> LookupGE(const RmapKey& key) const;
  // Last key strictly < `key`; useful for predecessor searches.
  Result<RmapRecord> Predecessor(const RmapKey& key) const;
  // First key strictly > `key`.
  Result<RmapRecord> Successor(const RmapKey& key) const;
};

// Validates one record's physical range, owner encoding and flags against the
// filesystem geometry.  This is the same check the reader applies; the writer
// uses it before every logical mutation so a refused edit cannot be published.
Status ValidateRmapRecord(const RmapRecord& rec, const XfsGeometry& geo,
                          uint32_t agno);

// xfs_rmapbt_init_high_key_from_rec: start+len-1, plus the logical offset for
// inode data/attr extents (never for metadata or bmbt records).  The unwritten
// attribute is not part of a key.
RmapKey RmapRecordHighKey(const RmapRecord& rec);

// Reads and completely verifies AG `agno`'s rmapbt: block magic/CRC/UUID/
// blkno/header owner/level/numrecs, sibling links, both low and high child
// keys, key ordering, every record's bounds/owner/flags, cycles, root level
// and the AGF rmap_blocks counter. `sb_lsn` is the superblock LSN for the
// metadata-LSN rule; pass 0 to skip only that time check (identity is still
// enforced). A filesystem without RMAPBT returns kUnsupportedFeature.
Result<RmapTree> LoadRmapTree(BlockDevice& device, const XfsGeometry& geo,
                              uint32_t agno, uint64_t sb_lsn = 0);

// Human-readable owner name for diagnostics (`OWN_AG`, `OWN_INODES`, inode).
std::string RmapOwnerName(uint64_t owner);

}  // namespace xfs
