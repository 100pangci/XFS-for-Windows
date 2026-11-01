// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/device_discovery.h"

#include <algorithm>
#include <cstdio>
#include <limits>

#include "xfs/block_device.h"
#include "xfs/superblock.h"
#include "xfs/write.h"

namespace xfs {
namespace {

std::string TrimLabel(const XfsSuperblock& sb) {
  std::string label;
  for (char c : sb.label) {
    if (c == '\0') break;
    label.push_back(c);
  }
  while (!label.empty() &&
         (label.back() == ' ' || label.back() == '\t' ||
          label.back() == '\r' || label.back() == '\n')) {
    label.pop_back();
  }
  return label;
}

bool MulOverflows(uint64_t a, uint64_t b) {
  if (a == 0 || b == 0) return false;
  return a > std::numeric_limits<uint64_t>::max() / b;
}

XfsProbeState StateForError(XfsError code) {
  switch (code) {
    case XfsError::kBadMagic:
      return XfsProbeState::kNoXfs;
    case XfsError::kIoError:
    case XfsError::kShortRead:
    case XfsError::kPermissionDenied:
      return XfsProbeState::kUnreadable;
    case XfsError::kUnsupportedFeature:
    case XfsError::kUnsupportedGeometry:
    case XfsError::kNotV5:
    case XfsError::kNeedsRepair:
      return XfsProbeState::kUnsupported;
    default:
      return XfsProbeState::kCorrupt;
  }
}

}  // namespace

const char* ToString(XfsProbeState state) {
  switch (state) {
    case XfsProbeState::kNoXfs: return "not-xfs";
    case XfsProbeState::kValid: return "valid";
    case XfsProbeState::kCorrupt: return "corrupt";
    case XfsProbeState::kUnsupported: return "unsupported";
    case XfsProbeState::kUnreadable: return "unreadable";
  }
  return "unknown";
}

XfsProbeResult ProbeXfsFilesystem(BlockDevice& device, uint64_t byte_offset,
                                  uint64_t available_bytes,
                                  const DiscoveryOptions& options) {
  XfsProbeResult result;
  if (available_bytes == 0 || byte_offset > device.Size() ||
      available_bytes > device.Size() - byte_offset) {
    result.state = XfsProbeState::kCorrupt;
    result.error_code = XfsError::kOutOfRange;
    result.error = "partition is outside the block device";
    return result;
  }

  auto sb_result = ReadSuperblock(device, byte_offset, /*verify_crc=*/true);
  if (!sb_result.ok()) {
    const ErrorContext& error = sb_result.error();
    result.error_code = error.code;
    result.error = error.Format();
    result.state = StateForError(error.code);
    result.magic_seen =
        error.code != XfsError::kBadMagic &&
        error.code != XfsError::kIoError &&
        error.code != XfsError::kShortRead &&
        error.code != XfsError::kPermissionDenied;
    if (result.state == XfsProbeState::kUnreadable) {
      result.device_error_kind = device.LastErrorKind();
    }
    return result;
  }

  XfsSuperblock sb = std::move(sb_result.value());
  result.magic_seen = true;
  result.superblock = sb;
  result.geometry = MakeGeometry(sb);
  result.label = TrimLabel(sb);

  Status mountable = CheckMountable(sb);
  if (!mountable.ok()) {
    result.state = StateForError(mountable.error().code);
    if (result.state == XfsProbeState::kNoXfs) {
      result.state = XfsProbeState::kCorrupt;
    }
    result.error_code = mountable.error().code;
    result.error = mountable.error().Format();
    return result;
  }

  const XfsGeometry& geo = result.geometry;
  if (geo.block_size == 0 || geo.dblocks == 0 ||
      MulOverflows(geo.dblocks, geo.block_size) ||
      geo.dblocks * geo.block_size > available_bytes) {
    result.state = XfsProbeState::kCorrupt;
    result.error_code = XfsError::kCorruptSuperblock;
    result.error = "filesystem is larger than the partition";
    return result;
  }
  result.filesystem_bytes = geo.dblocks * geo.block_size;
  if (sb.fdblocks <= sb.dblocks) {
    result.free_bytes = sb.fdblocks * geo.block_size;
  }

  // The internal log is validated by the same classifier the mount path uses.
  // An unreadable or malformed log does not invalidate the superblock: the
  // filesystem is still listed, with an explicit journal state.
  if (options.probe_journal && sb.logstart != 0 && sb.logblocks != 0) {
    const uint64_t log_start = geo.FsbToByte(sb.logstart);
    const uint64_t log_bytes =
        static_cast<uint64_t>(sb.logblocks) * geo.block_size;
    if (MulOverflows(sb.logblocks, geo.block_size) ||
        log_start > available_bytes || log_bytes > available_bytes - log_start) {
      result.log_state = LogState::kUnknown;
      result.log_detail = "internal log lies outside the partition";
    } else {
      auto log = CheckLogClean(device, geo, sb);
      if (!log.ok()) {
        result.log_state = LogState::kUnknown;
        result.log_detail = log.error().Format();
      } else {
        result.log_state = log.value().state;
        result.log_detail = log.value().detail;
      }
    }
  }

  if (options.probe_writable_profile) {
    Status writable = CheckWritableProfile(sb, geo);
    result.writable_profile = writable.ok();
    result.writable_profile_detail =
        writable.ok() ? WritableProfileSummary(sb) : writable.error().message;
  }

  result.state = XfsProbeState::kValid;
  result.error_code = XfsError::kOk;
  result.error.clear();
  return result;
}

std::string DescribeProbeRefusal(const XfsProbeResult& probe) {
  switch (probe.state) {
    case XfsProbeState::kNoXfs:
      return "not an XFS filesystem";
    case XfsProbeState::kValid:
      return {};
    case XfsProbeState::kUnreadable:
      return "the device could not be read (it may not be ready yet)";
    case XfsProbeState::kUnsupported:
      return "this XFS filesystem uses a feature this build does not support";
    case XfsProbeState::kCorrupt:
      return "the XFS superblock is damaged";
  }
  return "unknown filesystem state";
}

}  // namespace xfs