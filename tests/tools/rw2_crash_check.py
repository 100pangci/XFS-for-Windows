#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Crash-state checker for the RW-2 writer (extend / truncate).

Given an image left behind by `XFSWIN_CRASH_AT=<point>` during an extend or a
truncate, this tool requires the crash to be recoverable and the visible state
to be one of the two allowed ones:

  * every metadata buffer that was written is individually intact (CRC, magic,
    LSN, owner) - no torn metadata;
  * the log holds complete CRC-valid records plus at most one torn newest
    record;
  * the inode is never torn, and its extent map is valid (this uses the
    independent parser in rw2_probe.py, not the C++ one);
  * write-ahead logging holds: if the on-disk inode carries the LSN of a
    complete log record, that record must contain the matching inode item
    (same size, block count, format and - for an extent-format fork - the same
    packed extent records);
  * the file size is either the old or the new one, and the bytes below the
    new size are unchanged;
  * if the log is clean (the operation completed), the whole filesystem must be
    fully consistent.

usage:
  rw2_crash_check.py <image> <old-image> --ino N --mode extend|truncate
                     --old-size S --new-size S [--fill 0xNN] [--expect any|new]
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import log_probe  # noqa: E402
import rw2_probe  # noqa: E402


def le64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def inode_item_for(summary, ino):
    """Newest complete inode item for `ino` and its record LSN."""
    found = None
    for record in summary["records"]:
        for item in record["items"]:
            if item.get("type") == "inode" and item.get("ino") == ino:
                found = (record["lsn"], item)
    return found


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("old_image")
    parser.add_argument("--ino", type=int, required=True)
    parser.add_argument("--mode", choices=["extend", "truncate"], required=True)
    parser.add_argument("--old-size", type=int, required=True)
    parser.add_argument("--new-size", type=int, required=True)
    parser.add_argument("--expect", choices=["any", "new"], default="any")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv[1:])

    failures = []
    image = rw2_probe.Image(args.image)
    old_image = rw2_probe.Image(args.old_image)

    # 1. Every metadata buffer that reached the device must be intact.
    try:
        rw2_probe.check_image(image, verbose=False, blocks_only=True)
    except SystemExit as error:
        failures.append(f"torn metadata buffer: {error}")

    # 2. The log: complete records plus at most one torn newest record.
    log_failures, summary = log_probe.verify(args.image, "any")
    failures.extend(f"log: {failure}" for failure in log_failures)
    log_is_clean = summary.get("state") == "clean"

    # 3. The inode must parse and have a valid extent map.
    geo = rw2_probe.Geometry(image)
    inode = None
    try:
        inode = rw2_probe.read_inode(image, geo, args.ino)
    except SystemExit as error:
        failures.append(f"inode: {error}")

    # 4. Write-ahead logging: an inode at LSN L must be described by the
    #    complete record at L.
    if inode is not None:
        match = inode_item_for(summary, args.ino)
        if inode["lsn"] != 0 and match is not None and match[0] == inode["lsn"]:
            item = match[1]
            if item.get("core_size") != inode["size"]:
                failures.append(
                    f"logged size {item.get('core_size')} != on-disk "
                    f"{inode['size']} for lsn {inode['lsn']:#x}")
            if item.get("core_nblocks") != inode["nblocks"]:
                failures.append(
                    f"logged nblocks {item.get('core_nblocks')} != on-disk "
                    f"{inode['nblocks']}")
            if item.get("core_format") != inode["format"]:
                failures.append(
                    f"logged format {item.get('core_format')} != on-disk "
                    f"{inode['format']}")
            if item.get("core_nextents") != inode["nextents"]:
                failures.append(
                    f"logged nextents {item.get('core_nextents')} != on-disk "
                    f"{inode['nextents']}")
            if inode["format"] == 2 and item.get("region") is not None:
                want = item["region"]
                got = inode["raw"][176:176 + len(want)]
                if got != want:
                    failures.append(
                        "logged extent records differ from the on-disk fork")
        elif inode["lsn"] != 0 and not any(
                record["lsn"] == inode["lsn"] for record in summary["records"]):
            failures.append(
                f"inode lsn {inode['lsn']:#x} has no complete log record")

    # 5. Size and content: old or new, nothing in between.
    if inode is not None:
        allowed = {args.old_size, args.new_size}
        if inode["size"] not in allowed:
            failures.append(
                f"size {inode['size']} is neither the old ({args.old_size}) "
                f"nor the new size ({args.new_size})")
        old_inode = rw2_probe.read_inode(old_image, rw2_probe.Geometry(old_image),
                                         args.ino)
        old_data = rw2_probe.read_file_data(old_image,
                                            rw2_probe.Geometry(old_image),
                                            old_inode)
        data = rw2_probe.read_file_data(image, geo, inode)
        if args.mode == "truncate":
            # The bytes that stay visible must still be the old content; the
            # invisible tail of a partial last block may already be zeroed (the
            # truncate zeroes it before committing the size).
            retained = min(inode["size"], args.new_size)
            if data[:retained] != old_data[:retained]:
                failures.append("the bytes below the new size changed")
            if inode["size"] == args.old_size:
                for i in range(args.new_size, args.old_size):
                    if data[i] not in (old_data[i], 0):
                        failures.append(
                            f"byte {i} beyond the new EOF is garbage")
                        break
        else:
            # An extension may only add bytes after the old EOF.
            prefix = min(inode["size"], args.old_size)
            if data[:prefix] != old_data[:prefix]:
                failures.append("the old file content changed")

    # 6. A clean log means the operation completed (or never started): the
    #    whole filesystem must then be consistent. The per-point "old state vs
    #    new state" expectation is enforced by the Linux replay job, which sees
    #    the replayed state.
    if log_is_clean:
        try:
            rw2_probe.check_image(image, verbose=False)
        except SystemExit as error:
            failures.append(f"clean log but inconsistent filesystem: {error}")
    elif args.expect == "new" and inode is not None:
        if inode["size"] != args.new_size:
            failures.append(f"expected the new size, got {inode['size']}")

    if not args.quiet:
        print(f"{os.path.basename(args.image)}: log {summary.get('state')} "
              f"records {len(summary['records'])} "
              f"torn {len(summary.get('torn_blocks', []))} "
              f"size {inode['size'] if inode else '-'}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
