// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XFS for Windows - on-disk format definitions (XFS v5 / crc=1).
//
// All layouts here follow the public XFS on-disk format. Fields are accessed
// through explicit byte offsets with big-endian helpers (see endian.h); no
// on-disk buffer is ever reinterpret_cast to a host struct.
//
// Everything in this file was implemented from the documented format and
// cross-checked against the offset/size assertions in the upstream
// xfs_ondisk.h. The offset constants below carry those values as comments.
#pragma once

#include <cstddef>
#include <cstdint>

namespace xfs::ondisk {

// ---------------------------------------------------------------------------
// Magic numbers
// ---------------------------------------------------------------------------
constexpr uint32_t kSbMagic = 0x58465342;    // 'XFSB'
constexpr uint32_t kAgfMagic = 0x58414746;   // 'XAGF'
constexpr uint32_t kAgiMagic = 0x58414749;   // 'XAGI'
constexpr uint32_t kAgflMagic = 0x5841464c;  // 'XAFL'
constexpr uint16_t kDinodeMagic = 0x494e;    // 'IN'

constexpr uint32_t kBtbMagic = 0x41425442;    // 'ABTB' bnobt (v4)
constexpr uint32_t kBtbCrcMagic = 0x41423342; // 'AB3B' bnobt (v5)
constexpr uint32_t kBtcMagic = 0x41425443;    // 'ABTC' cntbt (v4)
constexpr uint32_t kBtcCrcMagic = 0x41423343; // 'AB3C' cntbt (v5)
constexpr uint32_t kIbtMagic = 0x49414254;    // 'IABT' inobt (v4)
constexpr uint32_t kIbtCrcMagic = 0x49414233; // 'IAB3' inobt (v5)
constexpr uint32_t kFibtMagic = 0x46494254;   // 'FIBT' finobt (v4)
constexpr uint32_t kFibtCrcMagic = 0x46494233;// 'FIB3' finobt (v5)
constexpr uint32_t kBmapMagic = 0x424d4150;   // 'BMAP' bmbt (v4)
constexpr uint32_t kBmapCrcMagic = 0x424d4133;// 'BMA3' bmbt (v5)
constexpr uint32_t kRmapCrcMagic = 0x524d4233; // 'RMB3' rmapbt (v5)
constexpr uint32_t kRefcountCrcMagic = 0x52334643; // 'R3FC' refcountbt (v5)

constexpr uint16_t kDaNodeMagic = 0xfebe;    // node block (v4)
constexpr uint16_t kDaNode3Magic = 0x3ebe;   // node block (v5)
constexpr uint16_t kDir2Leaf1Magic = 0xd2f1; // single leaf (v4)
constexpr uint16_t kDir3Leaf1Magic = 0x3df1; // single leaf (v5)
constexpr uint16_t kDir2LeafnMagic = 0xd2ff;
constexpr uint16_t kDir3LeafnMagic = 0x3dff;
constexpr uint16_t kAttrLeafMagic = 0xfbee;
constexpr uint16_t kAttr3LeafMagic = 0x3bee;
constexpr uint32_t kDir2BlockMagic = 0x58443242;  // 'XD2B' single block dir (v4)
constexpr uint32_t kDir3BlockMagic = 0x58444233;  // 'XDB3' single block dir (v5)
constexpr uint32_t kDir2DataMagic = 0x58443244;   // 'XD2D' data block (v4)
constexpr uint32_t kDir3DataMagic = 0x58444433;   // 'XDD3' data block (v5)
constexpr uint32_t kDir2FreeMagic = 0x58443246;   // 'XD2F' free block (v4)
constexpr uint32_t kDir3FreeMagic = 0x58444633;   // 'XDF3' free block (v5)
constexpr uint32_t kSymlinkMagic = 0x58534c4d;    // 'XSLM'
constexpr uint32_t kAttr3RmtMagic = 0x5841524d;   // 'XARM'

// ---------------------------------------------------------------------------
// Superblock (struct xfs_dsb, 304 bytes on disk)
// ---------------------------------------------------------------------------
namespace sb {
constexpr size_t kSize = 304;
constexpr size_t kMagic = 0;                 // be32
constexpr size_t kBlocksize = 4;             // be32
constexpr size_t kDblocks = 8;               // be64
constexpr size_t kRblocks = 16;              // be64
constexpr size_t kRextents = 24;             // be64
constexpr size_t kUuid = 32;                 // 16 bytes
constexpr size_t kLogstart = 48;             // be64
constexpr size_t kRootino = 56;              // be64
constexpr size_t kRbmino = 64;               // be64
constexpr size_t kRsumino = 72;              // be64
constexpr size_t kRextsize = 80;             // be32
constexpr size_t kAgblocks = 84;             // be32
constexpr size_t kAgcount = 88;              // be32
constexpr size_t kRbmblocks = 92;            // be32
constexpr size_t kLogblocks = 96;            // be32
constexpr size_t kVersionnum = 100;          // be16
constexpr size_t kSectsize = 102;            // be16
constexpr size_t kInodesize = 104;           // be16
constexpr size_t kInopblock = 106;           // be16
constexpr size_t kFname = 108;               // 12 bytes, not NUL terminated
constexpr size_t kBlocklog = 120;            // u8
constexpr size_t kSectlog = 121;             // u8
constexpr size_t kInodelog = 122;            // u8
constexpr size_t kInopblog = 123;            // u8
constexpr size_t kAgblklog = 124;            // u8
constexpr size_t kRextslog = 125;            // u8
constexpr size_t kInprogress = 126;          // u8
constexpr size_t kImaxPct = 127;             // u8
constexpr size_t kIcount = 128;              // be64
constexpr size_t kIfree = 136;               // be64
constexpr size_t kFdblocks = 144;            // be64
constexpr size_t kFrextents = 152;           // be64
constexpr size_t kUquotino = 160;            // be64
constexpr size_t kGquotino = 168;            // be64
constexpr size_t kQflags = 176;              // be16
constexpr size_t kFlags = 178;               // u8
constexpr size_t kSharedVn = 179;            // u8
constexpr size_t kInoalignmt = 180;          // be32
constexpr size_t kUnit = 184;                // be32
constexpr size_t kWidth = 188;               // be32
constexpr size_t kDirblklog = 192;           // u8
constexpr size_t kLogsectlog = 193;          // u8
constexpr size_t kLogsectsize = 194;         // be16
constexpr size_t kLogsunit = 196;            // be32
constexpr size_t kFeatures2 = 200;           // be32
constexpr size_t kBadFeatures2 = 204;        // be32
constexpr size_t kFeaturesCompat = 208;      // be32
constexpr size_t kFeaturesRoCompat = 212;    // be32
constexpr size_t kFeaturesIncompat = 216;    // be32
constexpr size_t kFeaturesLogIncompat = 220; // be32
constexpr size_t kCrc = 224;                 // le32  (XFS_SB_CRC_OFF)
constexpr size_t kSpinoAlign = 228;          // be32
constexpr size_t kPquotino = 232;            // be64
constexpr size_t kLsn = 240;                 // be64
constexpr size_t kMetaUuid = 248;            // 16 bytes
}  // namespace sb

// versionnum bit fields
constexpr uint16_t kSbVersionNumbits = 0x000f;
constexpr uint16_t kSbVersion5 = 5;
// An attr fork may only exist when this bit is set (XFS_SB_VERSION_ATTRBIT).
// xfs_scrub reports "attr block map: Repairs are required" for every inode
// with an attr fork otherwise, even though xfs_repair accepts the image.
constexpr uint16_t kSbVersionAttrBit = 0x0010;

// features2 bits (v4 compatibility area; XFS_SB_VERSION2_*)
constexpr uint32_t kVersion2LazySbCount = 0x00000002;
constexpr uint32_t kVersion2Attr2 = 0x00000008;
constexpr uint32_t kVersion2Projid32 = 0x00000080;
constexpr uint32_t kVersion2Crc = 0x00000100;
constexpr uint32_t kVersion2Ftype = 0x00000200;

// v5 ro-compat feature bits (XFS_SB_FEAT_RO_COMPAT_*)
constexpr uint32_t kRoCompatFinobt = 1u << 0;
constexpr uint32_t kRoCompatRmapbt = 1u << 1;
constexpr uint32_t kRoCompatReflink = 1u << 2;
constexpr uint32_t kRoCompatInobtcnt = 1u << 3;
constexpr uint32_t kRoCompatKnown =
    kRoCompatFinobt | kRoCompatRmapbt | kRoCompatReflink | kRoCompatInobtcnt;

// v5 incompat feature bits (XFS_SB_FEAT_INCOMPAT_*)
constexpr uint32_t kIncompatFtype = 1u << 0;
constexpr uint32_t kIncompatSpinodes = 1u << 1;
constexpr uint32_t kIncompatMetaUuid = 1u << 2;
constexpr uint32_t kIncompatBigtime = 1u << 3;
constexpr uint32_t kIncompatNeedsRepair = 1u << 4;
constexpr uint32_t kIncompatNrext64 = 1u << 5;
constexpr uint32_t kIncompatExchrange = 1u << 6;
constexpr uint32_t kIncompatParent = 1u << 7;
constexpr uint32_t kIncompatMetadir = 1u << 8;
constexpr uint32_t kIncompatZoned = 1u << 9;
constexpr uint32_t kIncompatZoneGaps = 1u << 10;

// v5 log incompat feature bits
constexpr uint32_t kLogIncompatXattrs = 1u << 0;

// ---------------------------------------------------------------------------
// AGF (struct xfs_agf, 224 bytes used; buffer is one sector)
// ---------------------------------------------------------------------------
namespace agf {
constexpr size_t kMagic = 0;          // be32
constexpr size_t kVersionnum = 4;     // be32
constexpr size_t kSeqno = 8;          // be32
constexpr size_t kLength = 12;        // be32
constexpr size_t kBnoRoot = 16;       // be32
constexpr size_t kCntRoot = 20;       // be32
constexpr size_t kRmapRoot = 24;      // be32
constexpr size_t kBnoLevel = 28;      // be32
constexpr size_t kCntLevel = 32;      // be32
constexpr size_t kRmapLevel = 36;     // be32
constexpr size_t kFlfirst = 40;       // be32
constexpr size_t kFllast = 44;        // be32
constexpr size_t kFlcount = 48;       // be32
constexpr size_t kFreeblks = 52;      // be32
constexpr size_t kLongest = 56;       // be32
constexpr size_t kBtreeblks = 60;     // be32
constexpr size_t kUuid = 64;          // 16 bytes
constexpr size_t kRmapBlocks = 80;    // be32
constexpr size_t kRefcountBlocks = 84; // be32
constexpr size_t kRefcountRoot = 88;  // be32
constexpr size_t kRefcountLevel = 92; // be32
// be64 agf_spare64[14] follows (96..207)
constexpr size_t kLsn = 208;          // be64
constexpr size_t kCrc = 216;          // be32 (le32 CRC)
constexpr size_t kSpare2 = 220;       // be32
constexpr size_t kVersion = 1;
}  // namespace agf

// ---------------------------------------------------------------------------
// AGI (struct xfs_agi, 344 bytes used; buffer is one sector)
// ---------------------------------------------------------------------------
namespace agi {
constexpr size_t kMagic = 0;          // be32
constexpr size_t kVersionnum = 4;     // be32
constexpr size_t kSeqno = 8;          // be32
constexpr size_t kLength = 12;        // be32
constexpr size_t kCount = 16;         // be32
constexpr size_t kRoot = 20;          // be32
constexpr size_t kLevel = 24;         // be32
constexpr size_t kFreecount = 28;     // be32
constexpr size_t kNewino = 32;        // be32
constexpr size_t kDirino = 36;        // be32
constexpr size_t kUnlinked = 40;      // be32[64]
constexpr size_t kUnlinkedBuckets = 64;
constexpr size_t kUuid = 296;         // 16 bytes
constexpr size_t kCrc = 312;          // le32
constexpr size_t kPad32 = 316;        // be32
constexpr size_t kLsn = 320;          // be64
constexpr size_t kFreeRoot = 328;     // be32
constexpr size_t kFreeLevel = 332;    // be32
constexpr size_t kIblocks = 336;      // be32
constexpr size_t kFblocks = 340;      // be32
constexpr size_t kVersion = 1;
}  // namespace agi

// ---------------------------------------------------------------------------
// AGFL (header only; entries are be32 array directly after it)
// ---------------------------------------------------------------------------
namespace agfl {
constexpr size_t kMagic = 0;      // be32
constexpr size_t kSeqno = 4;      // be32
constexpr size_t kUuid = 8;       // 16 bytes
constexpr size_t kLsn = 24;       // be64
constexpr size_t kCrc = 32;       // le32
constexpr size_t kHeaderSize = 36;
// v5 entries: (sectsize - 36) / 4. v4 (unsupported): sectsize / 4.
}  // namespace agfl

// ---------------------------------------------------------------------------
// Inode (struct xfs_dinode, v3 core is 176 bytes)
// ---------------------------------------------------------------------------
namespace inode {
// Field order and offsets follow struct xfs_dinode (v3 core = 176 bytes).
constexpr size_t kCoreSizeV3 = 176;
constexpr size_t kCoreSizeV2 = 100;   // v2 core ends before di_crc
constexpr size_t kMagic = 0;          // be16
constexpr size_t kMode = 2;           // be16
constexpr size_t kVersion = 4;        // u8
constexpr size_t kFormat = 5;         // u8
constexpr size_t kOnlink = 6;         // be16 (v2 only; di_metatype in 7.x)
constexpr size_t kUid = 8;            // be32
constexpr size_t kGid = 12;           // be32
constexpr size_t kNlink = 16;         // be32
constexpr size_t kProjidLo = 20;      // be16
constexpr size_t kProjidHi = 22;      // be16
constexpr size_t kBigNextents = 24;   // be64 when NREXT64, else pad
constexpr size_t kV3Pad = 24;         // be64 padding
constexpr size_t kAtime = 32;         // be64 timestamp
constexpr size_t kMtime = 40;         // be64 timestamp
constexpr size_t kCtime = 48;         // be64 timestamp
constexpr size_t kSize = 56;          // be64
constexpr size_t kNblocks = 64;       // be64
constexpr size_t kExtsize = 72;       // be32
constexpr size_t kNextents = 76;      // be32 (without NREXT64)
constexpr size_t kAnextents = 80;     // be16 (without NREXT64)
constexpr size_t kBigAnextents = 76;  // be32 when NREXT64
constexpr size_t kForkoff = 82;       // u8, attr fork offset in 8-byte units
constexpr size_t kAformat = 83;       // s8
constexpr size_t kDmevmask = 84;      // be32
constexpr size_t kDmstate = 88;       // be16
constexpr size_t kFlags = 90;         // be16
constexpr size_t kGen = 92;           // be32
constexpr size_t kNextUnlinked = 96;  // be32
constexpr size_t kCrc = 100;          // le32 (XFS_DINODE_CRC_OFF)
constexpr size_t kChangecount = 104;  // be64
constexpr size_t kLsn = 112;          // be64
constexpr size_t kFlags2 = 120;       // be64
constexpr size_t kCowextsize = 128;   // be32
constexpr size_t kPad2 = 132;         // 12 bytes
constexpr size_t kCrtime = 144;       // be64
constexpr size_t kIno = 152;          // be64
constexpr size_t kUuid = 160;         // 16 bytes

// di_format values
constexpr uint8_t kFormatDev = 0;
constexpr uint8_t kFormatLocal = 1;
constexpr uint8_t kFormatExtents = 2;
constexpr uint8_t kFormatBtree = 3;
constexpr uint8_t kFormatUuid = 4;
constexpr uint8_t kFormatMetaBtree = 5;

// di_flags bits
constexpr uint16_t kDiFlagRealtime = 1u << 0;
constexpr uint16_t kDiFlagPrealloc = 1u << 1;
constexpr uint16_t kDiFlagImmutable = 1u << 3;
constexpr uint16_t kDiFlagAppend = 1u << 4;
constexpr uint16_t kDiFlagSync = 1u << 5;
constexpr uint16_t kDiFlagNoatime = 1u << 6;
constexpr uint16_t kDiFlagNodump = 1u << 7;
constexpr uint16_t kDiFlagRtinherit = 1u << 8;
constexpr uint16_t kDiFlagProjinherit = 1u << 9;
constexpr uint16_t kDiFlagNosymlinks = 1u << 10;
constexpr uint16_t kDiFlagExtsize = 1u << 11;
constexpr uint16_t kDiFlagExtszinherit = 1u << 12;
constexpr uint16_t kDiFlagNodefrag = 1u << 13;
constexpr uint16_t kDiFlagFilestream = 1u << 14;

// di_flags2 bits
constexpr uint64_t kDiFlag2Dax = 1ULL << 0;
constexpr uint64_t kDiFlag2Reflink = 1ULL << 1;
constexpr uint64_t kDiFlag2Cowextsize = 1ULL << 2;
constexpr uint64_t kDiFlag2Bigtime = 1ULL << 3;
constexpr uint64_t kDiFlag2Nrext64 = 1ULL << 4;
constexpr uint64_t kDiFlag2Metadata = 1ULL << 5;
}  // namespace inode

// ---------------------------------------------------------------------------
// Btree blocks
// ---------------------------------------------------------------------------
namespace btree {
constexpr size_t kMagic = 0;      // be32
constexpr size_t kLevel = 4;      // be16
constexpr size_t kNumrecs = 6;    // be16
// short form
constexpr size_t kSLeftsib = 8;   // be32
constexpr size_t kSRightSib = 12; // be32
constexpr size_t kSBlkno = 16;    // be64
constexpr size_t kSLsn = 24;      // be64
constexpr size_t kSUuid = 32;     // 16 bytes
constexpr size_t kSOwner = 48;    // be32
constexpr size_t kSCrc = 52;      // le32
constexpr size_t kSHeaderLenV4 = 16;
constexpr size_t kSHeaderLenV5 = 56;
// long form
constexpr size_t kLLeftsib = 8;   // be64
constexpr size_t kLRightSib = 16; // be64
constexpr size_t kLBlkno = 24;    // be64
constexpr size_t kLLsn = 32;      // be64
constexpr size_t kLUuid = 40;     // 16 bytes
constexpr size_t kLOwner = 56;    // be64
constexpr size_t kLCrc = 64;      // le32
constexpr size_t kLPad = 68;      // be32
constexpr size_t kLHeaderLenV4 = 24;
constexpr size_t kLHeaderLenV5 = 72;

// Hard traversal cap. Per-type checks apply on top of this.
constexpr uint16_t kMaxTraversalLevels = 16;
// Block number meaning "no sibling".
constexpr uint32_t kNullAgBlock = 0xffffffffu;

// Record/key/pointer sizes (bytes), v5 trees.
constexpr size_t kAllocKeySize = 8;   // startblock be32 + blockcount be32
constexpr size_t kAllocPtrSize = 4;   // be32
constexpr size_t kAllocRecSize = 8;   // leaf record == key size
constexpr size_t kInobtKeySize = 4;   // startino be32
constexpr size_t kInobtPtrSize = 4;   // be32
constexpr size_t kInobtRecSize = 16;  // xfs_inobt_rec
// xfs_inobt_rec fields. The 4-byte word at offset 4 is a union:
//   full chunks   : ir_freecount be32
//   sparse chunks : ir_holemask be16, ir_count u8, ir_freecount u8
constexpr size_t kInobtRecStartino = 0;   // be32
constexpr size_t kInobtRecFreecount = 4;  // be32 (full layout)
constexpr size_t kInobtRecHolemask = 4;   // be16 (sparse layout)
constexpr size_t kInobtRecCount = 6;      // u8   (sparse layout)
constexpr size_t kInobtRecSparseFreecount = 7;  // u8 (sparse layout)
constexpr size_t kInobtRecFree = 8;       // be64
constexpr uint32_t kInobtHolemaskBits = 16;
constexpr uint32_t kInodesPerHolemaskBit = 4;
constexpr uint16_t kInobtHolemaskFull = 0;
constexpr size_t kBmbtKeySize = 8;    // startoff be64
constexpr size_t kBmbtPtrSize = 8;    // be64
constexpr size_t kBmbtRecSize = 16;   // xfs_bmbt_rec
constexpr size_t kBmdrSize = 4;       // inode-embedded bmbt root header
// The rmap btree keeps the short (56-byte) block header but packs two
// 20-byte keys (low + high) per node entry before the 4-byte pointers
// (XFS_RMAP_KEY_ADDR / XFS_RMAP_PTR_ADDR).
constexpr size_t kRmapKeySize = 40;   // 2 x xfs_rmap_key (startblock 4 + owner 8 + offset 8)
constexpr size_t kRmapPtrSize = 4;    // be32 agbno
constexpr size_t kRmapRecSize = 24;   // xfs_rmap_rec
// The refcount btree uses the short header, one 4-byte key per node entry and
// a 12-byte leaf record (XFS_REFCOUNT_KEY_ADDR / XFS_REFCOUNT_PTR_ADDR).
constexpr size_t kRefcountKeySize = 4;   // rc_startblock be32
constexpr size_t kRefcountPtrSize = 4;   // be32 agbno
constexpr size_t kRefcountRecSize = 12;  // startblock, blockcount, refcount
}  // namespace btree

// ---------------------------------------------------------------------------
// BMAP extent record (xfs_bmbt_rec, 128 bit packed)
//   bit 127      : extent flag (1 = unwritten)
//   bits 126..73 : startoff (54 bits, fs blocks)
//   bits 72..21  : startblock (52 bits, fs blocks)
//   bits 20..0   : blockcount (21 bits)
// ---------------------------------------------------------------------------
struct BmbtRec {
  uint64_t startoff = 0;    // in fs blocks from start of file
  uint64_t startblock = 0;  // absolute fs block (data device)
  uint64_t blockcount = 0;
  bool unwritten = false;
};

inline BmbtRec DecodeBmbtRec(uint64_t l0_be, uint64_t l1_be) {
  BmbtRec rec;
  rec.unwritten = (l0_be >> 63) != 0;
  rec.startoff = (l0_be >> 9) & ((1ULL << 54) - 1);
  rec.startblock = ((l0_be & 0x1FFu) << 43) | (l1_be >> 21);
  rec.blockcount = l1_be & 0x1FFFFFu;
  return rec;
}

inline void EncodeBmbtRec(const BmbtRec& rec, uint64_t* l0, uint64_t* l1) {
  *l0 = (rec.unwritten ? (1ULL << 63) : 0) |
        ((rec.startoff & ((1ULL << 54) - 1)) << 9) |
        ((rec.startblock >> 43) & 0x1FFu);
  *l1 = ((rec.startblock & ((1ULL << 43) - 1)) << 21) |
        (rec.blockcount & 0x1FFFFFu);
}

constexpr uint64_t kBmbtMaxBlockcount = (1ULL << 21) - 1;
constexpr uint64_t kBmbtMaxStartoff = (1ULL << 54) - 1;

// ---------------------------------------------------------------------------
// Directory structures (v5 / dir3)
// ---------------------------------------------------------------------------
namespace dir {
// Shortform (inode data fork, XFS_DINODE_FMT_LOCAL)
constexpr size_t kSfHdrCount = 0;      // u8 number of entries
constexpr size_t kSfHdrI8Count = 1;    // u8 number of 8-byte inodes
constexpr size_t kSfHdrParent = 2;     // be32 (i8count==0) or be64
constexpr size_t kSfHdrSizeFixed = 10; // with 8-byte parent
// xfs_dir2_sf_entry: namelen u8, offset be16, name, [ftype u8], inode
constexpr size_t kSfEntNamelen = 0;
constexpr size_t kSfEntOffset = 1;
constexpr size_t kSfEntName = 3;

// Data blocks
constexpr size_t kData3Crc = 4;            // le32
constexpr size_t kData3HeaderSize = 64;    // dir3 data header with bestfree
constexpr size_t kBlock3HeaderSize = 64;   // dir3 single block header
constexpr size_t kData2HeaderSize = 16;
constexpr size_t kDataFreeTag = 0xffff;
constexpr size_t kDataAlign = 8;
constexpr uint16_t kNullDataOff = 0xffff;

// Leaf blocks
constexpr size_t kLeaf3HeaderSize = 64;    // da3_blkinfo(56) + count + stale + pad
constexpr size_t kLeaf2HeaderSize = 16;    // da blkinfo(12) + count + stale
constexpr size_t kLeaf3Crc = 12;           // info.crc
constexpr size_t kLeafEntSize = 8;         // hashval be32 + address be32
constexpr size_t kLeafEntHashval = 0;      // be32
constexpr size_t kLeafEntAddress = 4;      // be32
constexpr size_t kLeafHdrCount = 56;       // be16 count
constexpr size_t kLeafHdrStale = 58;       // be16 stale
constexpr size_t kLeaf2HdrCount = 12;
constexpr size_t kLeaf2HdrStale = 14;
constexpr size_t kLeafTailBestcount = 0;   // be32 at block end - 4

// Free blocks (multi-block "node" format)
constexpr size_t kFree3HeaderSize = 64;    // blk hdr(48)+firstdb+nvalid+nused+pad
constexpr size_t kFree3Crc = 4;
constexpr size_t kFree2HeaderSize = 16;    // magic + firstdb + nvalid + nused
constexpr size_t kFreeHdrFirstdb = 48;     // be32 (v3 layout)
constexpr size_t kFreeHdrNvalid = 52;      // be32
constexpr size_t kFreeHdrNused = 56;       // be32
constexpr size_t kFree2Firstdb = 4;
constexpr size_t kFree2Nvalid = 8;
constexpr size_t kFree2Nused = 12;
constexpr int16_t kFreeBestsUnused = -1;

// Directory hash space layout (in the directory inode's block space):
//   data blocks      : [0, 1<<35)  (db numbers also address data thus)
//   leaf blocks      : start at XFS_DIR2_LEAF_OFFSET
//   free blocks      : start at XFS_DIR2_FREE_OFFSET
constexpr uint64_t kSpaceSize = 1ULL << (32 + 3);
constexpr uint64_t kLeafOffset = kSpaceSize;
constexpr uint64_t kFreeOffset = 2 * kSpaceSize;

// dir3 ftype values
constexpr uint8_t kFtUnknown = 0;
constexpr uint8_t kFtRegFile = 1;
constexpr uint8_t kFtDir = 2;
constexpr uint8_t kFtChrdev = 3;
constexpr uint8_t kFtBlkdev = 4;
constexpr uint8_t kFtFifo = 5;
constexpr uint8_t kFtSock = 6;
constexpr uint8_t kFtSymlink = 7;
constexpr uint8_t kFtWht = 8;
constexpr uint8_t kFtMax = 9;

// Data entry / unused sizes (v5).
// entsize = round_up(9 + namelen + (ftype?1:0) + 2, 8)
inline size_t DataEntrySize(size_t namelen, bool ftype) {
  size_t len = 8 + 1 + namelen + 2;  // inumber + namelen + name + tag
  if (ftype) len += 1;
  return (len + (kDataAlign - 1)) & ~(size_t)(kDataAlign - 1);
}
}  // namespace dir

// ---------------------------------------------------------------------------
// Remote symlink header (XFS_DINODE_FMT_*, symlink data in file blocks)
// ---------------------------------------------------------------------------
namespace symlink {
constexpr size_t kMagic = 0;     // be32
constexpr size_t kOffset = 4;    // be32
constexpr size_t kBytes = 8;     // be32
constexpr size_t kCrc = 12;      // be32 (le32)
constexpr size_t kUuid = 16;     // 16 bytes
constexpr size_t kOwner = 32;    // be64
constexpr size_t kBlkno = 40;    // be64
constexpr size_t kLsn = 48;      // be64
constexpr size_t kHeaderSize = 56;
constexpr uint32_t kMaxLen = 1024;
}  // namespace symlink

// ---------------------------------------------------------------------------
// General geometry limits (from libxfs)
// ---------------------------------------------------------------------------
constexpr uint32_t kMinBlockSize = 512;
constexpr uint32_t kMaxBlockSize = 64 * 1024;
constexpr uint32_t kMinInodeSize = 256;
constexpr uint32_t kMaxInodeSize = 2048;
constexpr uint32_t kInodesPerChunk = 64;
constexpr uint32_t kInodesPerChunkLog = 6;
constexpr uint32_t kInodesPerHolemaskBit = 4;
constexpr uint64_t kMaxInumber = (1ULL << 56) - 1;
constexpr uint32_t kAgiUnlinkedBuckets = 64;
constexpr uint16_t kDaNodeMaxDepth = 5;
constexpr uint32_t kSymlinkMaxLen = 1024;

}  // namespace xfs::ondisk
