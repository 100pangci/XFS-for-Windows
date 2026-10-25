// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - read-write policy: which filesystems may be modified.
//
// The rule is simple: a filesystem may be mounted read-write only if this
// build can update *every* structure the filesystem uses. A feature that is
// understood for reading (finobt, rmapbt, reflink, ...) still requires
// corresponding updates on every allocation, inode change and directory
// change; doing a "best effort" update would corrupt the filesystem silently.
// So the check is an allow-list, and anything else is refused for read-write
// while read-only access continues to work.
#include "xfs/write.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "xfs/log.h"
#include "xfs/ondisk.h"

namespace xfs {
namespace {

std::string Hex32(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", v);
  return buf;
}

// Incompat features whose structures a writer must maintain.
struct IncompatReason {
  uint32_t bit;
  const char* name;
  const char* why;
};

constexpr IncompatReason kIncompatReasons[] = {
    // RW-6A implements sparse chunks, bigtime and NREXT64; they are no longer
    // in the refusal list. Parent pointers still change the attribute fork on
    // every rename and are a later milestone.
    {ondisk::kIncompatParent, "parent pointers",
     "parent pointer xattrs must be written on every rename"},
};

struct RoCompatReason {
  uint32_t bit;
  const char* name;
  const char* why;
};

constexpr RoCompatReason kRoCompatReasons[] = {
    // RW-6A maintains finobt and the AGI btree block counters.  RW-6B has the
    // rmapbt writer.  RW-6C has the refcountbt/CoW/reflink writer, opened for
    // ordinary read-write after all twelve qualification gates passed; both
    // features are skipped in this loop below.
    {ondisk::kRoCompatRmapbt, "rmapbt",
     "the reverse mapping btree must track every allocation"},
    {ondisk::kRoCompatReflink, "reflink",
     "the refcount btree must track every shared extent"},
};

}  // namespace

Status CheckWritableProfile(const XfsSuperblock& sb, const XfsGeometry& geo,
                            bool allow_rmapbt_recovery,
                            bool allow_rmapbt_qualification,
                            bool allow_reflink_recovery,
                            bool allow_reflink_qualification) {
  // The old hidden qualification/recovery overrides are retained as accepted
  // no-ops so existing scripts keep working, but the ordinary allow-list below
  // is now the only gate: reflink is part of the writable profile.
  (void)allow_rmapbt_recovery;
  (void)allow_rmapbt_qualification;
  (void)allow_reflink_recovery;
  (void)allow_reflink_qualification;
  if (sb.features.reflink && geo.agblocks >= (1u << 31)) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "reflink requires allocation groups smaller than 2^31 "
                   "blocks")
        .Metadata("superblock")
        .Actual(geo.agblocks);
  }
  if (sb.features.incompat_unknown) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "unknown incompatible features " +
                       Hex32(sb.features.incompat_unknown) +
                       "; a writer cannot keep them consistent")
        .Metadata("superblock")
        .Actual(sb.features.incompat_unknown);
  }
  if (sb.features_incompat &
      (ondisk::kIncompatMetadir | ondisk::kIncompatZoned |
       ondisk::kIncompatZoneGaps | ondisk::kIncompatNeedsRepair)) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "feature flags forbid modification "
                   "(metadir/zoned/needsrepair)")
        .Metadata("superblock")
        .Actual(sb.features_incompat);
  }

  for (const auto& reason : kIncompatReasons) {
    if (sb.features_incompat & reason.bit) {
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     std::string("incompatible feature '") + reason.name +
                         "' is read-only in this build: " + reason.why)
          .Metadata("superblock")
          .Actual(reason.bit);
    }
  }
  for (const auto& reason : kRoCompatReasons) {
    if (sb.features_ro_compat & reason.bit) {
      // RW-6B maintains rmapbt; RW-6C maintains the refcount btree and the
      // shared-extent lifecycle.  Both are now ordinary writable features.
      if (reason.bit == ondisk::kRoCompatRmapbt ||
          reason.bit == ondisk::kRoCompatReflink) {
        continue;
      }
      return XFS_ERR(XfsError::kUnsupportedFeature,
                     std::string("read-only compatible feature '") +
                         reason.name +
                         "' is read-only in this build: " + reason.why)
          .Metadata("superblock")
          .Actual(reason.bit);
    }
  }
  if (sb.features_ro_compat & ~ondisk::kRoCompatKnown) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "unknown read-only compatible features " +
                       Hex32(sb.features.ro_compat_unknown) +
                       "; a writer cannot keep them consistent")
        .Metadata("superblock")
        .Actual(sb.features_ro_compat);
  }
  if (sb.features.compat_unknown) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "unknown compatible features " +
                       Hex32(sb.features.compat_unknown))
        .Metadata("superblock")
        .Actual(sb.features.compat_unknown);
  }
  if (sb.features.asciici) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "case-insensitive (asciici) directories are not supported")
        .Metadata("superblock");
  }

  // Realtime device: data would land on a second device this build cannot
  // address, and the rt bitmap/summary metadata would need updating.
  if (sb.rblocks != 0 || sb.rextents != 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "realtime devices are not supported for read-write")
        .Metadata("superblock")
        .Actual(sb.rblocks);
  }

  // Quota: the quota inodes and the on-disk dquot trees must be updated by
  // every allocation and inode change. No quota in use is encoded either as 0
  // (mkfs) or as NULLFSINO (all ones, what a mounted kernel writes back), so
  // only a real inode number means quota is enabled.
  constexpr uint64_t kNullFsino = 0xFFFFFFFFFFFFFFFFull;
  auto has_quota_inode = [](uint64_t ino) {
    return ino != 0 && ino != kNullFsino;
  };
  if (sb.qflags != 0 || has_quota_inode(sb.uquotino) ||
      has_quota_inode(sb.gquotino) || has_quota_inode(sb.pquotino)) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "quota accounting is not supported for read-write")
        .Metadata("superblock")
        .Actual(sb.qflags);
  }

  // An inode chunk is one inobt record (64 inodes). When a block holds more
  // than 64 inodes a chunk spans several records; that geometry is outside
  // the tested profile (the allocator would refuse before writing, but a
  // clean preflight is better).
  if (sb.inopblock > ondisk::kInodesPerChunk) {
    return XFS_ERR(XfsError::kUnsupportedGeometry,
                   "inodes per block above 64 are not supported for "
                   "read-write")
        .Metadata("superblock")
        .Actual(sb.inopblock);
  }

  // Directory blocks larger than one filesystem block would need multi-block
  // directory block allocation in the allocator.
  if (sb.dirblklog != 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "multiple filesystem blocks per directory block are not "
                   "supported for read-write")
        .Metadata("superblock")
        .Actual(sb.dirblklog);
  }

  // A writer must be able to write the log, so it must be the internal,
  // single-device log this build understands (CheckMountable already
  // validates its placement inside one AG).
  if (sb.logstart == 0 || sb.logblocks == 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "external log devices are not supported for read-write")
        .Metadata("superblock");
  }
  if (sb.features_log_incompat != 0) {
    return XFS_ERR(XfsError::kUnsupportedFeature,
                   "log-incompatible features are set; the log cannot be "
                   "written safely")
        .Metadata("superblock")
        .Actual(sb.features_log_incompat);
  }
  (void)geo;
  return OkStatus();
}

std::string WritableProfileSummary(const XfsSuperblock& sb) {
  std::string summary = "XFS v5, crc=1, internal log, single data device";
  if (sb.features.meta_uuid) summary += ", meta-uuid";
  if (sb.features.ftype) summary += ", ftype=1";
  if (sb.features.finobt) summary += ", finobt";
  if (sb.features.inobtcnt) summary += ", inobtcount";
  if (sb.features.spinodes) summary += ", sparse-inodes";
  if (sb.features.bigtime) summary += ", bigtime";
  if (sb.features.nrext64) summary += ", nrext64";
  if (sb.features.exchrange) summary += ", exchrange";
  if (sb.features.rmapbt) summary += ", rmapbt";
  if (sb.features.reflink) summary += ", reflink";
  return summary;
}

}  // namespace xfs
