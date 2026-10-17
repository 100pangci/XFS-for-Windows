#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Crash-window checker for the read-write writer.

A crash injection run (XFSWIN_CRASH_AT=<point>) leaves an image behind. This
tool says whether that image is *consistent*, on the device, before any replay:

  * the log contains only complete, CRC-valid records plus at most one torn
    record, and the torn one is the newest (the crash window itself)
  * the inode is never torn: magic, version and CRC are intact
  * write-ahead logging holds: if the inode changed, the log must contain a
    complete record that describes exactly that change (same LSN and times)
  * the file data is either the old or the new content

usage:
  rw_crash_check.py <image> <pristine-image> --ino <ino>
                    [--expect-times old|new|any]
                    [--data-fsb <fsb> --data-offset <off> --data-file <expected>]
                    [--expect-data old|new|any]

exit code 0 = the crash left a state recovery can finish, and the state is one
of the two allowed ones (never in between).
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import log_probe  # noqa: E402  (path setup must come first)

DINODE_MAGIC = 0x494E
DINODE_CRC_OFF = 100
DINODE_MTIME_OFF = 40
DINODE_LSN_OFF = 112
DINODE_MTIME_END = DINODE_MTIME_OFF + 8


def be16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def be32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def be64(buf, off):
    return struct.unpack_from(">Q", buf, off)[0]


class Geometry:
    """Everything needed to locate an inode and a data block on the device."""

    def __init__(self, image: bytes):
        self.sb = log_probe.Superblock(image)
        self.inode_size = be16(image, 104)  # sb_inodesize
        # sb_inopblog is log2(blocksize / inodesize), which the kernel also
        # validates (sb_blocklog - sb_inodelog == sb_inopblog).
        if self.inode_size <= 0 or self.sb.block_size % self.inode_size:
            raise ValueError("invalid inode geometry")
        self.inopblog = (self.sb.block_size // self.inode_size).bit_length() - 1
        self.agino_log = self.sb.agblklog + self.inopblog

    def ino_byte_offset(self, ino: int) -> int:
        agno = ino >> self.agino_log
        agino = ino & ((1 << self.agino_log) - 1)
        agbno = agino >> self.inopblog
        slot = agino & ((1 << self.inopblog) - 1)
        if agno >= self.sb.agcount or agbno >= self.sb.agblocks:
            raise ValueError(f"inode {ino} is outside the filesystem")
        return (agno * self.sb.agblocks + agbno) * self.sb.block_size + \
            slot * self.inode_size

    def fsb_to_byte(self, fsb: int) -> int:
        return self.sb.fsb_to_byte(fsb)


class CrashState:
    def __init__(self, image: bytes, geometry: Geometry, ino: int):
        self.image = image
        self.geo = geometry
        self.ino = ino
        self.offset = geometry.ino_byte_offset(ino)
        self.inode = image[self.offset:self.offset + geometry.inode_size]
        self.mtime = (be32(self.inode, DINODE_MTIME_OFF),
                      be32(self.inode, DINODE_MTIME_OFF + 4))
        self.lsn = be64(self.inode, DINODE_LSN_OFF)

    def inode_crc_ok(self) -> bool:
        raw = bytearray(self.inode)
        struct.pack_into("<I", raw, DINODE_CRC_OFF, 0)
        stored = struct.unpack_from("<I", self.inode, DINODE_CRC_OFF)[0]
        return log_probe.xfs_crc(bytes(raw)) == stored

    def times_text(self, other=None) -> str:
        if other is not None:
            return "old" if self.mtime == other.mtime else "new"
        return f"mtime={self.mtime} lsn={self.lsn:#x}"


def check(image_path: str, pristine_path: str, ino: int, expect_times: str,
          data: dict, expect_data: str) -> int:
    failures = []
    with open(image_path, "rb") as f:
        image = f.read()
    with open(pristine_path, "rb") as f:
        pristine = f.read()

    geometry = Geometry(image)
    state = CrashState(image, geometry, ino)
    old = CrashState(pristine, geometry, ino)

    log_failures, summary = log_probe.verify(image_path, "any", image)
    failures.extend(f"log: {failure}" for failure in log_failures)

    # 1. The inode must never be torn.
    if be16(state.inode, 0) != DINODE_MAGIC:
        failures.append("inode magic is not intact (torn inode write)")
    if state.inode[4] != 3:
        failures.append(f"inode version {state.inode[4]} (expected 3)")
    if not state.inode_crc_ok():
        failures.append("inode CRC mismatch (torn inode write)")

    # 2. Timestamps are either the old ones or the new ones, never a mix or
    #    garbage: if they changed, the log must describe the change.
    times = state.times_text(old)
    if expect_times != "any" and times != expect_times:
        failures.append(f"inode timestamps are {times}, expected "
                        f"{expect_times}")
    if times == "new":
        if state.lsn == 0:
            failures.append("inode timestamps changed but di_lsn is 0")
        match = None
        for record in summary["records"]:
            if record["lsn"] != state.lsn:
                continue
            for item in record["items"]:
                if item.get("type") == "inode" and item.get("ino") == ino:
                    match = item
        if match is None:
            failures.append(
                f"inode changed (mtime {state.mtime}) but no complete log "
                f"record with lsn {state.lsn:#x} describes it (write-ahead "
                f"violation)")
        else:
            if (match["mtime"]["sec"], match["mtime"]["nsec"]) != state.mtime:
                failures.append(
                    f"log record mtime {match['mtime']} does not match the "
                    f"inode's {state.mtime}")
            if match["blkno"] * 512 + match.get("boffset", 0) != state.offset:
                failures.append(
                    f"log record names block {match['blkno']} + offset "
                    f"{match.get('boffset', 0)}, inode is at {state.offset}")
    elif times == "old" and state.lsn != old.lsn:
        failures.append("inode LSN changed but the timestamps did not")

    # 3. File data: old or new, and it must match what the filesystem claims.
    data_state = None
    if data:
        offset = geometry.fsb_to_byte(data["fsb"]) + data["offset"]
        with open(data["file"], "rb") as f:
            want = f.read()
        got = image[offset:offset + len(want)]
        old_got = pristine[offset:offset + len(want)]
        if got == want:
            data_state = "new"
        elif got == old_got:
            data_state = "old"
        else:
            data_state = "mixed"
            failures.append("file data is neither the old nor the new content")
        if expect_data != "any" and data_state != expect_data:
            failures.append(f"file data is {data_state}, expected "
                            f"{expect_data}")

    print(f"{os.path.basename(image_path)}: log {summary['state']}, "
          f"records {len(summary['records'])}, torn {len(summary['torn_blocks'])}, "
          f"times {times}, data {data_state or '-'}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    return 0


def main(argv) -> int:
    if len(argv) < 4:
        print(__doc__)
        return 2
    image_path = argv[1]
    pristine_path = argv[2]
    ino = None
    expect_times = "any"
    expect_data = "any"
    data = {}
    index = 3
    while index < len(argv):
        arg = argv[index]
        value = argv[index + 1] if index + 1 < len(argv) else ""
        if arg == "--ino":
            ino = int(value)
            index += 2
        elif arg == "--expect-times":
            expect_times = value
            index += 2
        elif arg == "--expect-data":
            expect_data = value
            index += 2
        elif arg == "--data-fsb":
            data["fsb"] = int(value)
            index += 2
        elif arg == "--data-offset":
            data["offset"] = int(value)
            index += 2
        elif arg == "--data-file":
            data["file"] = value
            index += 2
        else:
            print(f"unknown argument {arg}", file=sys.stderr)
            return 2
    if ino is None:
        print("--ino is required", file=sys.stderr)
        return 2
    if data and len(data) != 3:
        print("--data-fsb, --data-offset and --data-file go together",
              file=sys.stderr)
        return 2
    return check(image_path, pristine_path, ino, expect_times, data,
                 expect_data)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
