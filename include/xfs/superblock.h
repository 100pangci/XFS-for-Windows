// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - superblock parsing, verification and geometry.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "xfs/error.h"
#include "xfs/block_device.h"
#include "xfs/uuid.h"

namespace xfs {

struct XfsFeatures {
  // incompat (supported by this build)
  bool ftype = false;
  bool spinodes = false;
  bool meta_uuid = false;
  bool bigtime = false;
  bool nrext64 = false;
  bool exchrange = false;
  // Parent pointers only add per-entry parent xattrs in the attribute fork;
  // the directory and inode layouts are unchanged, so read-only access is safe.
  bool parent = false;
  // ro-compat (safe to read, ignored on write)
  bool finobt = false;
  bool rmapbt = false;
  bool reflink = false;
  bool inobtcnt = false;
  // unknown bits
  uint32_t compat_unknown = 0;
  uint32_t ro_compat_unknown = 0;
  uint32_t incompat_unknown = 0;
  // v4 feature area bits we care about
  bool asciici = false;
};

// Parsed, host-endian superblock.
struct XfsSuperblock {
  uint32_t magic = 0;
  uint32_t blocksize = 0;
  uint64_t dblocks = 0;
  uint64_t rblocks = 0;
  uint64_t rextents = 0;
  Uuid uuid{};
  uint64_t logstart = 0;
  uint64_t rootino = 0;
  uint64_t rbmino = 0;
  uint64_t rsumino = 0;
  uint32_t rextsize = 0;
  uint32_t agblocks = 0;
  uint32_t agcount = 0;
  uint32_t rbmblocks = 0;
  uint32_t logblocks = 0;
  uint16_t versionnum = 0;
  uint16_t sectsize = 0;
  uint16_t inodesize = 0;
  uint16_t inopblock = 0;
  std::array<char, 13> label{};  // NUL terminated view of sb_fname
  uint8_t blocklog = 0;
  uint8_t sectlog = 0;
  uint8_t inodelog = 0;
  uint8_t inopblog = 0;
  uint8_t agblklog = 0;
  uint8_t rextslog = 0;
  uint8_t inprogress = 0;
  uint8_t imax_pct = 0;
  uint64_t icount = 0;
  uint64_t ifree = 0;
  uint64_t fdblocks = 0;
  uint64_t frextents = 0;
  uint64_t uquotino = 0;
  uint64_t gquotino = 0;
  uint64_t pquotino = 0;
  uint16_t qflags = 0;
  uint8_t flags = 0;
  uint8_t shared_vn = 0;
  uint32_t inoalignmt = 0;
  uint32_t unit = 0;
  uint32_t width = 0;
  uint8_t dirblklog = 0;
  uint8_t logsectlog = 0;
  uint16_t logsectsize = 0;
  uint32_t logsunit = 0;
  uint32_t features2 = 0;
  uint32_t features_compat = 0;
  uint32_t features_ro_compat = 0;
  uint32_t features_incompat = 0;
  uint32_t features_log_incompat = 0;
  uint32_t spino_align = 0;
  uint64_t lsn = 0;
  Uuid meta_uuid{};

  XfsFeatures features;

  // Which checks passed during parsing (for reporting).
  bool crc_verified = false;
  bool is_secondary = false;

  // Convenience accessors
  bool IsV5() const { return (versionnum & 0x000f) == 5; }
};

std::string FormatLabel(const XfsSuperblock& sb);

// Host-side geometry derived from the superblock; the single source of truth
// for all block/inode number arithmetic in the core.
struct XfsGeometry {
  uint32_t block_size = 0;
  uint32_t sector_size = 0;
  uint32_t inode_size = 0;
  uint32_t inopblock = 0;

  uint8_t block_log = 0;
  uint8_t sector_log = 0;
  uint8_t inode_log = 0;
  uint8_t inopblog = 0;
  uint8_t agblklog = 0;
  uint8_t agno_log = 0;
  uint8_t agino_log = 0;

  uint32_t agcount = 0;
  uint32_t agblocks = 0;
  uint64_t dblocks = 0;

  uint64_t logstart = 0;   // fs block
  uint32_t logblocks = 0;  // fs blocks
  uint64_t rootino = 0;

  uint32_t dir_blk_log = 0;
  uint32_t dir_block_size = 0;

  Uuid uuid{};
  Uuid meta_uuid{};
  XfsFeatures features;
  uint32_t ino_align = 0;

  // ---- block arithmetic ----
  // NOTE: XFS fsblock numbers encode the AG number in the high bits:
  //   fsb = (agno << agblklog) | agbno
  // The device byte offset is CONTIGUOUS: agno * agblocks + agbno. These two
  // differ when agblocks is not a power of two, so fsb -> byte must decode
  // the AG number and agbno first.
  uint64_t FsbToByte(uint64_t fsb) const {
    const uint32_t agno = static_cast<uint32_t>(fsb >> agblklog);
    const uint32_t agbno = static_cast<uint32_t>(fsb & ((1ULL << agblklog) - 1));
    return (static_cast<uint64_t>(agno) * agblocks + agbno) << block_log;
  }
  uint64_t AgByteOffset(uint32_t agno) const {
    return static_cast<uint64_t>(agno) * agblocks << block_log;
  }
  bool FsbValid(uint64_t fsb) const {
    const uint32_t agno = static_cast<uint32_t>(fsb >> agblklog);
    if (agno >= agcount) return false;
    const uint32_t agbno = static_cast<uint32_t>(fsb & ((1ULL << agblklog) - 1));
    return agbno < AgBlocksFor(agno);
  }
  // True when [fsb, fsb+count) lies entirely within one AG.
  bool FsbRangeValid(uint64_t fsb, uint64_t count) const {
    const uint32_t agno = static_cast<uint32_t>(fsb >> agblklog);
    if (agno >= agcount) return false;
    const uint32_t agbno = static_cast<uint32_t>(fsb & ((1ULL << agblklog) - 1));
    const uint64_t aglen = AgBlocksFor(agno);
    return agbno < aglen && count <= aglen - agbno;
  }
  uint64_t MaxFsb() const { return FsbOf(agcount - 1, AgBlocksFor(agcount - 1) - 1); }
  uint64_t AgFirstFsb(uint32_t agno) const {
    return static_cast<uint64_t>(agno) << agblklog;
  }
  uint64_t FsbOf(uint32_t agno, uint32_t agbno) const {
    return AgFirstFsb(agno) | agbno;
  }
  // Length of an AG in blocks (the last AG may be shorter).
  uint32_t AgBlocksFor(uint32_t agno) const {
    uint64_t first = static_cast<uint64_t>(agno) * agblocks;
    if (first >= dblocks) return 0;
    uint64_t remaining = dblocks - first;
    return static_cast<uint32_t>(remaining < agblocks ? remaining : agblocks);
  }
  bool AgbnoValid(uint32_t agno, uint32_t agbno) const {
    return agno < agcount && agbno < AgBlocksFor(agno);
  }
  uint64_t AgFsb(uint32_t agno, uint32_t agbno) const {
    return AgFirstFsb(agno) | agbno;
  }

  // ---- inode arithmetic ----
  // ino = (agno << agino_log) | (agbno << inopblog) | offset
  uint32_t InoAgno(uint64_t ino) const {
    return static_cast<uint32_t>(ino >> agino_log);
  }
  uint32_t InoAgino(uint64_t ino) const {
    const uint64_t mask = (1ULL << agino_log) - 1;
    return static_cast<uint32_t>(ino & mask);
  }
  uint32_t InoAgbno(uint64_t ino) const {
    const uint64_t mask = (1ULL << agblklog) - 1;
    return static_cast<uint32_t>((ino >> inopblog) & mask);
  }
  uint32_t InoOffset(uint64_t ino) const {
    return static_cast<uint32_t>(ino & ((1U << inopblog) - 1));
  }
  uint64_t AginoToIno(uint32_t agno, uint32_t agino) const {
    return (static_cast<uint64_t>(agno) << agino_log) | agino;
  }
  bool InoValid(uint64_t ino) const {
    if (ino == 0 || ino > ((1ULL << (agno_log + agino_log)) - 1)) return false;
    uint32_t agno = InoAgno(ino);
    if (agno >= agcount) return false;
    uint32_t agbno = InoAgbno(ino);
    return agbno < AgBlocksFor(agno);
  }
  uint64_t InoToFsb(uint64_t ino) const {
    return AgFsb(InoAgno(ino), InoAgbno(ino));
  }
  uint64_t InoToByteOffset(uint64_t ino) const {
    return FsbToByte(InoToFsb(ino)) +
           static_cast<uint64_t>(InoOffset(ino)) * inode_size;
  }
};

// Parses + verifies a superblock image of `len` bytes (must be >= sectsize).
// `verify_crc` controls CRC checking (off for repair-style inspection only).
Status ParseSuperblockBuffer(const uint8_t* buf, size_t len, bool verify_crc,
                             XfsSuperblock* out);

// Reads and verifies the superblock at `byte_offset` (agcount copies exist:
// byte_offset = agno << (blocklog+agblklog) once geometry is known).
Result<XfsSuperblock> ReadSuperblock(BlockDevice& dev, uint64_t byte_offset,
                                     bool verify_crc);

// Builds geometry from a parsed superblock.
XfsGeometry MakeGeometry(const XfsSuperblock& sb);

// Mount policy for v1: v5 only, internal log, no realtime, only known
// features. On failure the returned status explains exactly what is
// unsupported (never "try to read anyway").
Status CheckMountable(const XfsSuperblock& sb);

// True if the log-incompat feature mask is nonzero, meaning the log may hold
// records this build cannot interpret (clean unmount clears the mask).
bool HasLogIncompatFeatures(const XfsSuperblock& sb);

}  // namespace xfs
