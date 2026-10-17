#include "xfs/verifier.h"

#include "xfs/crc32c.h"
#include "xfs/endian.h"

namespace xfs {

Status VerifyMagic(const uint8_t* buf, size_t len, size_t off, uint32_t magic,
                   const char* what) {
  if (off + 4 > len) {
    return XFS_ERR(XfsError::kOutOfRange, "metadata too small for magic")
        .Metadata(what)
        .Offset(off);
  }
  uint32_t actual = GetBE32(buf, off);
  if (actual != magic) {
    return XFS_ERR(XfsError::kBadMagic, "magic mismatch")
        .Metadata(what)
        .Offset(off)
        .Expected(magic)
        .Actual(actual);
  }
  return OkStatus();
}

Status VerifyChecksum(const uint8_t* buf, size_t len, size_t crc_off,
                      const char* what) {
  if (crc_off + 4 > len) {
    return XFS_ERR(XfsError::kOutOfRange, "metadata too small for crc")
        .Metadata(what)
        .Offset(crc_off);
  }
  uint32_t expected = XfsChecksum(buf, len, crc_off);
  uint32_t actual = LoadLE32(buf + crc_off);
  if (expected != actual) {
    return XFS_ERR(XfsError::kBadCrc, "CRC mismatch")
        .Metadata(what)
        .Offset(crc_off)
        .Expected(expected)
        .Actual(actual);
  }
  return OkStatus();
}

Status VerifyUuidField(const uint8_t* buf, size_t len, size_t off,
                       const Uuid& expected, const char* what) {
  if (off + 16 > len) {
    return XFS_ERR(XfsError::kOutOfRange, "metadata too small for uuid")
        .Metadata(what)
        .Offset(off);
  }
  if (std::memcmp(buf + off, expected.data(), 16) != 0) {
    Uuid actual_uuid;
    std::memcpy(actual_uuid.data(), buf + off, 16);
    return XFS_ERR(XfsError::kBadUuid,
                   "uuid mismatch (expected " + FormatUuid(expected) +
                       ", found " + FormatUuid(actual_uuid) + ")")
        .Metadata(what)
        .Offset(off);
  }
  return OkStatus();
}

Status VerifyBlockNoField(const uint8_t* buf, size_t len, size_t off,
                          uint64_t expected_daddr, const char* what) {
  if (off + 8 > len) {
    return XFS_ERR(XfsError::kOutOfRange, "metadata too small for blkno")
        .Metadata(what)
        .Offset(off);
  }
  uint64_t actual = GetBE64(buf, off);
  if (actual != expected_daddr) {
    return XFS_ERR(XfsError::kBadBlockNo, "blkno mismatch (value is daddr)")
        .Metadata(what)
        .Offset(off)
        .Expected(expected_daddr)
        .Actual(actual);
  }
  return OkStatus();
}

Status VerifyLsnField(const uint8_t* buf, size_t len, size_t off,
                      uint64_t sb_lsn, const char* what) {
  if (off + 8 > len) {
    return XFS_ERR(XfsError::kOutOfRange, "metadata too small for lsn")
        .Metadata(what)
        .Offset(off);
  }
  uint64_t lsn = GetBE64(buf, off);
  // xfs_log_check_lsn: valid if lsn == 0 or lsn <= sb_lsn (lsn ahead of the
  // superblock means the metadata is from a future, uncommitted write).
  if (lsn != 0 && sb_lsn != 0 && lsn > sb_lsn) {
    return XFS_ERR(XfsError::kBadValue, "metadata LSN is ahead of superblock")
        .Metadata(what)
        .Offset(off)
        .Expected(sb_lsn)
        .Actual(lsn);
  }
  return OkStatus();
}

Status VerifyOwnerField(const uint8_t* buf, size_t len, size_t off,
                        size_t owner_size, uint64_t expected_owner,
                        const char* what) {
  if (off + owner_size > len) {
    return XFS_ERR(XfsError::kOutOfRange, "metadata too small for owner")
        .Metadata(what)
        .Offset(off);
  }
  uint64_t actual =
      owner_size == 4 ? GetBE32(buf, off) : GetBE64(buf, off);
  if (actual != expected_owner) {
    return XFS_ERR(XfsError::kBadOwner, "owner mismatch")
        .Metadata(what)
        .Offset(off)
        .Expected(expected_owner)
        .Actual(actual);
  }
  return OkStatus();
}

Status VerifyV5Metadata(const uint8_t* buf, size_t len, const V5MetaCheck& check,
                        uint64_t expected_daddr, uint64_t expected_owner,
                        const Uuid& meta_uuid, uint64_t sb_lsn) {
  if (check.magic_offset != kNoField) {
    if (check.magic_size == 2) {
      if (check.magic_offset + 2 > len) {
        return XFS_ERR(XfsError::kOutOfRange,
                       "metadata too small for magic")
            .Metadata(check.type)
            .Offset(check.magic_offset);
      }
      uint16_t actual = GetBE16(buf, check.magic_offset);
      if (actual != static_cast<uint16_t>(check.magic)) {
        return XFS_ERR(XfsError::kBadMagic, "magic mismatch")
            .Metadata(check.type)
            .Offset(check.magic_offset)
            .Expected(static_cast<uint16_t>(check.magic))
            .Actual(actual);
      }
    } else {
      Status s = VerifyMagic(buf, len, check.magic_offset, check.magic,
                             check.type);
      if (!s.ok()) return s;
    }
  }
  if (check.crc_offset != kNoField) {
    Status s = VerifyChecksum(buf, len, check.crc_offset, check.type);
    if (!s.ok()) return s;
  }
  if (check.uuid_offset != kNoField) {
    Status s = VerifyUuidField(buf, len, check.uuid_offset, meta_uuid,
                               check.type);
    if (!s.ok()) return s;
  }
  if (check.blkno_offset != kNoField) {
    Status s = VerifyBlockNoField(buf, len, check.blkno_offset, expected_daddr,
                                  check.type);
    if (!s.ok()) return s;
  }
  if (check.lsn_offset != kNoField) {
    Status s = VerifyLsnField(buf, len, check.lsn_offset, sb_lsn, check.type);
    if (!s.ok()) return s;
  }
  if (check.owner_offset != kNoField) {
    Status s = VerifyOwnerField(buf, len, check.owner_offset, check.owner_size,
                                expected_owner, check.type);
    if (!s.ok()) return s;
  }
  return OkStatus();
}

}  // namespace xfs
