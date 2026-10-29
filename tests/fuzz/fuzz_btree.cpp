// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// libFuzzer target: standard XFS btree block verification and walking.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xfs/btree.h"
#include "xfs/crc32c.h"
#include "xfs/endian.h"
#include "xfs/ondisk.h"

#include "fuzz_support.h"

namespace {

using namespace xfs;

constexpr uint32_t kBlocks = 256;

BtreeSpec ShortSpec() {
  BtreeSpec spec;
  spec.name = "fuzz-short";
  spec.format = BtreeFormat::kShort;
  spec.block_size = 4096;
  spec.key_size = ondisk::btree::kAllocKeySize;
  spec.ptr_size = ondisk::btree::kAllocPtrSize;
  spec.rec_size = ondisk::btree::kAllocRecSize;
  spec.magic = ondisk::kBtbCrcMagic;
  spec.null_ptr = ondisk::btree::kNullAgBlock;
  spec.uuid = fuzz::kFuzzUuid;
  spec.verify_identity = false;
  return spec;
}

BtreeSpec LongSpec() {
  BtreeSpec spec;
  spec.name = "fuzz-long";
  spec.format = BtreeFormat::kLong;
  spec.block_size = 4096;
  spec.key_size = ondisk::btree::kBmbtKeySize;
  spec.ptr_size = ondisk::btree::kBmbtPtrSize;
  spec.rec_size = ondisk::btree::kBmbtRecSize;
  spec.magic = ondisk::kBmapCrcMagic;
  spec.null_ptr = 0;
  spec.owner_size = 8;
  spec.uuid = fuzz::kFuzzUuid;
  spec.verify_identity = false;
  return spec;
}

void Walk(const BtreeSpec& spec, const std::vector<uint8_t>& block) {
  fuzz::MemoryDevice device(kBlocks * 4096);
  for (uint32_t i = 0; i < kBlocks; ++i) {
    std::memcpy(device.bytes().data() + i * 4096, block.data(), block.size());
  }
  BtreeWalkCallbacks callbacks;
  callbacks.ptr_in_range = [](uint64_t ptr) { return ptr < kBlocks; };
  callbacks.read_block = [&device](uint64_t ptr, std::vector<uint8_t>* out) {
    if (ptr >= kBlocks) return false;
    out->assign(4096, 0);
    std::memcpy(out->data(), device.bytes().data() + ptr * 4096, 4096);
    return true;
  };
  callbacks.visit = [&spec](uint64_t, const BtreeBlockMeta& meta,
                            const uint8_t* buf) -> Status {
    const bool leaf = meta.level == 0;
    const size_t max = BtreeMaxRecords(spec, leaf);
    for (size_t i = 0; i < meta.numrecs && i < max; ++i) {
      if (leaf) {
        (void)BtreeLeafRec(buf, meta, spec, i);
      } else {
        (void)BtreeKeyValue(buf, meta, spec, i);
        (void)BtreeChildPtr(buf, meta, spec, i);
      }
    }
    return OkStatus();
  };
  (void)WalkBtree(spec, 0, callbacks);
}

void Exercise(const std::vector<uint8_t>& input, const BtreeSpec& spec) {
  // Short buffers must be rejected without reading past the input.
  {
    BtreeBlockMeta short_meta;
    (void)VerifyBtreeBlock(input.data(), input.size(), spec, &short_meta);
  }
  std::vector<uint8_t> block(4096, 0);
  fuzz::CopyInput(input.data(), input.size(), 0, &block);
  PutBE32(block.data(), ondisk::btree::kMagic, spec.magic);
  BtreeBlockMeta meta;
  if (VerifyBtreeBlock(block.data(), block.size(), spec, &meta).ok()) {
    Walk(spec, block);
  }

  // Identity pass: repair UUID/LSN/block number/owner and CRC so the
  // verification path is exercised, not just the structural parser.
  BtreeSpec identity = spec;
  identity.verify_identity = true;
  identity.expected_daddr = 0;
  identity.expected_owner = spec.format == BtreeFormat::kShort ? 0 : 128;
  identity.sb_lsn = 0;
  std::vector<uint8_t> checked = block;
  const size_t uuid_off = spec.format == BtreeFormat::kShort
                              ? ondisk::btree::kSUuid
                              : ondisk::btree::kLUuid;
  const size_t lsn_off = spec.format == BtreeFormat::kShort
                             ? ondisk::btree::kSLsn
                             : ondisk::btree::kLLsn;
  const size_t blkno_off = spec.format == BtreeFormat::kShort
                               ? ondisk::btree::kSBlkno
                               : ondisk::btree::kLBlkno;
  const size_t owner_off = spec.format == BtreeFormat::kShort
                               ? ondisk::btree::kSOwner
                               : ondisk::btree::kLOwner;
  const size_t crc_off = spec.format == BtreeFormat::kShort
                             ? ondisk::btree::kSCrc
                             : ondisk::btree::kLCrc;
  std::memcpy(checked.data() + uuid_off, fuzz::kFuzzUuid.data(), 16);
  PutBE64(checked.data(), lsn_off, 0);
  PutBE64(checked.data(), blkno_off, 0);
  if (spec.format == BtreeFormat::kShort) {
    PutBE32(checked.data(), owner_off, 0);
  } else {
    PutBE64(checked.data(), owner_off, 128);
  }
  UpdateXfsChecksum(checked.data(), checked.size(), crc_off);
  (void)VerifyBtreeBlock(checked.data(), checked.size(), identity, &meta);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::vector<uint8_t> input;
  if (size != 0) input.assign(data, data + size);
  Exercise(input, ShortSpec());
  Exercise(input, LongSpec());
  return 0;
}
