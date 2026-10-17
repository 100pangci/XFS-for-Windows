// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - UUID helpers.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace xfs {

using Uuid = std::array<uint8_t, 16>;

// "12345678-1234-1234-1234-123456789abc" (lowercase).
std::string FormatUuid(const Uuid& uuid);

// Parses the 8-4-4-4-12 form. Returns false on malformed input.
bool ParseUuid(const std::string& text, Uuid* out);

bool IsNullUuid(const Uuid& uuid);
bool UuidEqual(const Uuid& a, const Uuid& b);

}  // namespace xfs
