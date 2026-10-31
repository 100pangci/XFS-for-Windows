// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - v3 inode parsing and verification.
#pragma once

#include <cstdint>
#include <vector>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/superblock.h"

namespace xfs {

// POSIX mode bits (inode di_mode).
constexpr uint16_t kSIfmt = 0xF000;
constexpr uint16_t kSIfreg = 0x8000;
constexpr uint16_t kSIfdir = 0x4000;
constexpr uint16_t kSIfchr = 0x2000;
constexpr uint16_t kSIfblk = 0x6000;
constexpr uint16_t kSIfifo = 0x1000;
constexpr uint16_t kSIfsock = 0xC000;
constexpr uint16_t kSIflnk = 0xA000;

struct XfsTimestamp {
  int64_t sec = 0;       // Unix epoch seconds (may be negative)
  uint32_t nsec = 0;     // 0..999999999
};

// Timestamp ranges from libxfs/xfs_format.h.
constexpr int64_t kLegacyTimeMin = INT32_MIN;
constexpr int64_t kLegacyTimeMax = INT32_MAX;
constexpr int64_t kBigtimeEpochOffset = -(int64_t)INT32_MIN;  // 2^31
constexpr uint64_t kNsecPerSec = 1000000000ULL;
// Largest on-disk bigtime seconds value (XFS_BIGTIME_TIME_MAX is rounded down
// to a multiple of 4 so quota and inode limits agree).
constexpr uint64_t kBigtimeMaxSeconds = (~0ULL / kNsecPerSec) & ~0x3ULL;
constexpr uint64_t kBigtimeTimeMax = kBigtimeMaxSeconds * kNsecPerSec;
constexpr int64_t kBigtimeTimeMaxUnix =
    static_cast<int64_t>(kBigtimeMaxSeconds) - kBigtimeEpochOffset;
constexpr int64_t kBigtimeTimeMinUnix = -kBigtimeEpochOffset;

// Parsed inode. Fork data is copied out of the on-disk buffer so an XfsInode
// is self-contained and can outlive the read buffer.
struct XfsInode {
  uint64_t ino = 0;
  uint16_t mode = 0;
  uint8_t version = 0;
  uint8_t format = 0;   // data fork format (XFS_DINODE_FMT_*)
  uint8_t aformat = 0;  // attr fork format
  uint32_t uid = 0;
  uint32_t gid = 0;
  uint32_t projid = 0;
  uint32_t nlink = 0;
  uint32_t gen = 0;
  uint64_t size = 0;
  uint64_t nblocks = 0;  // di_nblocks: filesystem blocks used
  uint32_t nextents = 0;
  uint32_t anextents = 0;
  uint32_t extsize = 0;      // raw di_extsize (fs blocks if EXTSIZE flag)
  uint32_t cowextsize = 0;
  uint16_t flags = 0;
  uint64_t flags2 = 0;
  uint32_t next_unlinked = 0;
  uint32_t dmevmask = 0;
  uint16_t dmstate = 0;
  uint64_t changecount = 0;
  uint64_t lsn = 0;
  uint8_t forkoff = 0;  // raw di_forkoff (8-byte units)

  XfsTimestamp atime, mtime, ctime, crtime;

  bool bigtime = false;   // per-inode bigtime flag
  bool nrext64 = false;   // per-inode large extent counters

  // Runtime snapshot token, never on disk: the metadata-cache generation that
  // OpenInode observed when it produced this image. Readers use it to decide
  // whether a later parse may be published (see InsertDataChunk and
  // LoadExtentsCached). Zero for inodes built by tests/formatters.
  uint64_t cache_generation = 0;

  std::vector<uint8_t> data_fork;
  std::vector<uint8_t> attr_fork;

  bool IsDir() const { return (mode & kSIfmt) == kSIfdir; }
  bool IsRegular() const { return (mode & kSIfmt) == kSIfreg; }
  bool IsSymlink() const { return (mode & kSIfmt) == kSIflnk; }
  bool IsSpecial() const {
    uint16_t t = mode & kSIfmt;
    return t == kSIfchr || t == kSIfblk || t == kSIfifo || t == kSIfsock;
  }
  bool HasAttrFork() const { return forkoff != 0; }
  // 0xffffffff means inode is not on the unlinked list.
  bool OnUnlinkedList() const { return next_unlinked != 0xffffffff; }
};

// Parses + verifies one on-disk inode (v3 core, 176 bytes, plus forks).
// `allow_free_inode` accepts di_mode == 0 (a freed inode, xfs_difree zeroed the
// core): the kernel's xfs_dinode_verify only applies the mode/type, extent and
// fork checks when the mode is non-zero, and recovery replays freed inodes.
// Readers keep the default (a mode-0 inode is not a file).
Status ParseInode(const uint8_t* buf, size_t len, const XfsGeometry& geo,
                  uint64_t ino, XfsInode* out, bool allow_free_inode = false);

Result<XfsInode> ReadInode(BlockDevice& dev, const XfsGeometry& geo,
                           uint64_t ino);

// Decodes an on-disk timestamp field.
XfsTimestamp DecodeTimestamp(const uint8_t* field, bool bigtime);

// Encodes a timestamp into the on-disk (big-endian) representation. Fails
// with kOutOfRange when the filesystem's encoding cannot represent it.
Status EncodeTimestamp(uint8_t* field, const XfsTimestamp& ts, bool bigtime);

// True when the raw inode image has XFS_DIFLAG2_BIGTIME set.
bool InodeImageHasBigtime(const uint8_t* inode);
bool InodeImageHasNrext64(const uint8_t* inode);

// Stores one timestamp in a raw inode image, choosing the encoding from the
// inode's own di_flags2 (the kernel does the same in xfs_inode_to_disk_ts).
Status StoreInodeTimestamp(uint8_t* inode, size_t field_offset,
                           const XfsTimestamp& ts);

// Converts a legacy inode to bigtime in place when the filesystem supports
// bigtime: all four timestamps are decoded and re-encoded and
// XFS_DIFLAG2_BIGTIME is set. This is what xfs_inode_item_precommit does for
// any inode core/timestamp update on a bigtime filesystem. No-op otherwise.
Status UpgradeInodeTimestampsToBigtime(uint8_t* inode, size_t inode_size,
                                       const XfsGeometry& geo);

// Extent counters: the layout depends on XFS_DIFLAG2_NREXT64.
uint64_t InodeDataExtentCount(const uint8_t* inode);
uint32_t InodeAttrExtentCount(const uint8_t* inode);
void SetInodeDataExtentCount(uint8_t* inode, uint64_t count);
void SetInodeAttrExtentCount(uint8_t* inode, uint32_t count);

// Current wall-clock time, for metadata updates.
XfsTimestamp CurrentTimestamp();

}  // namespace xfs
