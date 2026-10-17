#include "xfs/uuid.h"

#include <cstdio>

#include "xfs/endian.h"

namespace xfs {

bool UuidEqual(const Uuid& a, const Uuid& b) { return a == b; }

bool IsNullUuid(const Uuid& uuid) {
  for (uint8_t b : uuid) {
    if (b != 0) return false;
  }
  return true;
}

std::string FormatUuid(const Uuid& uuid) {
  char buf[40];
  std::snprintf(
      buf, sizeof(buf),
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
      uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14],
      uuid[15]);
  return buf;
}

namespace {
int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

bool ParseUuid(const std::string& text, Uuid* out) {
  // Accept "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (36 chars).
  if (text.size() != 36) return false;
  size_t positions[16];
  size_t out_idx = 0;
  for (size_t i = 0; i < 36 && out_idx < 16; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') return false;
      continue;
    }
    positions[out_idx++] = i;
  }
  if (out_idx != 16) return false;
  Uuid uuid{};
  for (size_t i = 0; i < 16; ++i) {
    int hi = HexDigit(text[positions[i]]);
    int lo = HexDigit(text[positions[i] + 1]);
    if (hi < 0 || lo < 0) return false;
    uuid[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  *out = uuid;
  return true;
}

}  // namespace xfs
