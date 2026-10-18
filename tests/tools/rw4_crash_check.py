#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Crash-state checker for the RW-4 writer (rename / link / symlink).

Given an image left behind by `XFSWIN_CRASH_AT=<point>` during one of the RW-4
namespace operations, this tool requires the crash to be recoverable:

  * every metadata buffer that reached the device is individually intact
    (CRC, magic, blkno, owner) - no torn metadata;
  * the log holds complete CRC-valid records plus at most one torn newest
    record;
  * write-ahead logging holds: an inode on disk that carries the LSN of a
    complete record must match the inode item of that record (core fields and
    the logged fork region);
  * when the log is clean (no transaction was interrupted) the whole
    filesystem must be consistent (the full rw4_probe namespace walk) and the
    visible state must be one of the scenario's two legal namespaces.

The authoritative old-or-new check runs after Linux replay (see
rw4_crash_replay_check.py); this tool cannot distinguish "the record was not
written yet" from "the operation never started", and it must not: both are the
old state, which replay produces.

usage:
  rw4_crash_check.py <image> <pristine-image> <scenario> [--params JSON]
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import log_probe  # noqa: E402
import rw2_probe  # noqa: E402
import rw3_probe  # noqa: E402
import rw4_probe  # noqa: E402


def read_allocated_inode(image, geo, ino):
    """Reads an inode, returning None for a pristine (free) inode image."""
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


def resolve(listings, root, path):
    """Resolves an absolute path over a namespace walk; (ino, ftype) or None."""
    ino = root
    ftype = 2
    for part in [p for p in path.split("/") if p]:
        found = listings.get(ino, {}).get(part.encode())
        if found is None:
            return None
        ino, ftype = found
    return ino, ftype


def check_state(image, geo, scenario, params, failures):
    """The clean-log namespace must be the old or the complete new one."""
    refs, parents, listings = rw4_probe.check_namespace(image, geo, False)

    def entry(path):
        found = resolve(listings, geo.rootino, path)
        if found is None:
            return None
        inode = rw2_probe.read_inode(image, geo, found[0])
        return inode

    def nlink(path):
        inode = entry(path)
        return None if inode is None else inode["nlink"]

    def error(message):
        failures.append(f"{scenario}: {message}")

    if scenario == "renamesame":
        a = entry("/a.bin")
        b = entry("/b.bin")
        if a is not None and b is None:
            if a["size"] != params["size"]:
                error("old state but the size differs")
        elif a is None and b is not None:
            if b["size"] != params["size"]:
                error("new state but the size differs")
        else:
            error("neither the old (a) nor the new (b) state")
    elif scenario == "renamecross":
        a = entry("/A/f")
        b = entry("/B/f")
        if a is not None and b is None:
            pass
        elif a is None and b is not None:
            if b["size"] != params["size"]:
                error("B/f has the wrong size")
        else:
            error("neither A/f nor B/f is the single owner")
    elif scenario == "renamereplace":
        src = entry("/src")
        dst = entry("/dst")
        if src is not None and dst is not None:
            if src["size"] != params["src_size"] or \
                    dst["size"] != params["dst_size"]:
                error("old state but the sizes differ")
        elif src is None and dst is not None:
            if dst["size"] != params["src_size"]:
                error("dst does not serve the moved inode")
        else:
            error("neither the old nor the new state")
    elif scenario == "dirmove":
        old = entry("/A/child")
        new = entry("/B/child")
        if old is not None and new is None:
            if nlink("/A") != 3:
                error("old state but A nlink != 3")
        elif old is None and new is not None:
            if nlink("/B") != 3 or nlink("/A") != 2:
                error("new state but the parent link counts are wrong")
        else:
            error("neither A/child nor B/child is the single owner")
    elif scenario == "dirreplace":
        s = entry("/s")
        t = entry("/t")
        root_nlink = nlink("/")
        if s is not None and t is not None:
            if root_nlink != 4:
                error(f"old state but root nlink {root_nlink} != 4")
        elif s is None and t is not None:
            if root_nlink != 3:
                error(f"new state but root nlink {root_nlink} != 3")
        else:
            error("neither the old nor the new state")
    elif scenario == "link":
        src = entry("/file")
        dst = entry("/linked")
        if src is not None and dst is None:
            if src["nlink"] != 1:
                error("old state but nlink != 1")
        elif src is not None and dst is not None:
            if src["nlink"] != 2:
                error("new state but nlink != 2")
            elif resolve(listings, geo.rootino, "/file")[0] != \
                    resolve(listings, geo.rootino, "/linked")[0]:
                error("the two names do not share an inode")
        else:
            error("neither the old nor the new state")
    elif scenario in ("symlink", "symlinkremote"):
        name = "/sl" if scenario == "symlink" else "/slr"
        found = resolve(listings, geo.rootino, name)
        if found is None:
            return
        inode = rw2_probe.read_inode(image, geo, found[0])
        if rw4_probe.inode_type(inode) != 10:
            error(f"{name} is not a symlink")
            return
        target = rw4_probe.symlink_target(image, geo, inode)
        if target != params["target"].encode():
            error(f"{name} target {target!r} != {params['target']!r}")
        if inode["nlink"] != 1:
            error(f"{name} nlink {inode['nlink']} != 1")
    else:
        error(f"unknown scenario {scenario}")


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("pristine")
    parser.add_argument("scenario")
    parser.add_argument("--params", default="{}")
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
            params = json.loads(args.params)
            check_state(image, geo, args.scenario, params, failures)
        except SystemExit as error:
            failures.append(f"clean log but inconsistent filesystem: {error}")

    if not args.quiet:
        print(f"{os.path.basename(args.image)}: log {summary.get('state')} "
              f"records {len(summary['records'])} "
              f"torn {len(summary.get('torn_blocks', []))} "
              f"scenario {args.scenario}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
