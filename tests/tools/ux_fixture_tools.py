#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Fixture helpers for the UX-1 Windows discovery/automount images.

These helpers deliberately know only the on-disk XFS superblock layout and the
fixed-VHD footer.  They do not link against the project, so the Linux image
generator can create fixtures that the Windows binary has to judge on its own.

Commands:
  summary <image>
      Print the superblock facts the Windows tests assert (JSON).
  corrupt-superblock <in> <out>
      Keep the XFS magic but break the superblock CRC.
  unsupported-feature <in> <out>
      Set the METADIR incompatible feature and repair the CRC, producing a
      valid superblock that this build must refuse as unsupported.
  vhd-footer <raw-in> <vhd-out>
      Write a fixed VHD: the raw bytes followed by a 512-byte footer.

usage: ux_fixture_tools.py <command> ...
"""

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from device_image_probe import crc32c  # noqa: E402

SB_MAGIC = 0x58465342
SB_CRC_OFF = 224
SB_SECTSIZE_OFF = 102
SB_UUID_OFF = 32
SB_LABEL_OFF = 108
SB_LABEL_LEN = 12
SB_DBLOCKS_OFF = 8
SB_FDBLOCKS_OFF = 144
SB_BLOCKSIZE_OFF = 4
SB_FEATURES_INCOMPAT_OFF = 216
INCOMPAT_METADIR = 1 << 8


def be16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def be32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def be64(buf, off):
    return struct.unpack_from(">Q", buf, off)[0]


def read_superblock(path):
    with open(path, "rb") as f:
        first = f.read(512)
        if len(first) < 512:
            raise SystemExit(f"{path}: file is smaller than one sector")
        if be32(first, 0) != SB_MAGIC:
            raise SystemExit(f"{path}: no XFS superblock magic")
        sectsize = be16(first, SB_SECTSIZE_OFF)
        if sectsize < 512 or sectsize > 32768 or (sectsize & (sectsize - 1)):
            raise SystemExit(f"{path}: invalid sector size {sectsize}")
        f.seek(0)
        return bytearray(f.read(sectsize))


def summarize(path):
    sb = read_superblock(path)
    label = bytes(sb[SB_LABEL_OFF:SB_LABEL_OFF + SB_LABEL_LEN])
    label = label.split(b"\x00", 1)[0].decode("ascii", "replace").rstrip()
    blocksize = be32(sb, SB_BLOCKSIZE_OFF)
    dblocks = be64(sb, SB_DBLOCKS_OFF)
    fdblocks = be64(sb, SB_FDBLOCKS_OFF)
    uuid = bytes(sb[SB_UUID_OFF:SB_UUID_OFF + 16])
    text = (uuid[0:4].hex() + "-" + uuid[4:6].hex() + "-" + uuid[6:8].hex() +
            "-" + uuid[8:10].hex() + "-" + uuid[10:16].hex())
    return {
        "uuid": text,
        "label": label,
        "block_size": blocksize,
        "dblocks": dblocks,
        "fdblocks": fdblocks,
        "filesystem_bytes": dblocks * blocksize,
        "free_bytes": fdblocks * blocksize,
        "sector_size": be16(sb, SB_SECTSIZE_OFF),
        "logstart": be64(sb, 48),
        "logblocks": be32(sb, 96),
    }


def copy_with_edit(src, dst, edit):
    data = bytearray(Path(src).read_bytes())
    if len(data) < 512:
        raise SystemExit(f"{src}: file is smaller than one sector")
    edit(data)
    Path(dst).write_bytes(data)


def corrupt(data):
    # A real value change inside the superblock with the old CRC left in place:
    # the magic is still XFS, but verification must reject the sector.
    data[240] ^= 0x5A


def unsupported(data):
    sectsize = be16(data, SB_SECTSIZE_OFF)
    features = be32(data, SB_FEATURES_INCOMPAT_OFF) | INCOMPAT_METADIR
    struct.pack_into(">I", data, SB_FEATURES_INCOMPAT_OFF, features)
    sector = bytearray(data[:sectsize])
    sector[SB_CRC_OFF:SB_CRC_OFF + 4] = b"\x00\x00\x00\x00"
    struct.pack_into("<I", data, SB_CRC_OFF, crc32c(bytes(sector)))


def vhd_geometry(size_bytes):
    sectors = size_bytes // 512
    heads, sectors_per_track = 16, 63
    cylinders = sectors // (heads * sectors_per_track)
    if cylinders > 0xFFFF:
        # The VHD specification saturates the geometry for very large disks.
        cylinders = 0xFFFF
        heads, sectors_per_track = 255, 17
    return cylinders, heads, sectors_per_track


def write_vhd(raw_path, vhd_path):
    raw = Path(raw_path)
    payload = raw.read_bytes()
    size = len(payload)
    if size % 512 != 0:
        raise SystemExit(f"{raw_path}: size {size} is not a multiple of 512")
    cylinders, heads, sectors = vhd_geometry(size)
    footer = bytearray(512)
    footer[0:8] = b"conectix"
    struct.pack_into(">I", footer, 8, 2)          # features (reserved)
    struct.pack_into(">I", footer, 12, 0x00010000)  # file format version
    struct.pack_into(">Q", footer, 16, 0xFFFFFFFFFFFFFFFF)  # fixed disk
    struct.pack_into(">I", footer, 24, 0)         # timestamp (deterministic)
    footer[28:32] = b"xfs1"
    struct.pack_into(">I", footer, 32, 0x00010000)
    footer[36:40] = b"Wi2k"
    struct.pack_into(">Q", footer, 40, size)
    struct.pack_into(">Q", footer, 48, size)
    struct.pack_into(">H", footer, 56, cylinders)
    footer[58] = heads
    footer[59] = sectors
    struct.pack_into(">I", footer, 60, 2)         # fixed
    # Deterministic unique id derived from the content; not a filesystem UUID.
    import hashlib
    footer[68:84] = hashlib.sha256(payload).digest()[:16]
    footer[84] = 0                                 # saved state (not saved)
    checksum = (~sum(footer)) & 0xFFFFFFFF
    struct.pack_into(">I", footer, 64, checksum)
    with open(vhd_path, "wb") as out:
        out.write(payload)
        out.write(footer)


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    command = argv[1]
    if command == "summary" and len(argv) == 3:
        print(json.dumps(summarize(argv[2]), indent=2, sort_keys=True))
        return 0
    if command == "corrupt-superblock" and len(argv) == 4:
        copy_with_edit(argv[2], argv[3], corrupt)
        return 0
    if command == "unsupported-feature" and len(argv) == 4:
        copy_with_edit(argv[2], argv[3], unsupported)
        return 0
    if command == "vhd-footer" and len(argv) == 4:
        write_vhd(argv[2], argv[3])
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))