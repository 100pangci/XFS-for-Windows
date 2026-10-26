#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent structural probe for XFS v5 images.

This is deliberately a *second* implementation (Python, no project code) used
to cross-check images produced by `xfswin format`. It verifies superblock,
AG headers, free space btrees, inode chunks, the root directory and the clean
log record.

It is a development/CI tool, not a mount implementation.

usage: xfs_probe.py <image>

exit code 0 = all checks passed.
"""

import struct
import sys
import uuid as uuidlib

SB_MAGIC = 0x58465342
AGF_MAGIC = 0x58414746
AGI_MAGIC = 0x58414749
AGFL_MAGIC = 0x5841464C
DINODE_MAGIC = 0x494E
XLOG_MAGIC = 0xFEEDBABE


# ---------------------------------------------------------------------------
# CRC32c (Castagnoli), independent table implementation.
# ---------------------------------------------------------------------------
def make_crc_table():
    poly = 0x82F63B78
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            crc = (crc >> 1) ^ poly if (crc & 1) else crc >> 1
        table.append(crc)
    return table


CRC_TABLE = make_crc_table()


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc = CRC_TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def xfs_crc(buf: bytes, crc_offset: int) -> int:
    """CRC over buf with the little-endian crc field treated as zero."""
    data = bytearray(buf)
    data[crc_offset:crc_offset + 4] = b"\x00\x00\x00\x00"
    return crc32c(bytes(data))


def le32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def u16(buf: bytes, off: int) -> int:
    return struct.unpack_from(">H", buf, off)[0]


def u32(buf: bytes, off: int) -> int:
    return struct.unpack_from(">I", buf, off)[0]


def u64(buf: bytes, off: int) -> int:
    return struct.unpack_from(">Q", buf, off)[0]


class Image:
    def __init__(self, path: str):
        with open(path, "rb") as f:
            self.data = f.read()

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or offset + size > len(self.data):
            raise AssertionError(f"read out of bounds: {offset}+{size}")
        return self.data[offset:offset + size]


def check_crc(buf: bytes, expected_off: int, what: str):
    actual = le32(buf, expected_off)
    computed = xfs_crc(buf, expected_off)
    assert actual == computed, (
        f"{what}: crc mismatch at {expected_off}: {actual:#x} != {computed:#x}"
    )


def main(path: str) -> int:
    img = Image(path)

    sb = img.read(0, 512)
    assert u32(sb, 0) == SB_MAGIC, "bad superblock magic"
    versionnum = u16(sb, 100)
    assert versionnum & 0xF == 5, f"not v5: versionnum={versionnum:#x}"
    assert versionnum & 0x10, (
        "ATTRBIT is clear; Linux xfs_scrub rejects any attr fork")
    blocksize = u32(sb, 4)
    dblocks = u64(sb, 8)
    uuid = sb[32:48]
    logstart = u64(sb, 48)
    rootino = u64(sb, 56)
    rbmino = u64(sb, 64)
    rsumino = u64(sb, 72)
    assert rbmino == rootino + 1, ("rt bitmap inode", rbmino)
    assert rsumino == rootino + 2, ("rt summary inode", rsumino)
    agblocks = u32(sb, 84)
    agcount = u32(sb, 88)
    logblocks = u32(sb, 96)
    sectsize = u16(sb, 102)
    inodesize = u16(sb, 104)
    blocklog = sb[120]
    inopblog = sb[123]
    agblklog = sb[124]
    icount = u64(sb, 128)
    ifree = u64(sb, 136)
    fdblocks = u64(sb, 144)
    features2 = u32(sb, 200)
    incompat = u32(sb, 216)

    assert blocksize == (1 << blocklog)
    assert sectsize in (512, 1024, 2048, 4096), sectsize
    assert inodesize in (256, 512, 1024, 2048), inodesize
    assert inopblog == blocklog - inodesize.bit_length() + 1
    assert agblklog == (agblocks - 1).bit_length()
    assert (dblocks + agblocks - 1) // agblocks == agcount

    sbsect = img.read(0, sectsize)
    check_crc(sbsect, 224, "superblock")
    assert incompat & (1 << 0), "ftype should be enabled"
    assert features2 & 0x100, "crc bit should be set"
    assert (features2 & 0x2), "lazy sb count bit should be set"
    rextsize = u32(sb, 80)
    assert 4096 <= rextsize * blocksize <= 1024 * 1024 * 1024, (
        "invalid realtime extent size", rextsize)

    meta_uuid = sbsect[248:264]
    if not (incompat & (1 << 2)):  # META_UUID not set
        # The kernel derives the metadata uuid from sb_uuid and never writes
        # sb_meta_uuid to disk in this case.
        assert meta_uuid == b"\x00" * 16, meta_uuid

    print(f"superblock OK: block={blocksize} sector={sectsize} "
          f"inode={inodesize} agcount={agcount} agblocks={agblocks}")
    print(f"uuid={uuidlib.UUID(bytes=uuid)} logstart={logstart} "
          f"logblocks={logblocks} rootino={rootino}")

    # ---- per-AG headers, btrees, free space ----
    total_free = 0
    total_icount = 0
    total_ifree = 0
    agf_list = []
    for agno in range(agcount):
        # contiguity: AG byte start = agno * agblocks (not fsb encoding)
        ag_base = agno * agblocks * blocksize
        first = agno * agblocks
        agsize = min(agblocks, dblocks - first)

        sb2 = img.read(ag_base, sectsize)
        assert u32(sb2, 0) == SB_MAGIC, f"AG{agno} secondary sb magic"
        check_crc(sb2, 224, f"AG{agno} sb")
        assert u64(sb2, 56) == rootino, f"AG{agno} secondary rootino"

        agf = img.read(ag_base + sectsize, sectsize)
        assert u32(agf, 0) == AGF_MAGIC, f"AG{agno} AGF magic"
        check_crc(agf, 216, f"AG{agno} AGF")
        assert u32(agf, 8) == agno
        assert u32(agf, 12) == agsize, (agno, u32(agf, 12), agsize)
        bno_root = u32(agf, 16)
        cnt_root = u32(agf, 20)
        flfirst = u32(agf, 40)
        fllast = u32(agf, 44)
        flcount = u32(agf, 48)
        freeblks = u32(agf, 52)
        longest = u32(agf, 56)
        btreeblks = u32(agf, 60)
        assert flfirst < (sectsize - 36) // 4
        assert fllast < (sectsize - 36) // 4
        assert btreeblks == 0, btreeblks

        agi = img.read(ag_base + 2 * sectsize, sectsize)
        assert u32(agi, 0) == AGI_MAGIC, f"AG{agno} AGI magic"
        check_crc(agi, 312, f"AG{agno} AGI")
        assert u32(agi, 8) == agno
        assert u32(agi, 12) == agsize
        agi_count = u32(agi, 16)
        agi_free = u32(agi, 28)

        agfl = img.read(ag_base + 3 * sectsize, sectsize)
        assert u32(agfl, 0) == AGFL_MAGIC, f"AG{agno} AGFL magic"
        check_crc(agfl, 32, f"AG{agno} AGFL")
        assert u32(agfl, 4) == agno
        entries = (sectsize - 36) // 4
        agfl_entries = [u32(agfl, 36 + 4 * i) for i in range(entries)]
        null = 0xFFFFFFFF
        real = [e for e in agfl_entries if e != null]
        assert len(real) == flcount, (agno, len(real), flcount)
        for b in real:
            assert b < agsize, (agno, "AGFL entry out of range", b)

        # bno root
        bnob = img.read(ag_base + bno_root * blocksize, blocksize)
        assert u32(bnob, 0) == 0x41423342, f"AG{agno} bno magic"
        check_crc(bnob, 52, f"AG{agno} bnobt")
        assert u16(bnob, 4) == 0, "bnobt should be a leaf root"
        numrecs = u16(bnob, 6)
        assert 1 <= numrecs <= 2, (agno, numrecs)
        recs = []
        last_end = -1
        for i in range(numrecs):
            start = u32(bnob, 56 + i * 8)
            length = u32(bnob, 60 + i * 8)
            assert start > last_end, (agno, "bnobt not sorted", start)
            assert start + length <= agsize, (agno, start, length)
            last_end = start + length
            recs.append((start, length))
        assert sum(l for _, l in recs) == freeblks, (agno, recs, freeblks)
        assert max(l for _, l in recs) == longest, (agno, recs, longest)
        assert u64(bnob, 16) == (ag_base + bno_root * blocksize) // 512
        assert u32(bnob, 48) == agno

        cntb = img.read(ag_base + cnt_root * blocksize, blocksize)
        assert u32(cntb, 0) == 0x41423343, f"AG{agno} cnt magic"
        check_crc(cntb, 52, f"AG{agno} cntbt")
        assert u16(cntb, 6) == numrecs
        cnt_recs = [(u32(cntb, 56 + i * 8), u32(cntb, 60 + i * 8))
                    for i in range(numrecs)]
        assert sum(l for _, l in cnt_recs) == sum(l for _, l in recs)
        assert cnt_recs == sorted(cnt_recs, key=lambda r: (r[1], r[0])), (
            agno, "cntbt order", cnt_recs)
        assert sorted(cnt_recs) == sorted(recs)

        ibt_root = u32(agi, 20)
        inobt = img.read(ag_base + ibt_root * blocksize, blocksize)
        assert u32(inobt, 0) == 0x49414233, f"AG{agno} inobt magic"
        check_crc(inobt, 52, f"AG{agno} inobt")
        ino_numrecs = u16(inobt, 6)
        if agi_count:
            assert ino_numrecs == 1
            startino = u32(inobt, 56)
            freecount = u32(inobt, 60)
            freemask = u64(inobt, 64)
            assert freecount == bin(freemask).count("1"), (agno, freecount)
            assert freecount == agi_free and 64 == agi_count
            print(f"AG{agno}: free={freeblks} AGFL={flcount} "
                  f"chunk@{startino} freecount={freecount}")
        else:
            assert ino_numrecs == 0
            print(f"AG{agno}: free={freeblks} AGFL={flcount} (no inodes)")

        total_free += freeblks + flcount
        total_icount += agi_count
        total_ifree += agi_free
        agf_list.append(agf)

    assert total_free == fdblocks, (total_free, fdblocks)
    assert total_icount == icount and total_ifree == ifree, (
        total_icount, icount, total_ifree, ifree)

    # ---- root inode ----
    agno = rootino >> (agblklog + inopblog)
    agino = rootino & ((1 << (agblklog + inopblog)) - 1)
    agbno = agino >> inopblog
    offset = agino & ((1 << inopblog) - 1)
    inode_offset = ((agno * agblocks + agbno) * blocksize
                    + offset * inodesize)
    inode = img.read(inode_offset, inodesize)
    assert u16(inode, 0) == DINODE_MAGIC, "root inode magic"
    assert inode[4] == 3, "root inode version"
    check_crc(inode, 100, "root inode")
    assert u64(inode, 152) == rootino, "root inode number"
    # di_uuid carries the metadata uuid, which equals sb_uuid when the
    # META_UUID incompat feature is not set.
    assert inode[160:176] == uuid, "root inode uuid"
    mode = u16(inode, 2)
    size = u64(inode, 56)
    fmt = inode[5]
    nlink = u32(inode, 16)
    assert mode == 0o040755, oct(mode)
    assert fmt == 1, "shortform dir"
    assert nlink == 2
    # Shortform directories do not store "." / ".."; the root directory is
    # just the 6-byte header (count=0, i8count=0, parent=rootino).
    assert size == 6, size
    assert inode[83] == 2, ("aformat should be EXTENTS", inode[83])
    fork = inode[176:176 + size]
    assert fork[0] == 0 and fork[1] == 0, "sf count/i8count"
    assert u32(fork, 2) == rootino, "sf parent"
    assert len(fork) == 6, len(fork)
    print(f"root inode OK at byte {inode_offset}: mode=040755 nlink=2 "
          f"size=6 empty shortform root")

    # ---- log ----
    log_agno = logstart >> agblklog
    log_agbno = logstart & ((1 << agblklog) - 1)
    log_offset = (log_agno * agblocks + log_agbno) * blocksize
    hdr = img.read(log_offset, 512)
    assert u32(hdr, 0) == XLOG_MAGIC, "log record magic"
    cycle = u32(hdr, 4)
    version = u32(hdr, 8)
    hlen = u32(hdr, 12)
    lsn = u64(hdr, 16)
    assert cycle == 1 and version == 2, (cycle, version)
    assert 512 <= hlen <= 256 * 1024
    assert lsn & 0xFFFFFFFF == 0, hex(lsn)
    assert hdr[304:320] == uuid, "log record uuid"
    data = bytearray(img.read(log_offset + 512, 512))
    data[0:4] = hdr[44:48]  # restore first word from h_cycle_data[0]
    assert data[8] == 0xAA, "log client id"
    assert data[9] & 0x20, "log unmount flag"
    assert u32(bytes(data), 4) == 8, "unmount record length"
    assert len(data) >= 20
    # Host byte order, like the kernel writes it; recovery never reads it.
    assert struct.unpack_from("<H", bytes(data), 12)[0] == 0x556E, \
        "unmount magic"
    print(f"log OK at byte {log_offset}: clean unmount record, cycle={cycle}")

    print("ALL STRUCTURAL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    try:
        sys.exit(main(sys.argv[1]))
    except AssertionError as exc:
        print(f"PROBE FAILED: {exc}", file=sys.stderr)
        sys.exit(1)
