// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - V5 metadata verification.
//
// Every piece of XFS v5 metadata carries an identification header:
//   magic, crc (le32), uuid, blkno (daddr, 512-byte units), lsn, owner
// at structure-specific offsets. This module provides uniform checks so the
// core can never parse unverified data.
#pragma once

#include <cstddef>
#include <cstdint>

#include "xfs/error.h"
#include "xfs/uuid.h"

namespace xfs {

constexpr size_t kNoField = static_cast<size_t>(-1);

// Individual field checks. `what` names the metadata type ("superblock",
// "AGF", ...) for error context.
Status VerifyMagic(const uint8_t* buf, size_t len, size_t off, uint32_t magic,
                   const char* what);
Status VerifyChecksum(const uint8_t* buf, size_t len, size_t crc_off,
                      const char* what);
Status VerifyUuidField(const uint8_t* buf, size_t len, size_t off,
                       const Uuid& expected, const char* what);
// expected is the on-disk value; for v5 metadata this is xfs_buf_daddr
// (byte offset / 512) of the buffer.
Status VerifyBlockNoField(const uint8_t* buf, size_t len, size_t off,
                          uint64_t expected_daddr, const char* what);
// Accepts lsn == 0 or lsn <= sb_lsn (xfs_log_check_lsn semantics).
Status VerifyLsnField(const uint8_t* buf, size_t len, size_t off,
                      uint64_t sb_lsn, const char* what);
Status VerifyOwnerField(const uint8_t* buf, size_t len, size_t off,
                        size_t owner_size, uint64_t expected_owner,
                        const char* what);

// Description of the common identification header of one metadata type.
struct V5MetaCheck {
  const char* type = "metadata";
  size_t magic_offset = kNoField;
  uint32_t magic = 0;
  size_t magic_size = 4;  // 2 for da-btree blocks (leaf/node), 4 otherwise
  size_t crc_offset = kNoField;
  size_t uuid_offset = kNoField;
  size_t blkno_offset = kNoField;
  size_t lsn_offset = kNoField;
  size_t owner_offset = kNoField;
  size_t owner_size = 8;  // 4 for short-form btree headers
};

// Runs all configured checks, reporting the first failure with context.
Status VerifyV5Metadata(const uint8_t* buf, size_t len, const V5MetaCheck& check,
                        uint64_t expected_daddr, uint64_t expected_owner,
                        const Uuid& meta_uuid, uint64_t sb_lsn);

}  // namespace xfs
