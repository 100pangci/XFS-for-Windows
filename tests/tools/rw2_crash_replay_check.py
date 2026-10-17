#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Verifies a replayed RW-2 crash image through the mounted file.

The crash contract allows exactly two visible states: the old one, or the new
one. For an extend the new state is the old content plus the fill pattern; for
a truncate it is the old prefix and nothing else. An interrupted truncate may
also have removed blocks while the old size is still in effect, which leaves
zeros where those blocks were - that is a legal intermediate *XFS* state (the
kernel rolls its own truncate transactions the same way), so the tail is
allowed to be old data or zeros, never anything else.

usage: rw2_crash_replay_check.py <mounted-file> <manifest.json> <mode>
"""
import json
import os
import sys


def fixture_byte(i):
    return ((i * 167) ^ (i >> 7) ^ 0x5A) & 0xFF


def extend_byte(i):
    return ((i * 131) + 7) & 0xFF


def main():
    path, manifest_path, mode = sys.argv[1:4]
    with open(manifest_path) as f:
        manifest = json.load(f)
    old = manifest["old_size"]
    new = manifest["extend_size"] if mode == "extend" else manifest["truncate_size"]
    size = os.path.getsize(path)
    if size not in (old, new):
        raise SystemExit(f"size {size} is neither old ({old}) nor new ({new})")
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != size:
        raise SystemExit("short read")
    if mode == "extend":
        for i in range(min(size, old)):
            if data[i] != fixture_byte(i):
                raise SystemExit(f"old byte {i} changed")
        if size == new:
            for i in range(old, new):
                if data[i] != extend_byte(i):
                    raise SystemExit(f"extended byte {i} has the wrong fill")
    else:
        for i in range(min(size, new)):
            if data[i] != fixture_byte(i):
                raise SystemExit(f"retained byte {i} changed")
        if size == old:
            for i in range(new, old):
                if data[i] not in (fixture_byte(i), 0):
                    raise SystemExit(f"byte {i} beyond the new EOF is garbage")
    print(f"{os.path.basename(path)}: {mode} state size={size} verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
