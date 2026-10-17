#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Content verification for the RW-2 Linux round trip.

Verifies a file through the mounted filesystem (the kernel is the reader, so
this also proves Linux accepts the extent map) against the patterns the Windows
writer was asked to produce.

usage: rw2_content.py <mounted-file> <manifest.json> <image-basename>
"""
import json
import os
import sys

BLOCK = 4096


def fixture_byte(i):
    return ((i * 167) ^ (i >> 7) ^ 0x5A) & 0xFF


def extend_byte(i):
    return ((i * 131) + 7) & 0xFF


def overlay_byte(i):
    # Same pattern as tests/tools/rw_content.py overlay().
    return (0xC3 ^ (i * 37)) & 0xFF


def expect_file(path, size, content_fn):
    actual = os.path.getsize(path)
    if actual != size:
        raise SystemExit(f"{path}: size {actual} != {size}")
    with open(path, "rb") as f:
        data = f.read()
    for i in range(size):
        want = content_fn(i)
        if data[i] != want:
            raise SystemExit(
                f"{path}: byte {i} is {data[i]:#x}, expected {want:#x}")


def main():
    mounted, manifest_path, image = sys.argv[1:4]
    with open(manifest_path) as f:
        manifest = json.load(f)
    old_size = manifest["old_size"]
    if image == "extend.img":
        expect_file(mounted, manifest["extend_size"],
                    lambda i: fixture_byte(i) if i < old_size else extend_byte(i))
    elif image == "truncate.img":
        expect_file(mounted, manifest["truncate_size"],
                    lambda i: fixture_byte(i))
    elif image == "reuse.img":
        expect_file(mounted, manifest["reuse_size"], lambda i: 0x5A)
    elif image == "btree.img":
        size = manifest["btree_size"]
        stride = 2 * BLOCK

        def btree_byte(i):
            if i < old_size:
                return fixture_byte(i)
            rel = i - old_size
            block = rel // stride
            within = rel % stride
            if block < manifest["btree_extents"]:
                if within < BLOCK:
                    return overlay_byte(within)
                return 0  # hole between the one-block writes
            return 0

        expect_file(mounted, size, btree_byte)
    elif image == "reverse.img":
        size = manifest["extend_size"] + len("LINUX-AFTER-WINDOWS")
        with open(mounted, "rb") as f:
            data = f.read()
        if len(data) != size:
            raise SystemExit(f"{mounted}: size {len(data)} != {size}")
        if not data.endswith(b"LINUX-AFTER-WINDOWS"):
            raise SystemExit(f"{mounted}: missing the Linux append")
        for i in range(manifest["extend_size"]):
            want = fixture_byte(i) if i < old_size else extend_byte(i)
            if data[i] != want:
                raise SystemExit(f"{mounted}: byte {i} changed")
    else:
        raise SystemExit(f"unknown image {image}")
    print(f"{image}: content verified ({os.path.getsize(mounted)} bytes)")


if __name__ == "__main__":
    main()
