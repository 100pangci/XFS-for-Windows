#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent structural probe for fixture images (xfs_tests --make-fixture).

Like tests/tools/xfs_probe.py this is deliberately a *second*, standalone
implementation (no project code): it re-derives the superblock, AG headers,
free space btrees, inode chunk, shortform directory, file extent and the file
data pattern from raw bytes. If this probe and the C++ reader agree, the
fixture's structures are not just "whatever our own parser accepts".

Also verifies the deterministic data pattern so the same check can run on
Linux against a loop-mounted file:

    byte(i) = ((i * 167) ^ (i >> 7) ^ 0x5A) & 0xFF

usage: fixture_probe.py <image>

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

BNO_MAGIC = 0x41423342  # 'AB3B'
CNT_MAGIC = 0x41423343  # 'AB3C'
INO_MAGIC = 0x49414233  # 'IAB3'

FILE_NAME = "hello.bin"
FILE_INO = 67
FILE_BLOCKS = 8
DIR3_DATA_FIRST_OFFSET = 96  # data header (64) + entsize(".") + entsize("..")
NULL_AGBLOCK = 0xFFFFFFFF
NULL_AGINO = 0xFFFFFFFF


# ---------------------------------------------------------------------------
# CRC32c (Castagnoli), independent table implementation.
# ---------------------------------------------------------------------------
def _make_crc_table():
    poly = 0x82F63B78
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            crc = (crc >> 1) ^ poly if (crc & 1) else crc >> 1
        table.append(crc)
    return table


CRC_TABLE = _make_crc_table()


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc = CRC_TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def xfs_crc(buf: bytes, crc_offset: int) -> int:
    data = bytearray(buf)
    data[crc_offset:crc_offset + 4] = b"\x00\x00\x00\x00"
    return crc32c(bytes(data))


def check_crc(buf: bytes, crc_offset: int, what: str):
    actual = struct.unpack_from("<I", buf, crc_offset)[0]
    computed = xfs_crc(buf, crc_offset)
    assert actual == computed, (
        f"{what}: crc mismatch at {crc_offset}: {actual:#x} != {computed:#x}")


def le32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from(">Q", buf, off)[0]


def pattern_byte(i: int) -> int:
    return ((i * 167) ^ (i >> 7) ^ 0x5A) & 0xFF


class Image:
    def __init__(self, path: str):
        with open(path, "rb") as f:
            self.data = f.read()

    def read(self, offset: int, size: int) -> bytes:
        assert offset >= 0 and offset + size <= len(self.data), (
            f"read out of bounds: {offset}+{size} of {len(self.data)}")
        return self.data[offset:offset + size]


def decode_bmbt_rec(l0: int, l1: int):
    unwritten = (l0 >> 63) != 0
    startoff = (l0 >> 9) & ((1 << 54) - 1)
    startblock = ((l0 & 0x1FF) << 43) | (l1 >> 21)
    blockcount = l1 & 0x1FFFFF
    return startoff, startblock, blockcount, unwritten


def check_btree_root(img: Image, block_off: int, blocksize: int, magic: int,
                     what: str, agno: int, expect_records=None):
    block = img.read(block_off, blocksize)
    assert u32(block, 0) == magic, f"{what}: bad magic {u32(block, 0):#x}"
    assert u16(block, 4) == 0, f"{what}: expected a leaf root"
    check_crc(block, 52, what)
    numrecs = u16(block, 6)
    assert numrecs <= (blocksize - 56) // 8, f"{what}: numrecs {numrecs}"
    blkno = u64(block, 16)
    assert blkno == block_off // 512, (what, blkno, block_off // 512)
    assert u32(block, 48) == agno, f"{what}: owner"
    records = [(u32(block, 56 + i * 8), u32(block, 60 + i * 8))
               for i in range(numrecs)]
    if expect_records is not None:
        assert records == expect_records, (what, records, expect_records)
    return records


def main(path: str) -> int:
    img = Image(path)

    sb = img.read(0, 512)
    assert u32(sb, 0) == SB_MAGIC, "bad superblock magic"
    versionnum = u16(sb, 100)
    assert versionnum & 0xF == 5, f"not v5: {versionnum:#x}"
    blocksize = u32(sb, 4)
    dblocks = u64(sb, 8)
    uuid = sb[32:48]
    logstart = u64(sb, 48)
    rootino = u64(sb, 56)
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
    incompat = u32(sb, 216)
    ro_compat = u32(sb, 212)

    assert blocksize == 4096 and inodesize == 512 and sectsize == 512, (
        blocksize, inodesize, sectsize)
    assert incompat == (1 << 0), f"expected only ftype incompat: {incompat:#x}"
    assert ro_compat == 0, f"expected no ro-compat features: {ro_compat:#x}"
    assert rootino == 64, rootino
    assert u64(sb, 64) == rootino + 1 and u64(sb, 72) == rootino + 2
    check_crc(img.read(0, sectsize), 224, "superblock")
    assert inopblog == blocklog - 9, (inopblog, blocklog)
    assert agblklog == (agblocks - 1).bit_length()
    assert (dblocks + agblocks - 1) // agblocks == agcount

    print(f"superblock OK: uuid={uuidlib.UUID(bytes=uuid)} agcount={agcount} "
          f"agblocks={agblocks} dblocks={dblocks} icount={icount} "
          f"ifree={ifree} fdblocks={fdblocks}")

    # ---- per-AG headers and trees ----
    total_free = 0
    total_icount = 0
    total_ifree = 0
    free_extents_ag0 = None
    for agno in range(agcount):
        ag_base = agno * agblocks * blocksize
        first = agno * agblocks
        agsize = min(agblocks, dblocks - first)

        sb2 = img.read(ag_base, sectsize)
        assert u32(sb2, 0) == SB_MAGIC, f"AG{agno} secondary sb magic"
        check_crc(sb2, 224, f"AG{agno} secondary sb")
        assert u64(sb2, 56) == rootino, f"AG{agno} secondary rootino"

        agf = img.read(ag_base + sectsize, sectsize)
        assert u32(agf, 0) == AGF_MAGIC, f"AG{agno} AGF magic"
        check_crc(agf, 216, f"AG{agno} AGF")
        assert u32(agf, 8) == agno and u32(agf, 12) == agsize
        bno_root = u32(agf, 16)
        cnt_root = u32(agf, 20)
        flfirst, fllast = u32(agf, 40), u32(agf, 44)
        flcount = u32(agf, 48)
        freeblks, longest = u32(agf, 52), u32(agf, 56)
        assert u32(agf, 60) == 0, "btreeblks must stay 0 (only roots)"
        assert flcount == 4, flcount
        assert flfirst < 16 and fllast < 16

        agi = img.read(ag_base + 2 * sectsize, sectsize)
        assert u32(agi, 0) == AGI_MAGIC, f"AG{agno} AGI magic"
        check_crc(agi, 312, f"AG{agno} AGI")
        assert u32(agi, 8) == agno and u32(agi, 12) == agsize
        agi_count, agi_free = u32(agi, 16), u32(agi, 28)
        ino_root = u32(agi, 20)
        assert u32(agi, 24) == 1, "inobt level"

        agfl = img.read(ag_base + 3 * sectsize, sectsize)
        assert u32(agfl, 0) == AGFL_MAGIC, f"AG{agno} AGFL magic"
        check_crc(agfl, 32, f"AG{agno} AGFL")
        assert u32(agfl, 4) == agno
        entries = (sectsize - 36) // 4
        agfl_entries = [u32(agfl, 36 + 4 * i) for i in range(entries)]
        real = [e for e in agfl_entries if e != NULL_AGBLOCK]
        assert len(real) == flcount, (agno, len(real), flcount)
        assert real == list(range(4, 4 + flcount)) or agno > 0, real

        # bnobt: start-ordered free space. AG0 lost 8 blocks to the fixture
        # file, so the free space starts one block after the inode chunk.
        bno_records = check_btree_root(
            img, ag_base + bno_root * blocksize, blocksize, BNO_MAGIC,
            f"AG{agno} bnobt", agno)
        last_end = -1
        for start, length in bno_records:
            assert start > last_end, (agno, "bnobt not sorted", bno_records)
            assert start + length <= agsize, (agno, start, length)
            last_end = start + length
        assert sum(l for _, l in bno_records) == freeblks, (
            agno, bno_records, freeblks)
        assert max(l for _, l in bno_records) == longest, (agno, longest)

        # cntbt: same multiset, ordered by (count, start).
        cnt_records = check_btree_root(
            img, ag_base + cnt_root * blocksize, blocksize, CNT_MAGIC,
            f"AG{agno} cntbt", agno)
        assert cnt_records == sorted(cnt_records, key=lambda r: (r[1], r[0])), (
            agno, "cntbt order", cnt_records)
        assert sorted(cnt_records) == sorted(bno_records), (agno, cnt_records)

        # inobt.
        inobt = img.read(ag_base + ino_root * blocksize, blocksize)
        assert u32(inobt, 0) == INO_MAGIC, f"AG{agno} inobt magic"
        check_crc(inobt, 52, f"AG{agno} inobt")
        assert u32(inobt, 48) == agno
        numrecs = u16(inobt, 6)
        if agi_count:
            assert numrecs == 1, numrecs
            startino = u32(inobt, 56)
            freecount = u32(inobt, 60)
            freemask = u64(inobt, 64)
            assert startino == 64, startino
            assert freecount == bin(freemask).count("1"), (agno, freecount)
            assert freecount == agi_free and agi_count == 64
            assert freemask & (1 << (FILE_INO - 64)) == 0, (
                "fixture inode must be allocated")
            assert freecount == 60, freecount
            print(f"AG{agno}: free={freeblks} AGFL={flcount} "
                  f"chunk@{startino} freecount={freecount}")
        else:
            assert numrecs == 0, numrecs
            assert agi_free == 0

        total_free += freeblks + flcount
        total_icount += agi_count
        total_ifree += agi_free
        if agno == 0:
            free_extents_ag0 = bno_records

    assert total_free == fdblocks, (total_free, fdblocks)
    assert total_icount == icount and total_ifree == ifree, (
        total_icount, icount, total_ifree, ifree)

    # ---- inodes ----
    def inode_offset(ino: int) -> int:
        agno = ino >> (agblklog + inopblog)
        agino = ino & ((1 << (agblklog + inopblog)) - 1)
        agbno = agino >> inopblog
        offset = agino & ((1 << inopblog) - 1)
        return ((agno * agblocks + agbno) * blocksize) + offset * inodesize

    root = img.read(inode_offset(rootino), inodesize)
    assert u16(root, 0) == DINODE_MAGIC and root[4] == 3, "root inode"
    check_crc(root, 100, "root inode")
    assert u64(root, 152) == rootino
    assert root[160:176] == uuid, "root inode uuid"
    assert u16(root, 2) == 0o040755, oct(u16(root, 2))
    assert u32(root, 16) == 2, "root nlink"
    assert root[5] == 1, "root inode must be LOCAL (shortform)"
    assert root[83] == 2, "aformat must be EXTENTS"

    # Shortform validation, mirroring the on-disk rules (offsets must be
    # non-decreasing and start at data_first_offset; the size must end exactly
    # after the last entry).
    sf_size = u64(root, 56)
    fork = root[176:176 + sf_size]
    count, i8count = fork[0], fork[1]
    assert count == 1 and i8count == 0, (count, i8count)
    parent = u32(fork, 2)
    assert parent == rootino, parent
    off = 6
    namelen = fork[off]
    entry_offset = u16(fork, off + 1)
    name = fork[off + 3:off + 3 + namelen]
    ftype = fork[off + 3 + namelen]
    ino = u32(fork, off + 3 + namelen + 1)
    entsize = 3 + namelen + 1 + 4
    assert name == FILE_NAME.encode(), name
    assert ftype == 1, ftype
    assert ino == FILE_INO, ino
    assert entry_offset >= DIR3_DATA_FIRST_OFFSET, entry_offset
    assert off + entsize == sf_size, (off + entsize, sf_size)
    print(f"root inode OK: shortform with 1 entry '{FILE_NAME}' -> inode {ino} "
          f"(offset {entry_offset})")

    f = img.read(inode_offset(FILE_INO), inodesize)
    assert u16(f, 0) == DINODE_MAGIC and f[4] == 3, "file inode"
    check_crc(f, 100, "file inode")
    assert u64(f, 152) == FILE_INO and f[160:176] == uuid
    assert u16(f, 2) == 0o100644, oct(u16(f, 2))
    assert u32(f, 16) == 1, "file nlink"
    assert f[5] == 2, "file inode must be EXTENTS"
    assert f[83] == 2, "file aformat"
    size = u64(f, 56)
    nblocks = u64(f, 64)
    nextents = u32(f, 76)
    assert size == FILE_BLOCKS * blocksize, size
    # di_nblocks is in filesystem blocks, not 512-byte units.
    assert nblocks == FILE_BLOCKS, (nblocks, FILE_BLOCKS)
    assert nextents == 1, nextents
    l0, l1 = u64(f, 176), u64(f, 184)
    startoff, startblock, blockcount, unwritten = decode_bmbt_rec(l0, l1)
    assert startoff == 0 and blockcount == FILE_BLOCKS, (startoff, blockcount)
    assert unwritten == 0, "fixture extent must be written"
    data_fsb = startblock
    data_agno = data_fsb >> agblklog
    data_agbno = data_fsb & ((1 << agblklog) - 1)
    assert data_agno == 0, data_agno
    print(f"file inode OK: size={size} nblocks={nblocks} extent="
          f"fsb {data_fsb} ({blockcount} blocks)")

    # The file's blocks must not be in the free space of their AG.
    for start, length in free_extents_ag0:
        assert not (start <= data_agbno < start + length), (
            "file extent overlaps free space", data_agbno, (start, length))
    for block in range(data_agbno, data_agbno + blockcount):
        assert block >= 16, (block, "file data overlaps AG0 metadata")

    # ---- file data pattern ----
    data_off = data_agno * agblocks * blocksize + data_agbno * blocksize
    data = img.read(data_off, size)
    expected = bytes(pattern_byte(i) for i in range(size))
    assert data == expected, "fixture file data does not match the pattern"
    print(f"file data OK: {size} bytes match the documented pattern")

    # ---- log ----
    log_agno = logstart >> agblklog
    log_agbno = logstart & ((1 << agblklog) - 1)
    log_offset = (log_agno * agblocks + log_agbno) * blocksize
    assert log_offset + (logblocks << blocklog) <= len(img.data), "log range"
    hdr = img.read(log_offset, 512)
    assert u32(hdr, 0) == XLOG_MAGIC, "log record magic"
    assert u32(hdr, 4) == 1 and u32(hdr, 8) == 2, "log cycle/version"
    assert u64(hdr, 16) & 0xFFFFFFFF == 0, "log record lsn"
    assert hdr[304:320] == uuid, "log record uuid"
    data = bytearray(img.read(log_offset + 512, 512))
    data[0:4] = hdr[44:48]  # restore the word replaced by the cycle value
    assert data[8] == 0xAA and (data[9] & 0x20), "clean unmount record"
    # xfs_unmount_log_format.magic is written in host byte order by the kernel
    # (see MakeUnmountOp); recovery never reads it, only the flag.
    assert struct.unpack_from("<H", data, 12)[0] == 0x556E, "unmount magic"
    print(f"log OK: clean unmount record at byte {log_offset}")

    print("ALL FIXTURE CHECKS PASSED")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    try:
        sys.exit(main(sys.argv[1]))
    except AssertionError as exc:
        print(f"FIXTURE PROBE FAILED: {exc}", file=sys.stderr)
        sys.exit(1)
