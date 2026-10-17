#include "xfs/crc32c.h"

#include <cstring>

#include "xfs/endian.h"

#if defined(_M_X64) || defined(__x86_64__)
#define XFSWIN_CRC32C_X64 1
#include <intrin.h>
#elif defined(_M_IX86) || defined(__i386__)
#define XFSWIN_CRC32C_X86 1
#include <intrin.h>
#endif

namespace xfs {
namespace {

// Reflected Castagnoli polynomial.
constexpr uint32_t kPoly = 0x82F63B78u;

struct Tables {
  uint32_t t[8][256];
  constexpr Tables() : t{} {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (int j = 0; j < 8; ++j) {
        crc = (crc & 1) ? (crc >> 1) ^ kPoly : crc >> 1;
      }
      t[0][i] = crc;
    }
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = t[0][i];
      for (int k = 1; k < 8; ++k) {
        crc = t[0][crc & 0xFF] ^ (crc >> 8);
        t[k][i] = crc;
      }
    }
  }
};

constexpr Tables kTables{};

uint32_t Crc32cSoftware(uint32_t crc, const uint8_t* p, size_t len) {
  // Slicing-by-8.
  while (len >= 8) {
    uint32_t lo = crc ^ static_cast<uint32_t>(p[0]) |
                  (static_cast<uint32_t>(p[1]) << 8) |
                  (static_cast<uint32_t>(p[2]) << 16) |
                  (static_cast<uint32_t>(p[3]) << 24);
    uint32_t hi = static_cast<uint32_t>(p[4]) |
                  (static_cast<uint32_t>(p[5]) << 8) |
                  (static_cast<uint32_t>(p[6]) << 16) |
                  (static_cast<uint32_t>(p[7]) << 24);
    crc = kTables.t[7][lo & 0xFF] ^ kTables.t[6][(lo >> 8) & 0xFF] ^
          kTables.t[5][(lo >> 16) & 0xFF] ^ kTables.t[4][(lo >> 24) & 0xFF] ^
          kTables.t[3][hi & 0xFF] ^ kTables.t[2][(hi >> 8) & 0xFF] ^
          kTables.t[1][(hi >> 16) & 0xFF] ^ kTables.t[0][(hi >> 24) & 0xFF];
    p += 8;
    len -= 8;
  }
  while (len--) {
    crc = kTables.t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

#if defined(XFSWIN_CRC32C_X64) || defined(XFSWIN_CRC32C_X86)
bool HaveSse42() {
  // Leaf 1, ECX bit 20 = SSE4.2.
  int regs[4] = {0, 0, 0, 0};
  __cpuid(regs, 1);
  return (regs[2] & (1 << 20)) != 0;
}
#endif

uint32_t Crc32cHardware(uint32_t crc, const uint8_t* p, size_t len) {
#if defined(XFSWIN_CRC32C_X64)
  uint64_t state = crc;
  while (len >= 8) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    state = _mm_crc32_u64(state, v);
    p += 8;
    len -= 8;
  }
  uint32_t s32 = static_cast<uint32_t>(state);
  while (len--) {
    s32 = _mm_crc32_u8(s32, *p++);
  }
  return s32;
#elif defined(XFSWIN_CRC32C_X86)
  uint32_t state = crc;
  while (len >= 4) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    state = _mm_crc32_u32(state, v);
    p += 4;
    len -= 4;
  }
  while (len--) {
    state = _mm_crc32_u8(state, *p++);
  }
  return state;
#else
  return Crc32cSoftware(crc, p, len);
#endif
}

}  // namespace

uint32_t Crc32cUpdate(uint32_t crc, const void* data, size_t len) {
  if (len == 0) return crc;
  const auto* p = static_cast<const uint8_t*>(data);
#if defined(XFSWIN_CRC32C_X64) || defined(XFSWIN_CRC32C_X86)
  static const bool has_sse42 = HaveSse42();
  if (has_sse42) return Crc32cHardware(crc, p, len);
#endif
  return Crc32cSoftware(crc, p, len);
}

uint32_t Crc32c(const void* data, size_t len) {
  return ~Crc32cUpdate(0xFFFFFFFFu, data, len);
}

uint32_t XfsChecksum(const void* buffer, size_t length, size_t crc_offset) {
  const auto* p = static_cast<const uint8_t*>(buffer);
  uint32_t crc = Crc32cUpdate(0xFFFFFFFFu, p, crc_offset);
  const uint8_t zero[4] = {0, 0, 0, 0};
  crc = Crc32cUpdate(crc, zero, sizeof(zero));
  if (length > crc_offset + 4) {
    crc = Crc32cUpdate(crc, p + crc_offset + 4, length - (crc_offset + 4));
  }
  return ~crc;  // final inversion; stored little-endian on disk
}

bool VerifyXfsChecksum(const void* buffer, size_t length,
                       size_t crc_offset) {
  const auto* p = static_cast<const uint8_t*>(buffer);
  uint32_t expected = XfsChecksum(buffer, length, crc_offset);
  uint32_t actual = LoadLE32(p + crc_offset);
  return expected == actual;
}

void UpdateXfsChecksum(void* buffer, size_t length, size_t crc_offset) {
  uint32_t crc = XfsChecksum(buffer, length, crc_offset);
  StoreLE32(static_cast<uint8_t*>(buffer) + crc_offset, crc);
}

}  // namespace xfs
