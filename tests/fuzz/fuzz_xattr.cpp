// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// libFuzzer target: the shortform extended-attribute fork parser.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xfs/inode.h"
#include "xfs/ondisk.h"
#include "xfs/xattr.h"

#include "fuzz_support.h"

namespace {

using namespace xfs;
namespace di = ondisk::inode;

void Exercise(const uint8_t* data, size_t size) {
  const XfsGeometry geo = fuzz::FuzzGeometry();
  fuzz::MemoryDevice device(1 << 20);
  XfsInode inode;
  inode.ino = 128;
  inode.version = 3;
  inode.aformat = di::kFormatLocal;
  inode.forkoff = 1;
  if (size != 0) inode.attr_fork.assign(data, data + size);

  (void)ReadXattrFork(device, geo, inode);
  (void)ListXattrs(device, geo, inode, true);
  (void)GetXattr(device, geo, inode, "user.fuzz", true);

  const std::string_view name(reinterpret_cast<const char*>(data), size);
  (void)ValidateXattrName(name, 0, false);
  (void)ValidateXattrName(name, 0, true);
  (void)AttrHashName(std::string(name));
  (void)AttrLocalEntrySize(size, size);
  (void)AttrRemoteEntrySize(size);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  Exercise(data, size);
  return 0;
}
