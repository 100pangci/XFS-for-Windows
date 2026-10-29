// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// libFuzzer target: the shortform directory parser.
//
// The input is the raw literal-area directory image. block/leaf/node formats
// are covered by the inode target and the image-level mount fuzzing, which
// need a device and are exercised through ListDirectory there.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xfs/directory.h"
#include "xfs/inode.h"
#include "xfs/ondisk.h"

#include "fuzz_support.h"

namespace {

using namespace xfs;
namespace di = ondisk::inode;

void Exercise(const uint8_t* data, size_t size) {
  const XfsGeometry geo = fuzz::FuzzGeometry();
  fuzz::MemoryDevice device(1 << 20);
  XfsInode inode;
  inode.ino = geo.rootino;
  inode.mode = kSIfdir | 0755;
  inode.version = 3;
  inode.format = di::kFormatLocal;
  inode.size = size;
  inode.nlink = 2;
  if (size != 0) inode.data_fork.assign(data, data + size);

  uint64_t count = 0;
  (void)ListDirectory(device, geo, inode,
                      [&count](const XfsDirEntry& e) -> Status {
                        ++count;
                        if (count > 1000000) {
                          return XFS_ERR(XfsError::kCorruptDirectory,
                                         "too many entries");
                        }
                        (void)XfsDirNameHash(e.name);
                        return OkStatus();
                      });
  (void)LookupName(device, geo, inode, "fuzz");
  (void)DetectDirFormat(device, geo, inode);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  Exercise(data, size);
  return 0;
}
