// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "xfs/win_name_map.h"

#include <algorithm>
#include <cctype>

namespace xfs {
namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

bool IsForbiddenByte(uint8_t c) {
  if (c < 0x20 || c == 0x7F) return true;
  switch (c) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '/':
    case '\\':
    case '|':
    case '?':
    case '*':
    case '%':
      return true;
    default:
      return false;
  }
}

bool IsReservedName(std::string_view name) {
  // Compare the part before the first '.' (Windows treats CON.txt as CON).
  std::string_view base = name;
  size_t dot = name.find('.');
  if (dot != std::string_view::npos) base = name.substr(0, dot);
  std::string upper;
  upper.reserve(base.size());
  for (char c : base) {
    upper.push_back(static_cast<char>(
        std::toupper(static_cast<unsigned char>(c))));
  }
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
    return true;
  }
  if (upper.size() == 4 &&
      (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0)) {
    char digit = upper[3];
    if (digit >= '1' && digit <= '9') return true;
  }
  return false;
}

bool IsHexDigit(char16_t c) {
  return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') ||
         (c >= u'A' && c <= u'F');
}

int HexValue(char16_t c) {
  if (c >= u'0' && c <= u'9') return c - u'0';
  if (c >= u'a' && c <= u'f') return c - u'a' + 10;
  if (c >= u'A' && c <= u'F') return c - u'A' + 10;
  return -1;
}

}  // namespace

WindowsName XfsNameToWindows(std::string_view xfs_name) {
  WindowsName out;
  std::string encoded;
  encoded.reserve(xfs_name.size());

  for (size_t i = 0; i < xfs_name.size(); ++i) {
    uint8_t c = static_cast<uint8_t>(xfs_name[i]);
    bool last = (i + 1 == xfs_name.size());
    bool escape = IsForbiddenByte(c);
    if (!escape && last && (c == '.' || c == ' ')) escape = true;
    // A reserved device name gets its first byte escaped.
    if (i == 0 && !escape && IsReservedName(xfs_name)) escape = true;
    if (escape) {
      encoded.push_back('%');
      encoded.push_back(kHexDigits[c >> 4]);
      encoded.push_back(kHexDigits[c & 0x0F]);
      out.escaped = true;
    } else {
      encoded.push_back(static_cast<char>(c));
    }
  }

  out.name = Utf8ToUtf16(encoded);
  if (out.name.size() > 255) out.too_long = true;
  return out;
}

std::optional<std::string> WindowsNameToXfs(std::u16string_view win_name) {
  if (win_name.empty()) return std::nullopt;
  // The encoder never produces names with these trailing characters.
  char16_t last = win_name.back();
  if (last == u'.' || last == u' ' || last == 0 || last < 0x20) {
    return std::nullopt;
  }
  std::string utf8;
  utf8.reserve(win_name.size());
  std::string literal_input;
  for (size_t i = 0; i < win_name.size(); ++i) {
    char16_t c = win_name[i];
    if (c == u'%') {
      if (i + 2 >= win_name.size() || !IsHexDigit(win_name[i + 1]) ||
          !IsHexDigit(win_name[i + 2])) {
        return std::nullopt;
      }
      int hi = HexValue(win_name[i + 1]);
      int lo = HexValue(win_name[i + 2]);
      uint8_t byte = static_cast<uint8_t>((hi << 4) | lo);
      utf8.push_back(static_cast<char>(byte));
      literal_input += '%';
      i += 2;
      continue;
    }
    if (c < 0x20 || c == 0x7F || c == u'<' || c == u'>' || c == u':' ||
        c == u'"' || c == u'/' || c == u'\\' || c == u'|' || c == u'?' ||
        c == u'*') {
      return std::nullopt;
    }
    // Convert this UTF-16 code unit (with surrogate handling) to UTF-8.
    std::u16string one;
    one.push_back(c);
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 >= win_name.size() || win_name[i + 1] < 0xDC00 ||
          win_name[i + 1] > 0xDFFF) {
        return std::nullopt;  // lone high surrogate
      }
      one.push_back(win_name[i + 1]);
      ++i;
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      return std::nullopt;  // lone low surrogate
    }
    auto converted = Utf16ToUtf8(one);
    if (!converted) return std::nullopt;
    utf8 += *converted;
    literal_input += *converted;
  }
  // A name that is literally a reserved device name cannot be produced by
  // the encoder (it would have escaped the first byte).
  if (IsReservedName(literal_input)) return std::nullopt;
  return utf8;
}

std::optional<std::string> Utf16ToUtf8(std::u16string_view input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    uint32_t cp = input[i];
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if (i + 1 >= input.size() || input[i + 1] < 0xDC00 ||
          input[i + 1] > 0xDFFF) {
        return std::nullopt;
      }
      cp = 0x10000 + ((cp - 0xD800) << 10) + (input[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      return std::nullopt;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

std::u16string Utf8ToUtf16(std::string_view input) {
  std::u16string out;
  out.reserve(input.size());
  const uint8_t* p = reinterpret_cast<const uint8_t*>(input.data());
  size_t len = input.size();
  size_t i = 0;
  while (i < len) {
    uint8_t c = p[i];
    uint32_t cp = 0;
    size_t extra = 0;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      extra = 3;
    } else {
      out.push_back(0xFFFD);  // replacement character
      ++i;
      continue;
    }
    if (i + extra >= len) {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    bool valid = true;
    for (size_t k = 1; k <= extra; ++k) {
      if ((p[i + k] & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (p[i + k] & 0x3F);
    }
    if (!valid || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    i += extra + 1;
    if (cp < 0x10000) {
      out.push_back(static_cast<char16_t>(cp));
    } else {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    }
  }
  return out;
}

}  // namespace xfs
