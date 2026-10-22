// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/ag.h"

#include <vector>

#include "xfs/endian.h"
#include "xfs/log.h"
#include "xfs/ondisk.h"
#include "xfs/verifier.h"

namespace xfs {
namespace {

namespace agf = ondisk::agf;
namespace agi = ondisk::agi;
namespace agfl = ondisk::agfl;

constexpr uint32_t kNullAgBlock = 0xffffffffu;

}  // namespace

Result<XfsAgf> ReadAgf(BlockDevice& dev, const XfsGeometry& geo, uint32_t agno) {
  if (agno >= geo.agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("AGF");
  }
  const uint64_t off = geo.AgByteOffset(agno) + AgfByteOffset(geo);
  std::vector<uint8_t> buf(geo.sector_size);
  if (!dev.Read(off, buf.data(), buf.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read AGF")
        .Ag(agno)
        .Offset(off)
        .Metadata("AGF");
  }

  // AGF layout: magic 0, version 4, seqno 8, length 12, ...
  V5MetaCheck check;
  check.type = "AGF";
  check.magic_offset = agf::kMagic;
  check.magic = ondisk::kAgfMagic;
  check.crc_offset = agf::kCrc;
  check.uuid_offset = agf::kUuid;
  check.lsn_offset = agf::kLsn;
  Status s = VerifyV5Metadata(buf.data(), buf.size(), check, off / 512,
                              /*owner*/ 0, geo.meta_uuid, /*sb_lsn*/ 0);
  if (!s.ok()) return s.error();

  XfsAgf out;
  out.seqno = GetBE32(buf.data(), agf::kSeqno);
  out.length = GetBE32(buf.data(), agf::kLength);
  out.bno_root = GetBE32(buf.data(), agf::kBnoRoot);
  out.cnt_root = GetBE32(buf.data(), agf::kCntRoot);
  out.rmap_root = GetBE32(buf.data(), agf::kRmapRoot);
  out.bno_level = GetBE32(buf.data(), agf::kBnoLevel);
  out.cnt_level = GetBE32(buf.data(), agf::kCntLevel);
  out.rmap_level = GetBE32(buf.data(), agf::kRmapLevel);
  out.flfirst = GetBE32(buf.data(), agf::kFlfirst);
  out.fllast = GetBE32(buf.data(), agf::kFllast);
  out.flcount = GetBE32(buf.data(), agf::kFlcount);
  out.freeblks = GetBE32(buf.data(), agf::kFreeblks);
  out.longest = GetBE32(buf.data(), agf::kLongest);
  out.btreeblks = GetBE32(buf.data(), agf::kBtreeblks);
  out.rmap_blocks = GetBE32(buf.data(), agf::kRmapBlocks);
  out.refcount_blocks = GetBE32(buf.data(), agf::kRefcountBlocks);
  out.refcount_root = GetBE32(buf.data(), agf::kRefcountRoot);
  out.refcount_level = GetBE32(buf.data(), agf::kRefcountLevel);
  out.lsn = GetBE64(buf.data(), agf::kLsn);

  uint32_t version = GetBE32(buf.data(), agf::kVersionnum);
  if (version != agf::kVersion) {
    return XFS_ERR(XfsError::kBadVersion, "AGF version mismatch")
        .Ag(agno)
        .Metadata("AGF")
        .Expected(agf::kVersion)
        .Actual(version);
  }
  if (out.seqno != agno) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF seqno mismatch")
        .Ag(agno)
        .Metadata("AGF")
        .Expected(agno)
        .Actual(out.seqno);
  }
  uint32_t expected_length = geo.AgBlocksFor(agno);
  if (out.length != expected_length) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF length mismatch")
        .Ag(agno)
        .Metadata("AGF")
        .Expected(expected_length)
        .Actual(out.length);
  }
  if (!geo.AgbnoValid(agno, out.bno_root) || !geo.AgbnoValid(agno, out.cnt_root)) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF btree root out of range")
        .Ag(agno)
        .Metadata("AGF")
        .Actual(out.bno_root);
  }
  uint32_t agfl_size = AgflSize(geo);
  if (out.flcount > agfl_size) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF freelist count too large")
        .Ag(agno)
        .Metadata("AGF")
        .Expected(agfl_size)
        .Actual(out.flcount);
  }
  // The kernel verifier only bounds-checks the AGFL ring indices; the
  // kernel itself creates flfirst=1/flast=0 for an empty freelist, so no
  // ring-consistency rule is imposed here.
  if (out.flfirst >= agfl_size || out.fllast >= agfl_size) {
    return XFS_ERR(XfsError::kCorruptAgHeader,
                   "AGF freelist index out of range")
        .Ag(agno)
        .Metadata("AGF")
        .Actual(out.flfirst);
  }
  if (out.freeblks > out.length) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF freeblks exceeds AG length")
        .Ag(agno)
        .Metadata("AGF")
        .Actual(out.freeblks);
  }
  if (out.longest > out.freeblks) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF longest exceeds freeblks")
        .Ag(agno)
        .Metadata("AGF")
        .Actual(out.longest);
  }
  if (out.btreeblks > out.length) {
    return XFS_ERR(XfsError::kCorruptAgHeader,
                   "AGF btreeblks exceeds AG length")
        .Ag(agno)
        .Metadata("AGF")
        .Actual(out.btreeblks);
  }
  if (out.bno_level < 1 || out.cnt_level < 1 ||
      out.bno_level > ondisk::btree::kMaxTraversalLevels ||
      out.cnt_level > ondisk::btree::kMaxTraversalLevels) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGF btree level out of range")
        .Ag(agno)
        .Metadata("AGF")
        .Actual(out.bno_level);
  }
  return out;
}

Result<XfsAgi> ReadAgi(BlockDevice& dev, const XfsGeometry& geo, uint32_t agno) {
  if (agno >= geo.agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("AGI");
  }
  const uint64_t off = geo.AgByteOffset(agno) + AgiByteOffset(geo);
  std::vector<uint8_t> buf(geo.sector_size);
  if (!dev.Read(off, buf.data(), buf.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read AGI")
        .Ag(agno)
        .Offset(off)
        .Metadata("AGI");
  }

  V5MetaCheck check;
  check.type = "AGI";
  check.magic_offset = agi::kMagic;
  check.magic = ondisk::kAgiMagic;
  check.crc_offset = agi::kCrc;
  check.uuid_offset = agi::kUuid;
  check.lsn_offset = agi::kLsn;
  Status s = VerifyV5Metadata(buf.data(), buf.size(), check, off / 512, 0,
                              geo.meta_uuid, 0);
  if (!s.ok()) return s.error();

  XfsAgi out;
  out.seqno = GetBE32(buf.data(), agi::kSeqno);
  out.length = GetBE32(buf.data(), agi::kLength);
  out.count = GetBE32(buf.data(), agi::kCount);
  out.root = GetBE32(buf.data(), agi::kRoot);
  out.level = GetBE32(buf.data(), agi::kLevel);
  out.freecount = GetBE32(buf.data(), agi::kFreecount);
  out.newino = GetBE32(buf.data(), agi::kNewino);
  out.dirino = GetBE32(buf.data(), agi::kDirino);
  for (size_t i = 0; i < agi::kUnlinkedBuckets; ++i) {
    out.unlinked[i] = GetBE32(buf.data(), agi::kUnlinked + i * 4);
  }
  out.free_root = GetBE32(buf.data(), agi::kFreeRoot);
  out.free_level = GetBE32(buf.data(), agi::kFreeLevel);
  out.iblocks = GetBE32(buf.data(), agi::kIblocks);
  out.fblocks = GetBE32(buf.data(), agi::kFblocks);
  out.lsn = GetBE64(buf.data(), agi::kLsn);

  uint32_t version = GetBE32(buf.data(), agi::kVersionnum);
  if (version != agi::kVersion) {
    return XFS_ERR(XfsError::kBadVersion, "AGI version mismatch")
        .Ag(agno)
        .Metadata("AGI")
        .Expected(agi::kVersion)
        .Actual(version);
  }
  if (out.seqno != agno) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGI seqno mismatch")
        .Ag(agno)
        .Metadata("AGI")
        .Expected(agno)
        .Actual(out.seqno);
  }
  uint32_t expected_length = geo.AgBlocksFor(agno);
  if (out.length != expected_length) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGI length mismatch")
        .Ag(agno)
        .Metadata("AGI")
        .Expected(expected_length)
        .Actual(out.length);
  }
  uint64_t max_inodes = static_cast<uint64_t>(out.length) * geo.inopblock;
  if (out.count > max_inodes || out.freecount > out.count) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGI inode counts invalid")
        .Ag(agno)
        .Metadata("AGI")
        .Expected(max_inodes)
        .Actual(out.count);
  }
  if (out.level == 0 || out.level > ondisk::btree::kMaxTraversalLevels ||
      out.free_level > ondisk::btree::kMaxTraversalLevels) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGI btree level out of range")
        .Ag(agno)
        .Metadata("AGI")
        .Actual(out.level);
  }
  if (!geo.AgbnoValid(agno, out.root)) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGI inobt root out of range")
        .Ag(agno)
        .Metadata("AGI")
        .Actual(out.root);
  }
  if (geo.features.finobt) {
    if (out.free_level == 0 || !geo.AgbnoValid(agno, out.free_root)) {
      return XFS_ERR(XfsError::kCorruptAgHeader,
                     "AGI finobt root out of range or level zero")
          .Ag(agno)
          .Metadata("AGI")
          .Actual(out.free_root);
    }
  } else if (out.free_level != 0) {
    return XFS_ERR(XfsError::kCorruptAgHeader,
                   "AGI finobt root set without the finobt feature")
        .Ag(agno)
        .Metadata("AGI");
  }
  return out;
}

Result<XfsAgfl> ReadAgfl(BlockDevice& dev, const XfsGeometry& geo, uint32_t agno,
                         uint32_t flcount) {
  if (agno >= geo.agcount) {
    return XFS_ERR(XfsError::kOutOfRange, "AG number out of range")
        .Ag(agno)
        .Metadata("AGFL");
  }
  uint32_t agfl_size = AgflSize(geo);
  if (flcount > agfl_size) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGFL count too large")
        .Ag(agno)
        .Metadata("AGFL")
        .Expected(agfl_size)
        .Actual(flcount);
  }
  const uint64_t off = geo.AgByteOffset(agno) + AgflByteOffset(geo);
  std::vector<uint8_t> buf(geo.sector_size);
  if (!dev.Read(off, buf.data(), buf.size())) {
    return XFS_ERR(XfsError::kIoError, "failed to read AGFL")
        .Ag(agno)
        .Offset(off)
        .Metadata("AGFL");
  }

  V5MetaCheck check;
  check.type = "AGFL";
  check.magic_offset = agfl::kMagic;
  check.magic = ondisk::kAgflMagic;
  check.crc_offset = agfl::kCrc;
  check.uuid_offset = agfl::kUuid;
  check.lsn_offset = agfl::kLsn;
  Status s = VerifyV5Metadata(buf.data(), buf.size(), check, off / 512, 0,
                              geo.meta_uuid, 0);
  if (!s.ok()) return s.error();

  XfsAgfl out;
  out.seqno = GetBE32(buf.data(), agfl::kSeqno);
  if (out.seqno != agno) {
    return XFS_ERR(XfsError::kCorruptAgHeader, "AGFL seqno mismatch")
        .Ag(agno)
        .Metadata("AGFL")
        .Expected(agno)
        .Actual(out.seqno);
  }
  out.entries.reserve(flcount);
  for (uint32_t i = 0; i < flcount; ++i) {
    uint32_t agbno = GetBE32(buf.data(), kAgflHeaderSize + i * 4);
    if (agbno != kNullAgBlock && !geo.AgbnoValid(agno, agbno)) {
      return XFS_ERR(XfsError::kCorruptAgHeader,
                     "AGFL entry out of AG range")
          .Ag(agno)
          .Metadata("AGFL")
          .Actual(agbno);
    }
    out.entries.push_back(agbno);
  }
  return out;
}

Result<XfsAgHeaders> ReadAgHeaders(BlockDevice& dev, const XfsGeometry& geo,
                                   uint32_t agno) {
  auto agf = ReadAgf(dev, geo, agno);
  if (!agf.ok()) return agf.error();
  auto agi = ReadAgi(dev, geo, agno);
  if (!agi.ok()) return agi.error();
  XfsAgHeaders out;
  out.agf = std::move(agf.value());
  out.agi = std::move(agi.value());
  return out;
}

}  // namespace xfs
