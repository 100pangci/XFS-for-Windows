// XFS for Windows - big/little endian load and store helpers.
//
// XFS on-disk data is big-endian, with a few little-endian exceptions (CRC
// fields). We never reinterpret_cast on-disk buffers to host structures:
// every field is read through these helpers from an explicit byte offset, so
// host endianness and struct padding can never leak into parsing.
#pragma once

#include <cstdint>
#include <cstring>

namespace xfs {

inline uint8_t LoadU8(const void* p) {
  uint8_t v;
  std::memcpy(&v, p, 1);
  return v;
}

inline int8_t LoadS8(const void* p) {
  int8_t v;
  std::memcpy(&v, p, 1);
  return v;
}

inline uint16_t LoadBE16(const void* p) {
  const auto* b = static_cast<const uint8_t*>(p);
  return static_cast<uint16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]);
}

inline uint32_t LoadBE32(const void* p) {
  const auto* b = static_cast<const uint8_t*>(p);
  return (static_cast<uint32_t>(b[0]) << 24) |
         (static_cast<uint32_t>(b[1]) << 16) |
         (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
}

inline uint64_t LoadBE64(const void* p) {
  const auto* b = static_cast<const uint8_t*>(p);
  return (static_cast<uint64_t>(b[0]) << 56) |
         (static_cast<uint64_t>(b[1]) << 48) |
         (static_cast<uint64_t>(b[2]) << 40) |
         (static_cast<uint64_t>(b[3]) << 32) |
         (static_cast<uint64_t>(b[4]) << 24) |
         (static_cast<uint64_t>(b[5]) << 16) |
         (static_cast<uint64_t>(b[6]) << 8) | static_cast<uint64_t>(b[7]);
}

inline uint16_t LoadLE16(const void* p) {
  const auto* b = static_cast<const uint8_t*>(p);
  return static_cast<uint16_t>((static_cast<uint16_t>(b[1]) << 8) | b[0]);
}

inline uint32_t LoadLE32(const void* p) {
  const auto* b = static_cast<const uint8_t*>(p);
  return (static_cast<uint32_t>(b[3]) << 24) |
         (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[1]) << 8) | static_cast<uint32_t>(b[0]);
}

inline void StoreBE16(void* p, uint16_t v) {
  auto* b = static_cast<uint8_t*>(p);
  b[0] = static_cast<uint8_t>(v >> 8);
  b[1] = static_cast<uint8_t>(v);
}

inline void StoreBE32(void* p, uint32_t v) {
  auto* b = static_cast<uint8_t*>(p);
  b[0] = static_cast<uint8_t>(v >> 24);
  b[1] = static_cast<uint8_t>(v >> 16);
  b[2] = static_cast<uint8_t>(v >> 8);
  b[3] = static_cast<uint8_t>(v);
}

inline void StoreBE64(void* p, uint64_t v) {
  auto* b = static_cast<uint8_t*>(p);
  b[0] = static_cast<uint8_t>(v >> 56);
  b[1] = static_cast<uint8_t>(v >> 48);
  b[2] = static_cast<uint8_t>(v >> 40);
  b[3] = static_cast<uint8_t>(v >> 32);
  b[4] = static_cast<uint8_t>(v >> 24);
  b[5] = static_cast<uint8_t>(v >> 16);
  b[6] = static_cast<uint8_t>(v >> 8);
  b[7] = static_cast<uint8_t>(v);
}

inline void StoreLE16(void* p, uint16_t v) {
  auto* b = static_cast<uint8_t*>(p);
  b[0] = static_cast<uint8_t>(v);
  b[1] = static_cast<uint8_t>(v >> 8);
}

inline void StoreLE32(void* p, uint32_t v) {
  auto* b = static_cast<uint8_t*>(p);
  b[0] = static_cast<uint8_t>(v);
  b[1] = static_cast<uint8_t>(v >> 8);
  b[2] = static_cast<uint8_t>(v >> 16);
  b[3] = static_cast<uint8_t>(v >> 24);
}

// Offset-based reads over a base pointer: GetBE32(base, 0x20). The one-argument
// forms read at offset 0 for convenience in pointer walks.
inline uint8_t GetU8(const void* base, size_t off) {
  return LoadU8(static_cast<const uint8_t*>(base) + off);
}
inline uint8_t GetU8(const void* p) { return LoadU8(p); }
inline int8_t GetS8(const void* base, size_t off) {
  return LoadS8(static_cast<const uint8_t*>(base) + off);
}
inline int8_t GetS8(const void* p) { return LoadS8(p); }
inline uint16_t GetBE16(const void* base, size_t off) {
  return LoadBE16(static_cast<const uint8_t*>(base) + off);
}
inline uint16_t GetBE16(const void* p) { return LoadBE16(p); }
inline uint32_t GetBE32(const void* base, size_t off) {
  return LoadBE32(static_cast<const uint8_t*>(base) + off);
}
inline uint32_t GetBE32(const void* p) { return LoadBE32(p); }
inline uint64_t GetBE64(const void* base, size_t off) {
  return LoadBE64(static_cast<const uint8_t*>(base) + off);
}
inline uint64_t GetBE64(const void* p) { return LoadBE64(p); }
inline uint32_t GetLE32(const void* base, size_t off) {
  return LoadLE32(static_cast<const uint8_t*>(base) + off);
}
inline uint32_t GetLE32(const void* p) { return LoadLE32(p); }
inline void PutBE16(void* base, size_t off, uint16_t v) {
  StoreBE16(static_cast<uint8_t*>(base) + off, v);
}
inline void PutBE32(void* base, size_t off, uint32_t v) {
  StoreBE32(static_cast<uint8_t*>(base) + off, v);
}
inline void PutBE64(void* base, size_t off, uint64_t v) {
  StoreBE64(static_cast<uint8_t*>(base) + off, v);
}
inline void PutLE32(void* base, size_t off, uint32_t v) {
  StoreLE32(static_cast<uint8_t*>(base) + off, v);
}

// Round x up to a multiple of align (power of two).
inline uint64_t RoundUp64(uint64_t x, uint64_t align) {
  return (x + align - 1) & ~(align - 1);
}
inline uint32_t RoundUp32(uint32_t x, uint32_t align) {
  return (x + align - 1) & ~(align - 1);
}
// log2 of a power of two.
inline int Log2(uint64_t x) {
  int n = 0;
  while (x > 1) {
    x >>= 1;
    ++n;
  }
  return x == 1 ? n : -1;
}
inline bool IsPowerOfTwo(uint64_t x) { return x != 0 && (x & (x - 1)) == 0; }

}  // namespace xfs
