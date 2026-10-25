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
// xlog_rec_ext_header: cycle word plus 64 saved data-block words. Only these
// 260 bytes are covered by the record CRC (not the 252 reserved bytes).
constexpr size_t kRecExtCycle = 0;    // be32
constexpr size_t kRecExtData = 4;     // be32[64]
constexpr size_t kRecExtSize = kRecExtData + kRecCycleDataCount * 4;  // 260
// Linux XLOG_MAX_RECORD_BSIZE. A transaction this build emits is a single
// legal Linux log record; larger metadata sets are split by the caller before
// commit rather than emitted as an unsupported record.
constexpr uint32_t kMaxRecordBytes = 256 * 1024;
constexpr uint32_t kMaxIclogBytes = 256 * 1024;

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
// XFS_LI_* item types (xfs_log_format.h). Recovery classifies every item it
// finds: the ones this build replays, the intent items, and the refused ones.
constexpr uint16_t kItemEfi = 0x1236;       // XFS_LI_EFI
constexpr uint16_t kItemEfd = 0x1237;       // XFS_LI_EFD
constexpr uint16_t kItemIunlink = 0x1238;   // XFS_LI_IUNLINK
constexpr uint16_t kItemInode = 0x123b;     // XFS_LI_INODE
constexpr uint16_t kItemBuf = 0x123c;       // XFS_LI_BUF
constexpr uint16_t kItemDquot = 0x123d;     // XFS_LI_DQUOT
constexpr uint16_t kItemQuotaoff = 0x123e;  // XFS_LI_QUOTAOFF
constexpr uint16_t kItemIcreate = 0x123f;   // XFS_LI_ICREATE
constexpr uint16_t kItemRui = 0x1240;       // XFS_LI_RUI
constexpr uint16_t kItemRud = 0x1241;       // XFS_LI_RUD
constexpr uint16_t kItemCui = 0x1242;       // XFS_LI_CUI
constexpr uint16_t kItemCud = 0x1243;       // XFS_LI_CUD
constexpr uint16_t kItemBui = 0x1244;       // XFS_LI_BUI
constexpr uint16_t kItemBud = 0x1245;       // XFS_LI_BUD
constexpr uint16_t kItemAttri = 0x1246;     // XFS_LI_ATTRI
constexpr uint16_t kItemAttrd = 0x1247;     // XFS_LI_ATTRD

// ---------------------------------------------------------------------------
// Intent item formats (host byte order, like every log item format)
// ---------------------------------------------------------------------------
// xfs_efi_log_format: type u16, size u16 (= regions), nextents u32, id u64,
// then xfs_extent[] (all host order). EFD carries the id of the EFI it
// completes, BUI/BUD the same for bmap updates.
constexpr size_t kIntentLogFormatSize = 16;  // header before the extent array
constexpr size_t kIntentIdOffset = 8;        // u64 efi_id / bui_id / ...
constexpr size_t kIntentNextentsOffset = 4;  // u32
constexpr size_t kExtentSize = 16;           // xfs_extent: start u64, len u32
constexpr size_t kExtentStartOffset = 0;     // u64
constexpr size_t kExtentLenOffset = 8;       // u32

// xfs_attri_log_format: type u16, size u16, igen u32, id u64, ino u64,
// op_flags u32, name_len u32, value_len u32, attr_filter u32.  ATTRD is a
// fixed 16-byte item with its intent id at offset 8.
constexpr size_t kAttriFormatSize = 40;
constexpr size_t kAttrdFormatSize = 16;
constexpr size_t kAttriIdOffset = 8;
constexpr size_t kAttriInoOffset = 16;
constexpr size_t kAttriIgenOffset = 4;
constexpr size_t kAttriOpOffset = 24;
constexpr size_t kAttriNameLenOffset = 28;
constexpr size_t kAttriValueLenOffset = 32;
constexpr size_t kAttriFilterOffset = 36;
constexpr size_t kAttrdIdOffset = 8;
constexpr uint32_t kAttriFilterMask =
    (1u << 1) | (1u << 2) | (1u << 3) | (1u << 7);
constexpr uint32_t kAttriOpMask = 0xff;

// xfs_rui_log_format: type u16, size u16, nextents u32, rui_id u64, then
// xfs_map_extent[] (all host order).  RUD is a fixed 16-byte item.
constexpr size_t kRuiFormatSize = 16;
constexpr size_t kRuiExtentSize = 32;
constexpr size_t kRuiOwnerOffset = 0;       // u64
constexpr size_t kRuiStartblockOffset = 8;  // u64 (encoded fsblock)
constexpr size_t kRuiStartoffOffset = 16;   // u64
constexpr size_t kRuiLenOffset = 24;        // u32
constexpr size_t kRuiFlagsOffset = 28;      // u32
constexpr size_t kRudFormatSize = 16;

// xfs_map_extent.me_flags: low byte is the operation, upper bits are flags.
constexpr uint32_t kRmapExtentTypeMask = 0xff;
constexpr uint32_t kRmapExtentMap = 1;
constexpr uint32_t kRmapExtentMapShared = 2;
constexpr uint32_t kRmapExtentUnmap = 3;
constexpr uint32_t kRmapExtentUnmapShared = 4;
constexpr uint32_t kRmapExtentConvert = 5;
constexpr uint32_t kRmapExtentConvertShared = 6;
constexpr uint32_t kRmapExtentAlloc = 7;
constexpr uint32_t kRmapExtentFree = 8;
constexpr uint32_t kRmapExtentAttrFork = 1u << 31;
constexpr uint32_t kRmapExtentBmbtBlock = 1u << 30;
constexpr uint32_t kRmapExtentUnwritten = 1u << 29;
constexpr uint32_t kRmapExtentFlags =
    kRmapExtentTypeMask | kRmapExtentAttrFork | kRmapExtentBmbtBlock |
    kRmapExtentUnwritten;

// xfs_icreate_log, 28 bytes, host endian type/size then big-endian fields.
// Replay calls xfs_ialloc_inode_init, which rebuilds the whole inode cluster
// from scratch: exactly what a newly allocated chunk needs.
constexpr size_t kIcreateLogSize = 28;
constexpr size_t kIclType = 0;     // u16
constexpr size_t kIclSize = 2;     // u16: 1 (single vector)
constexpr size_t kIclAg = 4;       // be32
constexpr size_t kIclAgbno = 8;    // be32
constexpr size_t kIclCount = 12;   // be32
constexpr size_t kIclIsize = 16;   // be32
constexpr size_t kIclLength = 20;  // be32
constexpr size_t kIclGen = 24;     // be32

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

// XFS_BLF_* flags in the low bits of blf_flags.
constexpr uint16_t kBlfInodeBuf = 1u << 0;  // XFS_BLF_INODE_BUF
constexpr uint16_t kBlfCancel = 1u << 1;    // XFS_BLF_CANCEL
constexpr uint16_t kBlfUdquot = 1u << 2;    // XFS_BLF_UDQUOT_BUF
constexpr uint16_t kBlfPdquot = 1u << 3;    // XFS_BLF_PDQUOT_BUF
constexpr uint16_t kBlfGdquot = 1u << 4;    // XFS_BLF_GDQUOT_BUF

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
