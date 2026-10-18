#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Crash-state checker for the RW-3 writer (create / mkdir / unlink / rmdir).

Given an image left behind by `XFSWIN_CRASH_AT=<point>` during a namespace
mutation, this tool requires the crash to be recoverable:

  * every metadata buffer that reached the device is individually intact (CRC,
    magic, blkno, owner) - no torn metadata;
  * the log holds complete CRC-valid records plus at most one torn newest
    record;
  * write-ahead logging holds: an inode on disk that carries the LSN of a
    complete record must match the inode item of that record (core fields and
    the logged fork region);
  * when the log is clean (no transaction was interrupted) the whole
    filesystem must be consistent (the full rw3_probe checks) and the
    namespace must show either the old or the complete new state.

The authoritative old-or-new check runs after Linux replay (see
rw3_crash_replay_check.py); this tool cannot distinguish "the record was not
written yet" from "the operation never started", and it must not: both are the
old state, which replay produces.

usage:
  rw3_crash_check.py <image> <pristine-image> --mode create|mkdir|unlink|rmdir
                     --name /c-new.bin [--old-size S] [--quiet]
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import log_probe  # noqa: E402
import rw2_probe  # noqa: E402
import rw3_probe  # noqa: E402


def read_allocated_inode(image, geo, ino):
    """Reads an inode, returning None for a pristine (free) inode image.

    The crash may have left the inode buffer at its old content: a free inode
    image (mode 0, format 0) is what the formatter wrote, not torn data. A bad
    magic or CRC still fails."""
    try:
        return rw2_probe.read_inode(image, geo, ino)
    except SystemExit:
        raw = image.read(geo.inode_offset(ino), geo.inodesize)
        if rw2_probe.u16(raw, 0) == 0x494E and rw2_probe.u16(raw, 2) == 0 and \
                raw[5] == 0:
            return None
        raise


def check_write_ahead(image, geo, summary, failures):
    """Every on-disk inode at a complete record's LSN must match its item."""
    seen = set()
    for record in summary["records"]:
        for item in record["items"]:
            if item.get("type") != "inode" or item.get("ino") in seen:
                continue
            ino = item["ino"]
            seen.add(ino)
            try:
                inode = read_allocated_inode(image, geo, ino)
            except SystemExit as error:
                failures.append(f"inode {ino}: {error}")
                continue
            if inode is None or inode["lsn"] == 0 or \
                    inode["lsn"] != record["lsn"]:
                continue
            if item.get("core_size") is not None and \
                    item["core_size"] != inode["size"]:
                failures.append(
                    f"inode {ino}: logged size {item['core_size']} != on-disk "
                    f"{inode['size']} at lsn {inode['lsn']:#x}")
            if item.get("core_nblocks") is not None and \
                    item["core_nblocks"] != inode["nblocks"]:
                failures.append(
                    f"inode {ino}: logged nblocks {item['core_nblocks']} != "
                    f"on-disk {inode['nblocks']}")
            if item.get("core_format") is not None and \
                    item["core_format"] != inode["format"]:
                failures.append(
                    f"inode {ino}: logged format {item['core_format']} != "
                    f"on-disk {inode['format']}")
            if item.get("core_nlink") is not None and \
                    item["core_nlink"] != inode["nlink"]:
                failures.append(
                    f"inode {ino}: logged nlink {item['core_nlink']} != "
                    f"on-disk {inode['nlink']}")
            if inode["format"] == 2 and item.get("region") is not None:
                want = item["region"]
                got = inode["raw"][176:176 + len(want)]
                if got != want:
                    failures.append(
                        f"inode {ino}: logged extent records differ from the "
                        f"on-disk fork")


def parse_name(name):
    parent, _, base = name.rstrip("/").rpartition("/")
    return (parent or "/", base)


def check_state(image, geo, mode, name, old_size, failures):
    """Requires the visible namespace state to be old or complete new.

    "Absent" is always one of the two states (the old one for create, mkdir
    and rmdir, the new one for unlink); a present entry must be complete."""
    parent, base = parse_name(name)
    try:
        node_ino = geo.rootino
        for part in [p for p in parent.split("/") if p]:
            listing = rw3_probe.enumerate_directory(image, geo, node_ino)
            entry = listing.get(part.encode())
            if entry is None:
                failures.append(f"parent {parent} does not exist")
                return
            node_ino = entry[0]
        listing = rw3_probe.enumerate_directory(image, geo, node_ino)
    except SystemExit as error:
        failures.append(f"cannot read the namespace: {error}")
        return
    entry = listing.get(base.encode())
    if entry is None:
        return
    ino, ftype = entry
    if mode in ("create", "unlink"):
        if ftype != 1:
            failures.append(f"{name} exists but is not a regular file "
                            f"(ftype {rw3_probe.FTYPE_NAME.get(ftype)})")
            return
        inode = rw2_probe.read_inode(image, geo, ino)
        if inode["nlink"] == 0:
            failures.append(f"{name} exists with nlink 0")
        if mode == "create" and inode["size"] != 0:
            failures.append(f"{name} was created with size {inode['size']}")
        if mode == "unlink" and old_size is not None and \
                inode["size"] != old_size:
            failures.append(f"{name} has size {inode['size']}, neither the "
                            f"old {old_size} nor removed")
    else:
        if ftype != 2:
            failures.append(f"{name} exists but is not a directory")
        else:
            sub = rw3_probe.enumerate_directory(image, geo, ino)
            real = [n for n in sub if n not in (b".", b"..")]
            if mode == "rmdir" and real:
                failures.append(f"{name} is not empty")


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("pristine")
    parser.add_argument("--mode", choices=["create", "mkdir", "unlink", "rmdir"],
                        required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--old-size", type=int, default=None)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv[1:])

    failures = []
    image = rw2_probe.Image(args.image)

    # 1. Every metadata buffer that reached the device must be intact.
    try:
        rw2_probe.check_image(image, verbose=False, blocks_only=True)
    except SystemExit as error:
        failures.append(f"torn metadata buffer: {error}")

    # 2. The log: complete records plus at most one torn newest record.
    log_failures, summary = log_probe.verify(args.image, "any")
    failures.extend(f"log: {failure}" for failure in log_failures)
    log_is_clean = summary.get("state") == "clean"

    geo = rw2_probe.Geometry(image)

    # 3. Write-ahead logging.
    check_write_ahead(image, geo, summary, failures)

    # 4. A clean log means no transaction was interrupted: then the whole
    #    filesystem has to be consistent and the namespace has to show one of
    #    the two allowed states.
    if log_is_clean:
        try:
            rw2_probe.check_image(image, verbose=False)
            rw3_probe.check_inobt(image, geo)
            check_state(image, geo, args.mode, args.name, args.old_size,
                        failures)
        except SystemExit as error:
            failures.append(f"clean log but inconsistent filesystem: {error}")

    if not args.quiet:
        print(f"{os.path.basename(args.image)}: log {summary.get('state')} "
              f"records {len(summary['records'])} "
              f"torn {len(summary.get('torn_blocks', []))} "
              f"mode {args.mode}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
