// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// libFuzzer target: v3 inode parser and the fork parsers reachable from it
// (extent/bmbt roots, shortform/leaf/node attributes, symlink targets).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xfs/crc32c.h"
#include "xfs/directory.h"
#include "xfs/endian.h"
#include "xfs/inode.h"
#include "xfs/ondisk.h"
#include "xfs/symlink.h"
#include "xfs/xattr.h"

#include "fuzz_support.h"

namespace {

using namespace xfs;
namespace di = ondisk::inode;

void Exercise(const XfsInode& inode) {
  const XfsGeometry geo = fuzz::FuzzGeometry();
  fuzz::MemoryDevice device(1 << 20);
  (void)ReadXattrFork(device, geo, inode);
  (void)ListXattrs(device, geo, inode, true);
  (void)GetXattr(device, geo, inode, "user.fuzz", true);
  if (inode.IsDir()) {
    uint64_t count = 0;
    (void)ListDirectory(device, geo, inode,
                        [&count](const XfsDirEntry&) {
                          ++count;
                          return OkStatus();
                        });
    (void)LookupName(device, geo, inode, "fuzz");
  }
  if (inode.IsSymlink()) {
    (void)ReadSymlinkTarget(device, geo, inode);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const XfsGeometry geo = fuzz::FuzzGeometry();
  constexpr uint64_t kIno = 128;

  // Pass 1: the inode exactly as supplied (short buffers must be rejected).
  {
    XfsInode inode;
    if (ParseInode(data, size, geo, kIno, &inode).ok()) {
      Exercise(inode);
    }
  }

  // Pass 2: repair the identity fields and CRC. Everything that describes
  // forks, modes, flags and extents stays under fuzzer control.
  std::vector<uint8_t> image(geo.inode_size, 0);
  fuzz::CopyInput(data, size, 0, &image);
  PutBE16(image.data(), di::kMagic, ondisk::kDinodeMagic);
  image[di::kVersion] = 3;
  PutBE64(image.data(), di::kIno, kIno);
  PutBE32(image.data(), di::kNextUnlinked, 0xffffffffu);
  std::memcpy(image.data() + di::kUuid, geo.meta_uuid.data(), 16);
  UpdateXfsChecksum(image.data(), image.size(), di::kCrc);

  XfsInode inode;
  if (ParseInode(image.data(), image.size(), geo, kIno, &inode).ok()) {
    Exercise(inode);
  }
  return 0;
}
