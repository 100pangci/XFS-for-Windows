// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - symlink target reading (never follows the link).
#pragma once

#include <string>

#include "xfs/block_device.h"
#include "xfs/error.h"
#include "xfs/inode.h"
#include "xfs/superblock.h"

namespace xfs {

// Returns the target string of a symlink inode, decoding both inline
// (LOCAL) and remote (extent/btree) symlinks. The link is never followed and
// nothing on disk is modified.
Result<std::string> ReadSymlinkTarget(BlockDevice& dev, const XfsGeometry& geo,
                                      const XfsInode& inode);

}  // namespace xfs
