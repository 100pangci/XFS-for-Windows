// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// libFuzzer target: AGF / AGI / AGFL header parsers.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xfs/ag.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/ondisk.h"

#include "fuzz_support.h"

namespace {

using namespace xfs;

void Exercise(const std::vector<uint8_t>& image, uint32_t flcount) {
  fuzz::MemoryDevice device(image.size());
  std::memcpy(device.bytes().data(), image.data(), image.size());
  const XfsGeometry geo = fuzz::FuzzGeometry();
  (void)ReadAgf(device, geo, 0);
  (void)ReadAgi(device, geo, 0);
  (void)ReadAgfl(device, geo, 0, flcount);
  (void)ReadAgHeaders(device, geo, 0);
}

uint32_t CountFrom(const uint8_t* data, size_t size, uint32_t cap) {
  if (size < 4) return 0;
  uint32_t value = 0;
  std::memcpy(&value, data, 4);
  return value % (cap + 1);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // The input is the three 512-byte AG header slots concatenated: AGF, then
  // AGI, then AGFL. This is also the shape of a real image's bytes 512..2048.
  auto field = [&](size_t slot) {
    const size_t offset = slot * 512;
    return size > offset ? std::min<size_t>(512, size - offset) : 0;
  };
  auto copy_slot = [&](size_t slot, std::vector<uint8_t>* image,
                       size_t target) {
    const size_t offset = slot * 512;
    if (size > offset) {
      fuzz::CopyInput(data + offset, field(slot), target, image);
    }
  };

  // Pass 1: arbitrary bytes in every AG header slot.
  {
    std::vector<uint8_t> image(4096, 0);
    copy_slot(0, &image, 512);
    copy_slot(1, &image, 1024);
    copy_slot(2, &image, 1536);
    Exercise(image, CountFrom(data, size, AgflSize(fuzz::FuzzGeometry())));
  }

  // Pass 2: repair identity fields and CRCs so the value parsers are reached
  // with arbitrary seqno/count/root/map/entry values.
  {
    std::vector<uint8_t> image(4096, 0);
    copy_slot(0, &image, 512);
    std::memcpy(image.data() + 512 + ondisk::agf::kUuid,
                fuzz::kFuzzUuid.data(), 16);
    PutBE32(image.data(), 512 + ondisk::agf::kMagic, ondisk::kAgfMagic);
    PutBE32(image.data(), 512 + ondisk::agf::kVersionnum,
              ondisk::agf::kVersion);
    PutBE32(image.data(), 512 + ondisk::agf::kSeqno, 0);
    PutBE32(image.data(), 512 + ondisk::agf::kLength,
              fuzz::FuzzGeometry().agblocks);
    PutBE64(image.data(), 512 + ondisk::agf::kLsn, 0);
    UpdateXfsChecksum(image.data() + 512, 512, ondisk::agf::kCrc);

    copy_slot(1, &image, 1024);
    std::memcpy(image.data() + 1024 + ondisk::agi::kUuid,
                fuzz::kFuzzUuid.data(), 16);
    PutBE32(image.data(), 1024 + ondisk::agi::kMagic, ondisk::kAgiMagic);
    PutBE32(image.data(), 1024 + ondisk::agi::kVersionnum,
              ondisk::agi::kVersion);
    PutBE32(image.data(), 1024 + ondisk::agi::kSeqno, 0);
    PutBE32(image.data(), 1024 + ondisk::agi::kLength,
              fuzz::FuzzGeometry().agblocks);
    PutBE64(image.data(), 1024 + ondisk::agi::kLsn, 0);
    UpdateXfsChecksum(image.data() + 1024, 512, ondisk::agi::kCrc);

    copy_slot(2, &image, 1536);
    std::memcpy(image.data() + 1536 + ondisk::agfl::kUuid,
                fuzz::kFuzzUuid.data(), 16);
    PutBE32(image.data(), 1536 + ondisk::agfl::kMagic, ondisk::kAgflMagic);
    PutBE32(image.data(), 1536 + ondisk::agfl::kSeqno, 0);
    PutBE64(image.data(), 1536 + ondisk::agfl::kLsn, 0);
    UpdateXfsChecksum(image.data() + 1536, 512, ondisk::agfl::kCrc);

    Exercise(image, CountFrom(data, size, AgflSize(fuzz::FuzzGeometry())));
  }
  return 0;
}
