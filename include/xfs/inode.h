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
  uint64_t nblocks = 0;
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
Status ParseInode(const uint8_t* buf, size_t len, const XfsGeometry& geo,
                  uint64_t ino, XfsInode* out);

Result<XfsInode> ReadInode(BlockDevice& dev, const XfsGeometry& geo,
                           uint64_t ino);

// Decodes an on-disk timestamp field.
XfsTimestamp DecodeTimestamp(const uint8_t* field, bool bigtime);

}  // namespace xfs
