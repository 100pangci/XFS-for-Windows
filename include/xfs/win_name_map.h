// XFS for Windows - deterministic XFS <-> Windows name mapping.
//
// XFS names are arbitrary byte strings (UTF-8 in practice). Windows names
// forbid a set of characters, have reserved device names, and disallow
// trailing dots/spaces. The mapping below is:
//
//   * deterministic and reversible (never two XFS names to one Windows name)
//   * non-destructive: the on-disk name is never modified
//
// Encoding rules (applied to the raw UTF-8 bytes):
//   - bytes 0x00-0x1F, 0x7F and  < > : " / \ | ? *  %  are emitted as %XX
//   - '%' is always escaped, which makes decoding unambiguous
//   - a trailing '.' or ' ' (0x2E / 0x20) is escaped
//   - reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or
//     without an extension) get their first byte escaped
//
// After byte-level encoding the result is valid UTF-8 (escape sequences are
// pure ASCII) and is converted to UTF-16 for WinFsp.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xfs {

struct WindowsName {
  std::u16string name;
  bool escaped = false;   // name differs from the raw XFS name
  bool too_long = false;  // > 255 UTF-16 code units; cannot be represented
};

// Converts an on-disk name to the Windows-visible name.
WindowsName XfsNameToWindows(std::string_view xfs_name);

// Converts a Windows-visible name back to the raw on-disk bytes.
// Returns nullopt when the name cannot have been produced by the encoder
// (contains forbidden characters, is a reserved device name, ...).
std::optional<std::string> WindowsNameToXfs(std::u16string_view win_name);

// UTF-8 <-> UTF-16 conversion helpers (lossy replacement on invalid input;
// the name mapping layer works on raw bytes and produces valid UTF-8).
std::optional<std::string> Utf16ToUtf8(std::u16string_view input);
std::u16string Utf8ToUtf16(std::string_view input);

}  // namespace xfs
