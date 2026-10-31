#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Independent CONC-1 CoW stress image oracle.

Reads the image exported by conc_stress_cow_both_sides_seeded and checks,
using only the raw on-disk format, that:

  * the root directory contains exactly the two stress files;
  * each file is a regular inode with nlink 1 and size 8192;
  * every byte equals the last pattern its writer produced (writers w=0/1
    target the source file at offsets 0/4096, w=2/3 the destination file);
  * no extent is unwritten and the two files no longer share any block.

The expected pattern is data[b] = (b*53 + iter*29 + w*7 + seed) & 0xff for
the last iteration, matching tests/unit/conc_tests.cpp. The seed and iteration
count default to the test defaults and can be overridden with --seed /
--iterations or XFSWIN_CONC_STRESS_SEED / XFSWIN_CONC_STRESS_ITERS (same
variables that generated the image).

usage: conc_cow_probe.py <image> [--seed N] [--iterations N] [--quiet]
"""

import argparse
import os
import stat
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rw2_probe  # noqa: E402
import rw6e_parent_probe  # noqa: E402
import stateful_probe  # noqa: E402

SRC_PATH = "/stress-src.bin"
DST_PATH = "/stress-dst.bin"
FILE_SIZE = 8192
CHUNK = 4096
DEFAULT_SEED = 0xC0DEC0DE1234
DEFAULT_ITERS = 4


def fail(message):
    raise SystemExit("conc_cow_probe: FAIL: %s" % message)


def parse_uint(text):
    if isinstance(text, int):
        return text
    return int(text, 0)


def expected_regions(seed, iterations):
    if iterations < 1:
        fail("iterations must be >= 1")
    last_iter = iterations - 1
    regions = []
    for writer in range(4):
        regions.append(bytes(
            (b * 53 + last_iter * 29 + writer * 7 + seed) & 0xFF
            for b in range(CHUNK)))
    return regions  # [src@0, src@4096, dst@0, dst@4096]


def file_blocks(record):
    blocks = set()
    for startoff, startblock, blockcount, unwritten in record["extents"]:
        if unwritten:
            fail("inode %d has an unwritten extent at file offset %d" %
                 (record["ino"], startoff))
        for block in range(startblock, startblock + blockcount):
            blocks.add(block)
    return blocks


def check_bytes(path, data, regions):
    size = len(regions) * CHUNK
    if len(data) != size:
        fail("%s: read %d bytes, expected %d" % (path, len(data), size))
    for index, expected in enumerate(regions):
        offset = index * CHUNK
        actual = data[offset:offset + CHUNK]
        if actual != expected:
            for b, (got, want) in enumerate(zip(actual, expected)):
                if got != want:
                    fail("%s: byte %d is %#x, expected %#x" %
                         (path, offset + b, got, want))
            fail("%s: region %d differs" % (path, index))


def check_file(img, geo, path, ino, regions, quiet):
    info = rw6e_parent_probe.inode_record(img, geo, ino)
    size = len(regions) * CHUNK
    if stateful_probe.inode_type(info) != "file":
        fail("%s: inode %d is not a regular file" % (path, ino))
    if info["size"] != size:
        fail("%s: size %d, expected %d" % (path, info["size"], size))
    if info["nlink"] != 1:
        fail("%s: nlink %d, expected 1" % (path, info["nlink"]))
    data = stateful_probe.read_data(img, geo, info)
    check_bytes(path, data, regions)
    if not quiet:
        print("%s: inode %d, %d bytes, %d extents OK" %
              (path, ino, size, len(info["extents"])))
    return info


def check_mounted(mount, regions, quiet):
    try:
        names = sorted(os.listdir(mount))
    except OSError as exc:
        fail("cannot list %s: %s" % (mount, exc))
    if names != ["stress-dst.bin", "stress-src.bin"]:
        fail("mounted root listing %r, expected the two stress files" %
             (names,))
    for path, file_regions in ((SRC_PATH, regions[0:2]),
                               (DST_PATH, regions[2:4])):
        full = os.path.join(mount, path.lstrip("/"))
        try:
            st = os.stat(full)
        except OSError as exc:
            fail("cannot stat %s: %s" % (full, exc))
        if not stat.S_ISREG(st.st_mode):
            fail("%s is not a regular file" % full)
        if st.st_nlink != 1:
            fail("%s: nlink %d, expected 1" % (full, st.st_nlink))
        if st.st_size != len(file_regions) * CHUNK:
            fail("%s: size %d, expected %d" %
                 (full, st.st_size, len(file_regions) * CHUNK))
        with open(full, "rb") as handle:
            data = handle.read()
        check_bytes(full, data, file_regions)
        if not quiet:
            print("%s: mounted file %d bytes OK" % (full, st.st_size))


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("image", nargs="?",
                        help="raw image (omit with --mounted)")
    parser.add_argument("--mounted", default=None,
                        help="check a mounted directory instead of a raw "
                             "image")
    parser.add_argument("--seed", default=None,
                        help="writer seed (default: XFSWIN_CONC_STRESS_SEED "
                             "or 0xc0dec0de1234)")
    parser.add_argument("--iterations", default=None,
                        help="writer iterations (default: "
                             "XFSWIN_CONC_STRESS_ITERS or 4)")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    if bool(args.image) == bool(args.mounted):
        parser.error("give exactly one of an image path or --mounted DIR")

    seed = parse_uint(args.seed) if args.seed is not None else parse_uint(
        os.environ.get("XFSWIN_CONC_STRESS_SEED", str(DEFAULT_SEED)))
    iterations = (parse_uint(args.iterations) if args.iterations is not None
                  else parse_uint(
                      os.environ.get("XFSWIN_CONC_STRESS_ITERS",
                                     str(DEFAULT_ITERS))))

    regions = expected_regions(seed, iterations)
    if args.mounted:
        check_mounted(args.mounted, regions, args.quiet)
        if not args.quiet:
            print("conc_cow_probe: mounted view ok (seed=%#x, iterations=%d)"
                  % (seed, iterations))
        return 0

    img = rw2_probe.Image(args.image)
    geo = stateful_probe.Geometry(img)
    listings, paths, records = stateful_probe.walk(img, geo)

    root_names = listings.get("/")
    if root_names != ["stress-dst.bin", "stress-src.bin"]:
        fail("root listing %r, expected the two stress files" % (root_names,))
    by_path = {}
    for ino, names in paths.items():
        for name in names:
            by_path[name] = ino
    for path in (SRC_PATH, DST_PATH):
        if path not in by_path:
            fail("%s is missing from the walked namespace" % path)
        if len(paths[by_path[path]]) != 1:
            fail("%s has %d names, expected 1 (%r)" %
                 (path, len(paths[by_path[path]]), paths[by_path[path]]))

    src_info = check_file(img, geo, SRC_PATH, by_path[SRC_PATH],
                          regions[0:2], args.quiet)
    dst_info = check_file(img, geo, DST_PATH, by_path[DST_PATH],
                          regions[2:4], args.quiet)

    if src_info["ino"] == dst_info["ino"]:
        fail("source and destination resolved to the same inode")
    shared = file_blocks(src_info) & file_blocks(dst_info)
    if shared:
        fail("source and destination still share %d block(s), e.g. %d" %
             (len(shared), min(shared)))

    if not args.quiet:
        print("conc_cow_probe: ok (seed=%#x, iterations=%d, src ino %d, "
              "dst ino %d, no shared extents)" %
              (seed, iterations, src_info["ino"], dst_info["ino"]))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except AssertionError as exc:
        print("conc_cow_probe: FAIL: %s" % exc, file=sys.stderr)
        sys.exit(1)
