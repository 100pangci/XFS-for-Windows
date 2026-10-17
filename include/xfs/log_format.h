// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - internal log (journal) on-disk format.
//
// Everything here was verified against the kernel sources (fs/xfs, see
// docs/JOURNAL.md) and matches the "dirty log written in incompatible format"
// check: we emit the Linux little-endian format.
//
// Byte order is mixed, exactly as upstream:
//   - log record header, op header, and everything that is on-disk metadata:
//     big-endian;
//   - item format headers (xfs_trans_header, xfs_inode_log_format,
//     xfs_buf_log_format) and the inode log dinode: host byte order. We write
//     little-endian explicitly, which is what h_fmt = XLOG_FMT_LINUX_LE says.
#pragma once

#include <cstddef>
#include <cstdint>

namespace xfs::logfmt {

// ---------------------------------------------------------------------------
// Log record header (xlog_rec_header), 512 bytes, big-endian fields
// ---------------------------------------------------------------------------
constexpr uint32_t kRecordMagic = 0xFEEDBABE;
constexpr size_t kRecordHeaderSize = 512;
constexpr size_t kRecMagic = 0;          // be32
constexpr size_t kRecCycle = 4;          // be32
constexpr size_t kRecVersion = 8;        // be32
constexpr size_t kRecLen = 12;           // be32: data bytes after the header
constexpr size_t kRecLsn = 16;           // be64
constexpr size_t kRecTailLsn = 24;       // be64
constexpr size_t kRecCrc = 32;           // le32
constexpr size_t kRecPrevBlock = 36;     // be32
constexpr size_t kRecNumLogOps = 40;     // be32
constexpr size_t kRecCycleData = 44;     // be32[64]
constexpr size_t kRecCycleDataCount = 64;
constexpr size_t kRecFmt = 300;          // be32
constexpr size_t kRecUuid = 304;         // 16 bytes
constexpr size_t kRecSize = 320;         // be32: iclog size
constexpr size_t kRecPad0 = 324;         // u32 padding
// Bytes of the header covered by the record CRC (XLOG_REC_SIZE for the
// non-i386 layout, which includes h_size).
constexpr size_t kRecCrcCovered = 328;
constexpr uint32_t kVersion2 = 2;
constexpr uint32_t kFmtLinuxLe = 1;
constexpr uint32_t kDefaultRecordSize = 32 * 1024;  // h_size (one header block)
// One cycle word per 512-byte log block; the first 64 live in the record
// header, records larger than 32 KiB use xlog_rec_ext_header blocks.
constexpr uint32_t kCycleBlockSize = 512;
constexpr uint32_t kHeaderCycleBytes = kRecCycleDataCount * kCycleBlockSize;

// ---------------------------------------------------------------------------
// Log operation header (xlog_op_header), 12 bytes, big-endian fields
// ---------------------------------------------------------------------------
constexpr size_t kOpHeaderSize = 12;
constexpr size_t kOpTid = 0;        // be32
constexpr size_t kOpLen = 4;        // be32: payload bytes after this header
constexpr size_t kOpClientId = 8;   // u8
constexpr size_t kOpFlags = 9;      // u8
constexpr size_t kOpRes2 = 10;      // be16

constexpr uint8_t kFlagStartTrans = 0x01;
constexpr uint8_t kFlagCommitTrans = 0x02;
constexpr uint8_t kFlagContinueTrans = 0x04;
constexpr uint8_t kFlagWasContTrans = 0x08;
constexpr uint8_t kFlagEndTrans = 0x10;
constexpr uint8_t kFlagUnmountTrans = 0x20;

constexpr uint8_t kClientTransaction = 0x69;  // XFS_TRANSACTION
constexpr uint8_t kClientLog = 0xaa;          // XFS_LOG (unmount record)

// ---------------------------------------------------------------------------
// Transaction header (xfs_trans_header), 16 bytes, host byte order
// ---------------------------------------------------------------------------
constexpr size_t kTransHeaderSize = 16;
constexpr uint32_t kTransHeaderMagic = 0x5452414e;  // 'TRAN'
constexpr uint32_t kTransTypeCheckpoint = 40;       // XFS_TRANS_CHECKPOINT
constexpr size_t kThMagic = 0;   // u32
constexpr size_t kThType = 4;    // u32
constexpr size_t kThTid = 8;     // s32
constexpr size_t kThNumItems = 12;  // u32

// ---------------------------------------------------------------------------
// Item types and item format headers, host byte order
// ---------------------------------------------------------------------------
constexpr uint16_t kItemInode = 0x123b;  // XFS_LI_INODE
constexpr uint16_t kItemBuf = 0x123c;    // XFS_LI_BUF

// xfs_inode_log_format, 56 bytes
constexpr size_t kInodeLogFormatSize = 56;
constexpr size_t kIlfType = 0;      // u16
constexpr size_t kIlfSize = 2;      // u16: item size in 32-bit words
constexpr size_t kIlfFields = 4;    // u32: XFS_ILOG_*
constexpr size_t kIlfAsize = 8;     // u16
constexpr size_t kIlfDsize = 10;    // u16
constexpr size_t kIlfPad = 12;      // u32
constexpr size_t kIlfIno = 16;      // u64
constexpr size_t kIlfU = 24;        // union, 16 bytes
constexpr size_t kIlfBlkno = 40;    // s64: daddr (512-byte units)
constexpr size_t kIlfLen = 48;      // s32: buffer length in 512-byte units
constexpr size_t kIlfBoffset = 52;  // s32: inode byte offset in that buffer

// XFS_ILOG_* flags (which parts of the inode are in the item)
constexpr uint32_t kILogCore = 0x001;
constexpr uint32_t kILogDData = 0x002;
constexpr uint32_t kILogDExt = 0x004;
constexpr uint32_t kILogDBRoot = 0x008;
constexpr uint32_t kILogDev = 0x010;
constexpr uint32_t kILogAData = 0x040;
constexpr uint32_t kILogAExt = 0x080;
constexpr uint32_t kILogABRoot = 0x100;

// struct xfs_log_dinode: same field offsets as the on-disk v3 inode core,
// but every integer field is in host (little-endian) byte order. 176 bytes.
constexpr size_t kLogDinodeSize = 176;

// ---------------------------------------------------------------------------
// Buffer item (xfs_buf_log_format), host byte order
// ---------------------------------------------------------------------------
// | offset | field | notes |
// |---|---|---|
// | 0 | blf_type | XFS_LI_BUF |
// | 2 | blf_size | 1 + number of contiguous logged regions |
// | 4 | blf_flags | buffer type in bits 11..15 (XFS_BLFT_*), host order |
// | 6 | blf_len | buffer length in 512-byte units |
// | 8 | blf_blkno | daddr (512-byte units) |
// | 16 | blf_map_size | number of used words of the data bitmap |
// | 20 | blf_data_map[] | one bit per 128-byte chunk, host byte order |
//
// The logged chunks follow, one log operation per contiguous run of set bits.
constexpr size_t kBufLogFormatSize = 20;
constexpr size_t kBufMapOffset = 20;
constexpr size_t kBufChunkSize = 128;  // XFS_BLF_CHUNK
constexpr size_t kBufChunkShift = 7;   // XFS_BLF_SHIFT

// XFS_BLFT_* buffer types (bits 11..15 of blf_flags), from xfs_log_format.h.
constexpr uint16_t kBlftUnknown = 0;
constexpr uint16_t kBlftBtree = 4;
constexpr uint16_t kBlftAgf = 5;
constexpr uint16_t kBlftAgfl = 6;
constexpr uint16_t kBlftAgi = 7;
constexpr uint16_t kBlftDino = 8;
constexpr uint16_t kBlftSymlink = 9;
constexpr uint16_t kBlftDirBlock = 10;
constexpr uint16_t kBlftDirData = 11;
constexpr uint16_t kBlftDirFree = 12;
constexpr uint16_t kBlftDirLeaf1 = 13;
constexpr uint16_t kBlftDirLeafn = 14;
constexpr uint16_t kBlftDaNode = 15;
constexpr uint16_t kBlftAttrLeaf = 16;
constexpr uint16_t kBlftAttrRmt = 17;
constexpr uint16_t kBlftSb = 18;
constexpr size_t kBlftShift = 11;
constexpr size_t kBlftMask = 0x1f;

// ---------------------------------------------------------------------------
// Clean-unmount record payload (xfs_unmount_log_format), 8 bytes
// ---------------------------------------------------------------------------
constexpr size_t kUnmountFormatSize = 8;
constexpr uint16_t kUnmountType = 0x556e;  // 'Un'

}  // namespace xfs::logfmt
