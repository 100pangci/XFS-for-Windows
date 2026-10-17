// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <algorithm>
#include <cstring>

#include "mkfs/mkfs_internal.h"
#include "xfs/endian.h"
#include "xfs/ondisk.h"

namespace xfs::mkfs::internal {
namespace {

// xlog_rec_header field offsets (512-byte record header).
constexpr size_t kOffMagic = 0;      // be32
constexpr size_t kOffCycle = 4;      // be32
constexpr size_t kOffVersion = 8;    // be32
constexpr size_t kOffLen = 12;       // be32
constexpr size_t kOffLsn = 16;       // be64
constexpr size_t kOffTailLsn = 24;   // be64
constexpr size_t kOffCrc = 32;       // le32
constexpr size_t kOffPrevBlock = 36; // be32
constexpr size_t kOffNumLogops = 40;
constexpr size_t kOffCycleData = 44; // be32[64]
constexpr size_t kOffFmt = 300;      // be32
constexpr size_t kOffUuid = 304;     // 16 bytes
constexpr size_t kOffSize = 320;     // be32

constexpr uint32_t kLogMagic = 0xFEEDBABE;
constexpr size_t kSectorSize = 512;
constexpr uint32_t kLogVersion2 = 2;
constexpr uint32_t kFmtLinuxLe = 1;
constexpr uint32_t kBigRecordSize = 32 * 1024;
constexpr uint32_t kUnmountTransFlag = 0x20;
constexpr uint8_t kClientXfsLog = 0xaa;
constexpr uint16_t kUnmountType = 0x556e;

}  // namespace

void BuildLogRecord(const FormatGeometry& geo, std::vector<uint8_t>* buf) {
  buf->assign(2 * kSectorSize, 0);
  uint8_t* p = buf->data();

  const uint64_t lsn = (1ULL << 32);  // XLOG_INIT_CYCLE = 1, block 0
  const uint32_t cycle = 1;

  // Record header (block 0).
  PutBE32(p, kOffMagic, kLogMagic);
  PutBE32(p, kOffCycle, cycle);
  PutBE32(p, kOffVersion, kLogVersion2);
  PutBE32(p, kOffLen, 512);  // max(2 sectors, sunit) - 1 header sector
  PutBE64(p, kOffLsn, lsn);
  PutBE64(p, kOffTailLsn, lsn);
  PutLE32(p, kOffCrc, 0);
  PutBE32(p, kOffPrevBlock, 0xffffffffu);
  PutBE32(p, kOffNumLogops, 1);
  PutBE32(p, kOffFmt, kFmtLinuxLe);
  std::memcpy(p + kOffUuid, geo.uuid.data(), 16);
  PutBE32(p, kOffSize, kBigRecordSize);

  // Unmount record (block 1): op header + xfs_unmount_log_format.
  uint8_t* data = p + kSectorSize;
  PutBE32(data, 0, 0xb0c0d0d0u);  // oh_tid: "written from userspace"
  PutBE32(data, 4, 8);            // oh_len
  data[8] = kClientXfsLog;        // oh_clientid
  data[9] = kUnmountTransFlag;    // oh_flags
  PutBE16(data, 10, 0);           // oh_res2
  PutBE16(data, 12, kUnmountType);
  PutBE16(data, 14, 0);
  PutBE32(data, 16, 0);

  // The first word of every record data sector is replaced by the cycle
  // value; the original word is stored in the record header.
  PutBE32(p, kOffCycleData, GetBE32(data, 0));
  PutBE32(data, 0, cycle);
}

}  // namespace xfs::mkfs::internal
