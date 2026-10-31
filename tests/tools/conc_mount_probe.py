#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Mounted semantic check for the CONC-1 stress images.

Each stress test leaves a final namespace that the test already validated in
process. This probe re-reads the same namespace through a Linux kernel mount
(or, with --image, through the independent raw-image walk), so the directory
and inode views must agree:

  mixed:   root is the fixture's hello.bin plus conc-dir-0..N-1; each worker
           directory is empty with nlink 2, and hello.bin is the fixture's
           32 KiB deterministic file with nlink 1 and unchanged bytes.
  shared:  root is the fixture's hello.bin plus zero, one or two surviving
           hot-N.bin names; which hot names survive is the history's legal
           final state and is not hard-coded. Each survivor is an empty
           regular file with nlink 1, and hello.bin is unchanged.
  cow:     /stress-src.bin and /stress-dst.bin hold the last bytes their
           writer produced (delegated to conc_cow_probe.py).

usage: conc_mount_probe.py --mount DIR --workload mixed|shared|cow
                           [--threads N] [--seed N] [--iterations N] [--quiet]
       conc_mount_probe.py --image IMG --workload mixed|shared|cow
                           [--threads N] [--seed N] [--iterations N] [--quiet]
"""

import argparse
import hashlib
import json
import os
import stat
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import conc_cow_probe  # noqa: E402
import rw2_probe  # noqa: E402
import stateful_probe  # noqa: E402

# The fixture created by tests/support/fixture_image.h. Its pattern is also
# implemented independently in fixture_probe.py:
#   byte(i) = ((i * 167) ^ (i >> 7) ^ 0x5A) & 0xFF
FIXTURE_FILE = "hello.bin"
FIXTURE_SIZE = 32768
FIXTURE_PATTERN = bytes(
    ((i * 167) ^ (i >> 7) ^ 0x5A) & 0xFF for i in range(FIXTURE_SIZE))


def fail(message):
    raise SystemExit("conc_mount_probe: FAIL: %s" % message)


def list_dir(path):
    try:
        return sorted(os.listdir(path))
    except OSError as exc:
        fail("cannot list %s: %s" % (path, exc))


def check_fixture_bytes(path, data):
    if len(data) != FIXTURE_SIZE:
        fail("%s: read %d bytes, expected %d" %
             (path, len(data), FIXTURE_SIZE))
    for index, (got, want) in enumerate(zip(data, FIXTURE_PATTERN)):
        if got != want:
            fail("%s: fixture byte %d is %#x, expected %#x" %
                 (path, index, got, want))


def check_fixture_mounted(mount, quiet):
    full = os.path.join(mount, FIXTURE_FILE)
    try:
        st = os.stat(full)
    except OSError as exc:
        fail("cannot stat the fixture file %s: %s" % (full, exc))
    if not stat.S_ISREG(st.st_mode):
        fail("%s is not a regular file" % full)
    if st.st_nlink != 1:
        fail("%s: nlink %d, expected 1" % (full, st.st_nlink))
    if st.st_size != FIXTURE_SIZE:
        fail("%s: size %d, expected %d" %
             (full, st.st_size, FIXTURE_SIZE))
    with open(full, "rb") as handle:
        check_fixture_bytes(full, handle.read())
    if not quiet:
        print("fixture %s unchanged (%d bytes)" %
              (FIXTURE_FILE, FIXTURE_SIZE))


def raw_view(image):
    img = rw2_probe.Image(image)
    geo = stateful_probe.Geometry(img)
    listings, paths, records = stateful_probe.walk(img, geo)
    by_path = {path: ino for ino, names in paths.items() for path in names}
    return img, geo, listings, records, by_path


def check_fixture_raw(img, geo, records, by_path, quiet):
    ino = by_path.get("/" + FIXTURE_FILE)
    if ino is None:
        fail("raw namespace has no /%s" % FIXTURE_FILE)
    info = records[ino]
    if stateful_probe.inode_type(info) != "file":
        fail("/%s is not a regular file" % FIXTURE_FILE)
    if info["nlink"] != 1:
        fail("/%s: nlink %d, expected 1" %
             (FIXTURE_FILE, info["nlink"]))
    if info["size"] != FIXTURE_SIZE:
        fail("/%s: size %d, expected %d" %
             (FIXTURE_FILE, info["size"], FIXTURE_SIZE))
    check_fixture_bytes("/" + FIXTURE_FILE,
                        stateful_probe.read_data(img, geo, info))
    if not quiet:
        print("fixture %s unchanged (%d bytes, raw)" %
              (FIXTURE_FILE, FIXTURE_SIZE))


def check_mixed(mount, threads, quiet):
    expected = sorted(["conc-dir-%d" % i for i in range(threads)] +
                      [FIXTURE_FILE])
    names = list_dir(mount)
    if names != expected:
        fail("mixed root listing %r, expected %r" % (names, expected))
    check_fixture_mounted(mount, quiet)
    for i in range(threads):
        full = os.path.join(mount, "conc-dir-%d" % i)
        try:
            st = os.stat(full)
        except OSError as exc:
            fail("cannot stat %s: %s" % (full, exc))
        if not stat.S_ISDIR(st.st_mode):
            fail("%s is not a directory" % full)
        if st.st_nlink != 2:
            fail("%s: directory nlink %d, expected 2" % (full, st.st_nlink))
        if list_dir(full):
            fail("%s is not empty" % full)
    if not quiet:
        print("mixed: %d empty worker directories OK" % threads)


def check_shared(mount, quiet):
    allowed = (FIXTURE_FILE, "hot-0.bin", "hot-1.bin")
    names = list_dir(mount)
    unexpected = [name for name in names if name not in allowed]
    if unexpected:
        fail("shared root has unexpected entries %r" % (unexpected,))
    if FIXTURE_FILE not in names:
        fail("shared root is missing the fixture %s" % FIXTURE_FILE)
    check_fixture_mounted(mount, quiet)
    hot_names = [name for name in names if name != FIXTURE_FILE]
    for name in hot_names:
        full = os.path.join(mount, name)
        try:
            st = os.stat(full)
        except OSError as exc:
            fail("cannot stat %s: %s" % (full, exc))
        if not stat.S_ISREG(st.st_mode):
            fail("%s is not a regular file" % full)
        if st.st_size != 0:
            fail("%s: size %d, expected 0" % (full, st.st_size))
        if st.st_nlink != 1:
            fail("%s: nlink %d, expected 1" % (full, st.st_nlink))
    if not quiet:
        print("shared: %d surviving hot names OK (final state is serial)"
              % len(hot_names))


def check_mixed_raw(image, threads, quiet):
    img, geo, listings, records, by_path = raw_view(image)
    expected = sorted(["conc-dir-%d" % i for i in range(threads)] +
                      [FIXTURE_FILE])
    names = listings.get("/")
    if names != expected:
        fail("raw mixed root listing %r, expected %r" % (names, expected))
    check_fixture_raw(img, geo, records, by_path, quiet)
    for i in range(threads):
        path = "/conc-dir-%d" % i
        ino = by_path.get(path)
        if ino is None:
            fail("raw namespace has no %s" % path)
        info = records[ino]
        if stateful_probe.inode_type(info) != "dir":
            fail("%s is not a directory" % path)
        if info["nlink"] != 2:
            fail("%s: directory nlink %d, expected 2" %
                 (path, info["nlink"]))
        if listings.get(path) != []:
            fail("%s is not empty: %r" % (path, listings.get(path)))
    if not quiet:
        print("mixed: %d empty worker directories OK (raw)" % threads)


def check_shared_raw(image, quiet):
    img, geo, listings, records, by_path = raw_view(image)
    allowed = (FIXTURE_FILE, "hot-0.bin", "hot-1.bin")
    names = listings.get("/", [])
    unexpected = [name for name in names if name not in allowed]
    if unexpected:
        fail("raw shared root has unexpected entries %r" % (unexpected,))
    if FIXTURE_FILE not in names:
        fail("raw shared root is missing the fixture %s" % FIXTURE_FILE)
    check_fixture_raw(img, geo, records, by_path, quiet)
    hot_names = [name for name in names if name != FIXTURE_FILE]
    for name in hot_names:
        path = "/" + name
        info = records[by_path[path]]
        if stateful_probe.inode_type(info) != "file":
            fail("%s is not a regular file" % path)
        if info["size"] != 0:
            fail("%s: size %d, expected 0" % (path, info["size"]))
        if info["nlink"] != 1:
            fail("%s: nlink %d, expected 1" % (path, info["nlink"]))
    if not quiet:
        print("shared: %d surviving hot names OK (raw, final state is serial)"
              % len(hot_names))


def check_manifest(mount, manifest_path, quiet):
    try:
        with open(manifest_path, "r", encoding="utf-8-sig") as handle:
            manifest = json.load(handle)
    except (OSError, ValueError) as exc:
        fail("cannot read manifest %s: %s" % (manifest_path, exc))
    if manifest.get("workload") != "winfsp-concurrent":
        fail("manifest workload %r is not winfsp-concurrent" %
             (manifest.get("workload"),))
    prefix = manifest.get("prefix")
    if not isinstance(prefix, str) or not prefix:
        fail("manifest has no prefix")

    expected_files = {}
    for entry in manifest.get("files", []):
        rel = entry["path"].lstrip("/")
        if not rel.startswith(prefix + "-"):
            fail("manifest path %r does not use prefix %r" %
                 (entry["path"], prefix))
        expected_files[rel] = entry
    empty_dirs = [path.lstrip("/") for path in manifest.get("empty_dirs", [])]

    # Every first-level name under the prefix must be accounted for; other
    # pre-existing entries in the base image do not matter.
    expected_top = {rel.split("/", 1)[0] for rel in expected_files}
    expected_top.update(path.split("/", 1)[0] for path in empty_dirs)
    actual_top = {name for name in list_dir(mount)
                  if name.startswith(prefix + "-")}
    if actual_top != expected_top:
        fail("concurrency namespace mismatch: extra %r, missing %r" %
             (sorted(actual_top - expected_top),
              sorted(expected_top - actual_top)))

    for rel, entry in expected_files.items():
        full = os.path.join(mount, rel)
        try:
            st = os.stat(full)
        except OSError as exc:
            fail("cannot stat %s: %s" % (full, exc))
        if not stat.S_ISREG(st.st_mode):
            fail("%s is not a regular file" % full)
        if st.st_nlink != 1:
            fail("%s: nlink %d, expected 1" % (full, st.st_nlink))
        if st.st_size != int(entry["size"]):
            fail("%s: size %d, expected %d" %
                 (full, st.st_size, int(entry["size"])))
        with open(full, "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()
        if digest != entry["sha256"]:
            fail("%s: sha256 %s, expected %s" %
                 (full, digest, entry["sha256"]))

    expected_children = {}
    for rel in expected_files:
        parent, name = rel.rsplit("/", 1)
        expected_children.setdefault(parent, set()).add(name)
    for rel in empty_dirs:
        expected_children.setdefault(rel, set())
    for rel, children in expected_children.items():
        full = os.path.join(mount, rel)
        try:
            st = os.stat(full)
        except OSError as exc:
            fail("cannot stat %s: %s" % (full, exc))
        if not stat.S_ISDIR(st.st_mode):
            fail("%s is not a directory" % full)
        actual = set(list_dir(full))
        if actual != children:
            fail("%s contains %r, expected %r" %
                 (full, sorted(actual), sorted(children)))
    if not quiet:
        print("manifest: %d files and %d empty directories OK (prefix %s)" %
              (len(expected_files), len(empty_dirs), prefix))


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--mount", default=None,
                        help="check a mounted directory")
    parser.add_argument("--image", default=None,
                        help="check a raw image with the independent walk")
    parser.add_argument("--workload", required=True,
                        choices=("mixed", "shared", "cow", "manifest"))
    parser.add_argument("--manifest", default=None)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--seed", default=None)
    parser.add_argument("--iterations", default=None)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    if bool(args.mount) == bool(args.image):
        parser.error("give exactly one of --mount DIR or --image IMG")
    if args.workload == "mixed" and (args.threads < 1 or args.threads > 8):
        parser.error("--threads must be 1..8")

    if args.image:
        if args.workload == "mixed":
            check_mixed_raw(args.image, args.threads, args.quiet)
        elif args.workload == "shared":
            check_shared_raw(args.image, args.quiet)
        elif args.workload == "cow":
            cow_args = [args.image]
            if args.seed is not None:
                cow_args += ["--seed", args.seed]
            if args.iterations is not None:
                cow_args += ["--iterations", args.iterations]
            if args.quiet:
                cow_args.append("--quiet")
            return conc_cow_probe.main(cow_args)
        else:
            parser.error("--workload manifest requires --mount DIR")
        return 0

    if not os.path.isdir(args.mount):
        fail("%s is not a directory" % args.mount)
    if args.workload == "mixed":
        check_mixed(args.mount, args.threads, args.quiet)
    elif args.workload == "shared":
        check_shared(args.mount, args.quiet)
    elif args.workload == "manifest":
        if not args.manifest:
            parser.error("--workload manifest needs --manifest FILE")
        check_manifest(args.mount, args.manifest, args.quiet)
    else:
        seed = (conc_cow_probe.parse_uint(args.seed)
                if args.seed is not None else conc_cow_probe.parse_uint(
                    os.environ.get("XFSWIN_CONC_STRESS_SEED",
                                   str(conc_cow_probe.DEFAULT_SEED))))
        iterations = (conc_cow_probe.parse_uint(args.iterations)
                      if args.iterations is not None
                      else conc_cow_probe.parse_uint(
                          os.environ.get("XFSWIN_CONC_STRESS_ITERS",
                                         str(conc_cow_probe.DEFAULT_ITERS))))
        regions = conc_cow_probe.expected_regions(seed, iterations)
        conc_cow_probe.check_mounted(args.mount, regions, args.quiet)
        if not args.quiet:
            print("cow: both files match the last writer bytes (seed=%#x)"
                  % seed)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except AssertionError as exc:
        print("conc_mount_probe: FAIL: %s" % exc, file=sys.stderr)
        sys.exit(1)
