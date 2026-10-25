// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - typed reference-count btree (refcountbt) reader.
//
// The on-disk rules follow the local Linux/xfsprogs trees (xfs_format.h,
// xfs_refcount.c, xfs_refcount_btree.c and repair/scan.c); see
// docs/REFLINK.md.  The btree stores only two domains:
//
//   shared file data : encoded startblock = agbno, refcount >= 2
//   CoW staging      : encoded startblock = agbno | (1u << 31), refcount == 1
//
// Every other allocation (metadata, attributes, unwritten data, free space) is
// implied refcount 1 and has no record.  Records are ordered by the *encoded*
// startblock, so all CoW staging records sort after the shared records.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/superblock.h"

namespace xfs {

constexpr uint32_t kRefcountCowFlag = 1u << 31;
constexpr uint32_t kRefcountAgBlockMask = ~kRefcountCowFlag;
constexpr uint32_t kRefcountMax = 0xffffffffu;
constexpr uint32_t kRefcountLenMax = 0xffffffffu;

enum class RefcountDomain {
  kShared = 0,  // ordinary shared written file data
  kCow = 1,     // CoW staging extent, refcount == 1
};

const char* ToString(RefcountDomain domain);

// A record's identity is (domain, physical startblock).  `startblock` is the
// AG-relative physical block with the CoW flag removed.
struct RefcountKey {
  RefcountDomain domain = RefcountDomain::kShared;
  uint32_t startblock = 0;

  uint32_t Encoded() const {
    return startblock | (domain == RefcountDomain::kCow ? kRefcountCowFlag : 0);
  }
  int Compare(const RefcountKey& other) const;
  bool operator<(const RefcountKey& other) const { return Compare(other) < 0; }
  bool operator==(const RefcountKey& other) const {
    return Compare(other) == 0;
  }
};

struct RefcountRecord {
  RefcountKey key;
  uint32_t blockcount = 0;
  uint32_t refcount = 0;

  uint64_t EndBlock() const {
    return static_cast<uint64_t>(key.startblock) + blockcount;
  }
  bool cow() const { return key.domain == RefcountDomain::kCow; }
};

// A fully verified per-AG tree. `records` is in on-disk encoded-key order.
struct RefcountTree {
  uint32_t agno = 0;
  uint32_t root = 0;
  uint32_t level = 0;   // AGF height, 1 = leaf root
  uint32_t blocks = 0;  // reachable blocks including the root
  std::vector<RefcountRecord> records;

  // Exact key lookup, LE/GE on the encoded key.
  Result<RefcountRecord> LookupEq(const RefcountKey& key) const;
  Result<RefcountRecord> LookupLE(const RefcountKey& key) const;
  Result<RefcountRecord> LookupGE(const RefcountKey& key) const;
  // Last record starting strictly before `key`.
  Result<RefcountRecord> Predecessor(const RefcountKey& key) const;
  // First record starting strictly after `key`.
  Result<RefcountRecord> Successor(const RefcountKey& key) const;

  // True when any shared-data record physically overlaps [agbno, agbno+len).
  bool OverlapsShared(uint32_t agbno, uint32_t len) const;
  // First shared record run intersecting [agbno, agbno+len), trimmed to the
  // query.  This is xfs_refcount_find_shared with find_end_of_shared=false.
  bool FindShared(uint32_t agbno, uint32_t len, RefcountRecord* out) const;
  // First CoW staging record run intersecting the query.
  bool FindCow(uint32_t agbno, uint32_t len, RefcountRecord* out) const;
};

// Validates one record against the AG geometry and the domain/refcount rules.
Status ValidateRefcountRecord(const RefcountRecord& rec,
                              const XfsGeometry& geo, uint32_t agno);

// Reads and completely verifies AG `agno`'s refcountbt: R3FC magic, CRC,
// UUID, dblkno, header owner, level/numrecs, sibling links, child keys,
// encoded-key ordering, physical non-overlap, domain/refcount rules and the
// AGF refcount root/level/block counters.
Result<RefcountTree> LoadRefcountTree(BlockDevice& device,
                                      const XfsGeometry& geo, uint32_t agno,
                                      uint64_t sb_lsn = 0);

// Human-readable domain description for diagnostics.
std::string RefcountDomainName(RefcountDomain domain);

}  // namespace xfs
