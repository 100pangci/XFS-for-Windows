// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - directory parsing (shortform / block / leaf / node).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/inode.h"
#include "xfs/superblock.h"

namespace xfs {

struct XfsDirEntry {
  std::string name;  // raw bytes as stored (UTF-8)
  uint64_t ino = 0;
  uint8_t ftype = 0;  // XFS_DIR3_FT_* (0 = unknown)
};

enum class XfsDirFormat { kShortform, kBlock, kLeaf, kNode };

// XFS directory name hash (xfs_da_hashname). Exposed for testing/validation.
uint32_t XfsDirNameHash(const uint8_t* name, size_t len);
inline uint32_t XfsDirNameHash(std::string_view name) {
  return XfsDirNameHash(reinterpret_cast<const uint8_t*>(name.data()),
                        name.size());
}

// Determines the on-disk directory layout (reads the first block).
Result<XfsDirFormat> DetectDirFormat(BlockDevice& dev, const XfsGeometry& geo,
                                     const XfsInode& inode);

// Lists all entries (including "." and ".."). The callback may stop early by
// returning an error. Entries are verified before being reported.
Status ListDirectory(BlockDevice& dev, const XfsGeometry& geo,
                     const XfsInode& inode,
                     const std::function<Status(const XfsDirEntry&)>& emit);

// Looks up one name. Returns kNotFound when the name does not exist, and
// kCorruptDirectory only for actual corruption.
Result<XfsDirEntry> LookupName(BlockDevice& dev, const XfsGeometry& geo,
                               const XfsInode& inode, std::string_view name);

}  // namespace xfs
