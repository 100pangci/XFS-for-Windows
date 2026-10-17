// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/btree.h"

#include <unordered_map>
#include <unordered_set>

#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/verifier.h"

namespace xfs {
namespace {

namespace bt = ondisk::btree;

size_t HeaderSize(const BtreeSpec& spec) {
  if (spec.format == BtreeFormat::kLong) return bt::kLHeaderLenV5;
  return bt::kSHeaderLenV5;
}

}  // namespace

size_t BtreeMaxRecords(const BtreeSpec& spec, bool leaf) {
  const size_t hdr = HeaderSize(spec);
  if (spec.block_size <= hdr) return 0;
  if (leaf) return (spec.block_size - hdr) / spec.rec_size;
  return (spec.block_size - hdr) / (spec.key_size + spec.ptr_size);
}

Status VerifyBtreeBlock(const uint8_t* buf, size_t len, const BtreeSpec& spec,
                        BtreeBlockMeta* out) {
  BtreeBlockMeta meta;
  meta.long_format = spec.format == BtreeFormat::kLong;
  meta.header_size = HeaderSize(spec);

  if (len < spec.block_size) {
    return XFS_ERR(XfsError::kCorruptBtree, "btree block buffer too small")
        .Metadata(spec.name)
        .Expected(spec.block_size)
        .Actual(len);
  }

  if (spec.verify_identity) {
    V5MetaCheck check;
    check.type = spec.name;
    check.magic_offset = bt::kMagic;
    check.magic = spec.magic;
    check.crc_offset =
        spec.format == BtreeFormat::kLong ? bt::kLCrc : bt::kSCrc;
    check.uuid_offset =
        spec.format == BtreeFormat::kLong ? bt::kLUuid : bt::kSUuid;
    check.blkno_offset =
        spec.format == BtreeFormat::kLong ? bt::kLBlkno : bt::kSBlkno;
    check.lsn_offset =
        spec.format == BtreeFormat::kLong ? bt::kLLsn : bt::kSLsn;
    if (spec.owner_size != 0) {
      check.owner_offset =
          spec.format == BtreeFormat::kLong ? bt::kLOwner : bt::kSOwner;
      check.owner_size = spec.owner_size;
    }
    Status s = VerifyV5Metadata(buf, spec.block_size, check,
                                spec.expected_daddr, spec.expected_owner,
                                spec.uuid, spec.sb_lsn);
    if (!s.ok()) return s;
  } else {
    Status s = VerifyMagic(buf, len, bt::kMagic, spec.magic, spec.name);
    if (!s.ok()) return s;
  }

  meta.magic = GetBE32(buf, bt::kMagic);
  meta.level = GetBE16(buf, bt::kLevel);
  meta.numrecs = GetBE16(buf, bt::kNumrecs);
  if (spec.format == BtreeFormat::kLong) {
    meta.leftsib = GetBE64(buf, bt::kLLeftsib);
    meta.rightsib = GetBE64(buf, bt::kLRightSib);
  } else {
    meta.leftsib = GetBE32(buf, bt::kSLeftsib);
    meta.rightsib = GetBE32(buf, bt::kSRightSib);
  }

  if (meta.level > spec.max_depth) {
    return XFS_ERR(XfsError::kCorruptBtree, "btree level exceeds maximum")
        .Metadata(spec.name)
        .Expected(spec.max_depth)
        .Actual(meta.level);
  }
  size_t maxrecs = BtreeMaxRecords(spec, meta.level == 0);
  if (meta.numrecs > maxrecs) {
    return XFS_ERR(XfsError::kCorruptBtree, "btree record count out of range")
        .Metadata(spec.name)
        .Expected(maxrecs)
        .Actual(meta.numrecs);
  }
  // A node with zero records is not a valid interior node (an empty tree has
  // a level-0 root), but empty leaf roots are legal (e.g. inobt of an AG
  // without inode chunks).
  if (meta.numrecs == 0 && meta.level != 0) {
    return XFS_ERR(XfsError::kCorruptBtree, "empty interior btree node")
        .Metadata(spec.name);
  }
  *out = meta;
  return OkStatus();
}

uint64_t BtreeChildPtr(const uint8_t* buf, const BtreeBlockMeta& meta,
                       const BtreeSpec& spec, size_t index) {
  size_t node_maxrecs = BtreeMaxRecords(spec, /*leaf=*/false);
  size_t ptrs_offset =
      meta.header_size + node_maxrecs * spec.key_size + index * spec.ptr_size;
  if (spec.format == BtreeFormat::kLong) {
    return GetBE64(buf, ptrs_offset);
  }
  return GetBE32(buf, ptrs_offset);
}

uint64_t BtreeKeyValue(const uint8_t* buf, const BtreeBlockMeta& meta,
                       const BtreeSpec& spec, size_t index) {
  size_t off = meta.header_size + index * spec.key_size;
  if (spec.key_size == 8) {
    return GetBE64(buf, off);
  }
  return GetBE32(buf, off);
}

const uint8_t* BtreeLeafRec(const uint8_t* buf, const BtreeBlockMeta& meta,
                            const BtreeSpec& spec, size_t index) {
  return buf + meta.header_size + index * spec.rec_size;
}

Status WalkBtree(const BtreeSpec& spec, uint64_t root,
                 const BtreeWalkCallbacks& callbacks) {
  if (!callbacks.read_block || !callbacks.visit) {
    return XFS_ERR(XfsError::kInternal, "btree walk missing callbacks")
        .Metadata(spec.name);
  }
  std::unordered_set<uint64_t> visited;
  std::unordered_map<uint16_t, uint64_t> last_block_at_level;

  // Explicit stack with expected child level (0xffff = root, any level).
  constexpr uint16_t kAnyLevel = 0xffff;
  struct Frame {
    uint64_t ptr;
    uint16_t depth;
    uint16_t expected_level;
  };
  std::vector<Frame> stack;
  stack.push_back({root, 0, kAnyLevel});

  while (!stack.empty()) {
    Frame frame = stack.back();
    stack.pop_back();

    if (frame.depth > spec.max_depth) {
      return XFS_ERR(XfsError::kCorruptBtree, "btree traversal too deep")
          .Metadata(spec.name)
          .Actual(frame.depth);
    }
    if (frame.ptr == spec.null_ptr) {
      return XFS_ERR(XfsError::kCorruptBtree, "null block pointer encountered")
          .Metadata(spec.name);
    }
    if (callbacks.ptr_in_range && !callbacks.ptr_in_range(frame.ptr)) {
      return XFS_ERR(XfsError::kCorruptBtree, "block pointer out of range")
          .Metadata(spec.name)
          .Actual(frame.ptr);
    }
    if (!visited.insert(frame.ptr).second) {
      return XFS_ERR(XfsError::kCorruptBtree, "btree cycle detected")
          .Metadata(spec.name)
          .Actual(frame.ptr);
    }
    if (visited.size() > (1u << 22)) {
      return XFS_ERR(XfsError::kCorruptBtree, "btree too large to walk")
          .Metadata(spec.name);
    }

    std::vector<uint8_t> buf;
    if (!callbacks.read_block(frame.ptr, &buf)) {
      return XFS_ERR(XfsError::kIoError, "failed to read btree block")
          .Metadata(spec.name)
          .Actual(frame.ptr);
    }

    BtreeSpec block_spec = spec;
    if (spec.ptr_to_daddr) {
      uint64_t daddr = 0;
      if (!spec.ptr_to_daddr(frame.ptr, &daddr)) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "cannot compute expected block address")
            .Metadata(spec.name)
            .Actual(frame.ptr);
      }
      block_spec.expected_daddr = daddr;
    }

    BtreeBlockMeta meta;
    Status s = VerifyBtreeBlock(buf.data(), buf.size(), block_spec, &meta);
    if (!s.ok()) {
      auto err = s.error();
      if (!err.metadata.size()) err.metadata = spec.name;
      return err;
    }
    if (frame.expected_level != kAnyLevel && meta.level != frame.expected_level) {
      return XFS_ERR(XfsError::kCorruptBtree,
                     "child block level does not match parent level - 1")
          .Metadata(spec.name)
          .Expected(frame.expected_level)
          .Actual(meta.level);
    }

    // Sibling coherence: blocks are visited in in-order; the previous block
    // at this level should be exactly leftsib.
    if (callbacks.check_siblings && meta.level > 0) {
      auto it = last_block_at_level.find(meta.level);
      if (it != last_block_at_level.end()) {
        if (meta.leftsib != it->second) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "left sibling pointer mismatch")
              .Metadata(spec.name)
              .Expected(it->second)
              .Actual(meta.leftsib);
        }
      } else {
        if (meta.leftsib != spec.null_ptr) {
          return XFS_ERR(XfsError::kCorruptBtree,
                         "first block at level has a left sibling")
              .Metadata(spec.name)
              .Actual(meta.leftsib);
        }
      }
      last_block_at_level[meta.level] = frame.ptr;
    }

    s = callbacks.visit(frame.ptr, meta, buf.data());
    if (!s.ok()) return s;

    if (meta.level == 0) continue;
    if (callbacks.descend && !callbacks.descend(meta.level)) continue;

    // Push children in reverse so they are processed left to right.
    for (size_t i = meta.numrecs; i > 0; --i) {
      uint64_t child = BtreeChildPtr(buf.data(), meta, spec, i - 1);
      stack.push_back(
          {child, static_cast<uint16_t>(frame.depth + 1),
           static_cast<uint16_t>(meta.level - 1)});
    }
  }
  return OkStatus();
}

Status WalkBtreePath(const BtreeSpec& spec, uint64_t root, uint64_t target_key,
                     const BtreeWalkCallbacks& callbacks) {
  uint64_t ptr = root;
  std::unordered_set<uint64_t> visited;
  for (uint16_t depth = 0; depth <= spec.max_depth; ++depth) {
    if (ptr == spec.null_ptr) {
      return XFS_ERR(XfsError::kCorruptBtree, "null block pointer")
          .Metadata(spec.name);
    }
    if (callbacks.ptr_in_range && !callbacks.ptr_in_range(ptr)) {
      return XFS_ERR(XfsError::kCorruptBtree, "block pointer out of range")
          .Metadata(spec.name)
          .Actual(ptr);
    }
    if (!visited.insert(ptr).second) {
      return XFS_ERR(XfsError::kCorruptBtree, "btree cycle detected")
          .Metadata(spec.name)
          .Actual(ptr);
    }
    std::vector<uint8_t> buf;
    if (!callbacks.read_block(ptr, &buf)) {
      return XFS_ERR(XfsError::kIoError, "failed to read btree block")
          .Metadata(spec.name)
          .Actual(ptr);
    }
    BtreeSpec block_spec = spec;
    if (spec.ptr_to_daddr) {
      uint64_t daddr = 0;
      if (!spec.ptr_to_daddr(ptr, &daddr)) {
        return XFS_ERR(XfsError::kCorruptBtree,
                       "cannot compute expected block address")
            .Metadata(spec.name)
            .Actual(ptr);
      }
      block_spec.expected_daddr = daddr;
    }
    BtreeBlockMeta meta;
    Status s = VerifyBtreeBlock(buf.data(), buf.size(), block_spec, &meta);
    if (!s.ok()) return s;
    s = callbacks.visit(ptr, meta, buf.data());
    if (!s.ok()) return s;
    if (meta.level == 0) return OkStatus();

    // Largest key <= target; keys order: keys[i] is the smallest key in child
    // subtree i (XFS convention for standard btrees).
    size_t chosen = 0;
    for (size_t i = 0; i < meta.numrecs; ++i) {
      if (BtreeKeyValue(buf.data(), meta, spec, i) <= target_key) {
        chosen = i;
      } else {
        break;
      }
    }
    ptr = BtreeChildPtr(buf.data(), meta, spec, chosen);
  }
  return XFS_ERR(XfsError::kCorruptBtree, "btree path exceeded maximum depth")
      .Metadata(spec.name);
}

}  // namespace xfs
