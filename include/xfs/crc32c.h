// XFS for Windows - CRC32c (Castagnoli) implementation.
//
// XFS metadata checksums use CRC32c with a seed of ~0 and a final bitwise
// inversion; the value is stored little-endian at the structure's crc_offset.
// The crc field itself is treated as zero while the checksum is computed.
#pragma once

#include <cstddef>
#include <cstdint>

namespace xfs {

// Raw CRC32c accumulator: continues from `crc` without any final inversion.
// This matches crc32c(seed, data, len) as used by the XFS kernel and xfsprogs.
uint32_t Crc32cUpdate(uint32_t crc, const void* data, size_t len);

// Standard CRC32c of a buffer (seed ~0, with final inversion).
uint32_t Crc32c(const void* data, size_t len);

// XFS metadata checksum over a buffer whose crc field (4 bytes, little-endian
// on disk) is located at `crc_offset` and is treated as zero.
uint32_t XfsChecksum(const void* buffer, size_t length, size_t crc_offset);

// Verifies the on-disk crc field against the computed checksum.
bool VerifyXfsChecksum(const void* buffer, size_t length, size_t crc_offset);

// Writes the freshly computed checksum into the crc field.
void UpdateXfsChecksum(void* buffer, size_t length, size_t crc_offset);

}  // namespace xfs
