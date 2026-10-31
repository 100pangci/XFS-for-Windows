#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent raw-image oracle for a Windows-written XFS v5 device image.

This is deliberately a second, standalone implementation (no project code).
It re-derives the superblock, every AG header and the journal header from raw
bytes and verifies their CRCs.  It is used by the DEV-1 Linux verification
chain after xfs_repair/xfs_db and before the kernel mount, so a bug shared
between the Windows writer and the Linux tools is less likely to go unnoticed.

usage: device_image_probe.py <image> [--expect-marker <text>]

exit code 0 = all checks passed.
"""

import struct
import sys

SB_MAGIC = 0x58465342
AGF_MAGIC = 0x58414746
AGI_MAGIC = 0x58414749
AGFL_MAGIC = 0x5841464C
XLOG_MAGIC = 0xFEEDBABE

SB_CRC_OFF = 224
AGF_CRC_OFF = 216
AGI_CRC_OFF = 312
AGFL_CRC_OFF = 32


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


def be16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def be32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def be64(buf, off):
    return struct.unpack_from(">Q", buf, off)[0]


def read_at(f, offset, size):
    f.seek(offset)
    data = f.read(size)
    assert len(data) == size, f"short read at {offset}: {len(data)} != {size}"
    return data


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    path = argv[1]
    marker = None
    if len(argv) >= 4 and argv[2] == "--expect-marker":
        marker = argv[3]

    with open(path, "rb") as f:
        f.seek(0, 2)
        file_size = f.tell()
        assert file_size > 0, "empty image"
        sb = read_at(f, 0, 512)

        assert be32(sb, 0) == SB_MAGIC, "bad superblock magic"
        version = be16(sb, 100)
        assert (version & 0x000F) == 5, f"not a v5 filesystem: {version:#x}"
        check_crc(sb, SB_CRC_OFF, "superblock")

        block_size = be32(sb, 4)
        dblocks = be64(sb, 8)
        logstart = be64(sb, 48)
        rootino = be64(sb, 56)
        agblocks = be32(sb, 84)
        agcount = be32(sb, 88)
        logblocks = be32(sb, 96)
        sectsize = be16(sb, 102)
        inodesize = be16(sb, 104)
        inopblock = be16(sb, 106)
        blocklog = sb[120]
        uuid = sb[32:48]

        assert block_size >= 1024 and (block_size & (block_size - 1)) == 0, \
            f"bad block size {block_size}"
        assert blocklog == block_size.bit_length() - 1, "blocklog mismatch"
        assert sectsize in (512, 1024, 2048, 4096), f"bad sector size {sectsize}"
        assert inodesize >= 256 and (inodesize & (inodesize - 1)) == 0, \
            f"bad inode size {inodesize}"
        assert inopblock * inodesize == block_size, "inopblock mismatch"
        assert agcount >= 1, "zero AG count"
        assert agblocks * agcount >= dblocks, "AG geometry does not cover dblocks"
        assert rootino != 0, "zero root inode"
        assert logblocks >= 512 and logblocks <= dblocks, "bad log size"
        assert logstart + logblocks <= dblocks, "log outside the data device"
        assert any(uuid), "zero uuid"
        assert file_size >= dblocks * block_size, \
            f"image is shorter than sb_dblocks ({file_size} < {dblocks * block_size})"

        # Every AG header is a v5 sector with its own CRC.
        ag_bytes = agblocks * block_size
        for agno in range(agcount):
            base = agno * ag_bytes
            agf = read_at(f, base, sectsize)
            assert be32(agf, 0) == AGF_MAGIC, f"AG {agno}: bad AGF magic"
            assert be32(agf, 4) == 1, f"AG {agno}: bad AGF version"
            check_crc(agf, AGF_CRC_OFF, f"AG {agno} AGF")

            agi = read_at(f, base + 2 * sectsize, sectsize)
            assert be32(agi, 0) == AGI_MAGIC, f"AG {agno}: bad AGI magic"
            assert be32(agi, 4) == 1, f"AG {agno}: bad AGI version"
            check_crc(agi, AGI_CRC_OFF, f"AG {agno} AGI")

            agfl = read_at(f, base + 3 * sectsize, sectsize)
            assert be32(agfl, 0) == AGFL_MAGIC, f"AG {agno}: bad AGFL magic"
            check_crc(agfl, AGFL_CRC_OFF, f"AG {agno} AGFL")

        # Journal: the first log record header must be recognizable.
        log_byte = logstart * block_size
        header = read_at(f, log_byte, 512)
        assert be32(header, 0) == XLOG_MAGIC, "bad log record magic"
        h_size = be32(header, 12)
        assert h_size >= 512 and (h_size & (h_size - 1)) == 0, \
            f"bad log header size {h_size}"

        if marker is not None:
            f.seek(0)
            found = marker.encode("utf-8") in f.read()
            assert found, "marker text was not found in the raw image"

    print(f"device image probe OK: {path}")
    print(f"  block size      : {block_size}")
    print(f"  sector size     : {sectsize}")
    print(f"  inode size      : {inodesize}")
    print(f"  dblocks         : {dblocks}")
    print(f"  allocation groups: {agcount} x {agblocks} blocks")
    print(f"  internal log    : start {logstart}, {logblocks} blocks")
    print(f"  root inode      : {rootino}")
    print(f"  uuid            : {uuid.hex()}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except AssertionError as exc:
        print(f"PROBE FAILED: {exc}", file=sys.stderr)
        sys.exit(1)
