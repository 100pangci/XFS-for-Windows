// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// libFuzzer target: the v5 superblock parser + mount policy.
//
// The raw pass exercises rejection of arbitrary bytes (magic, version, feature
// masks, geometry, CRC). The normalized pass keeps the fuzzable feature and
// geometry choices but repairs the identity fields and CRC, so the parser is
// also exercised past its checksum gate.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/ondisk.h"
#include "xfs/superblock.h"

#include "fuzz_support.h"

namespace {

using namespace xfs;
namespace sb = xfs::ondisk::sb;

void Exercise(const XfsSuperblock& superblock) {
  const XfsGeometry geo = MakeGeometry(superblock);
  (void)geo;
  (void)CheckMountable(superblock);
  (void)HasLogIncompatFeatures(superblock);
  (void)FormatLabel(superblock);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Pass 1: the bytes exactly as supplied (including short-buffer rejection).
  {
    XfsSuperblock out;
    if (ParseSuperblockBuffer(data, size, true, &out).ok()) {
      Exercise(out);
    }
    if (ParseSuperblockBuffer(data, size, false, &out).ok()) {
      Exercise(out);
    }
  }

  // Pass 2: repair only the identity fields and the checksum, leaving the
  // feature/geometry/accounting bytes under fuzzer control. Pad short inputs
  // to one sector so the parser itself runs instead of returning "too small".
  std::vector<uint8_t> image;
  if (size != 0) image.assign(data, data + std::min<size_t>(size, 4096));
  if (image.size() < 512) image.resize(512, 0);
  PutBE32(image.data(), sb::kMagic, ondisk::kSbMagic);
  PutBE16(image.data(), sb::kVersionnum, 0xb4a5);
  constexpr uint32_t kFeatures2 = 0x0000018a;  // lazy/attr2/projid32/crc
  PutBE32(image.data(), sb::kFeatures2, kFeatures2);
  PutBE32(image.data(), sb::kBadFeatures2, kFeatures2);
  std::memcpy(image.data() + sb::kUuid, fuzz::kFuzzUuid.data(), 16);
  std::memcpy(image.data() + sb::kMetaUuid, fuzz::kFuzzUuid.data(), 16);
  PutBE32(image.data(), sb::kBlocksize, 4096);
  image[sb::kBlocklog] = 12;
  PutBE16(image.data(), sb::kSectsize, 512);
  image[sb::kSectlog] = 9;
  PutBE16(image.data(), sb::kInodesize, 512);
  image[sb::kInodelog] = 9;
  PutBE16(image.data(), sb::kInopblock, 8);
  image[sb::kInopblog] = 3;
  PutBE32(image.data(), sb::kAgblocks, 65536);
  image[sb::kAgblklog] = 16;
  PutBE32(image.data(), sb::kAgcount, 1);
  PutBE64(image.data(), sb::kDblocks, 65536);
  PutBE64(image.data(), sb::kLogstart, 8);
  PutBE32(image.data(), sb::kLogblocks, 8);
  PutBE16(image.data(), sb::kLogsectsize, 512);
  image[sb::kLogsectlog] = 9;
  PutBE64(image.data(), sb::kRootino, 128);
  UpdateXfsChecksum(image.data(), 512, sb::kCrc);

  XfsSuperblock out;
  if (ParseSuperblockBuffer(image.data(), image.size(), true, &out).ok()) {
    Exercise(out);
  }
  return 0;
}
