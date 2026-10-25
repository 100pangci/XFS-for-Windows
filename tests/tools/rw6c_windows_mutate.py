#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-6C Windows-side mutation of a Linux-created reflink image.

The source fixture is produced by tests/scripts/make_rw6c_linux.sh.  This tool
performs native clones, CoW writes, a shared truncate and a shared unlink, then
checks every file byte-for-byte and runs the independent raw oracle.  It also
writes mutated.manifest.json for the Linux verifier.

usage: rw6c_windows_mutate.py --exe xfswin.exe --image in.img
       --work-dir out/ [--json manifest.json]
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys

BLOCK = 4096


def pattern(size, seed):
    return bytes(((i * 37 + seed * 17 + 11) & 0xFF) for i in range(size))


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Runner:
    def __init__(self, exe, image):
        self.exe = os.path.abspath(exe)
        self.image = os.path.abspath(image)

    def run(self, args, binary=False):
        proc = subprocess.run([self.exe] + args, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        if proc.returncode != 0:
            out = (proc.stdout + proc.stderr).decode("utf-8", "replace")
            raise SystemExit("xfswin %s failed (exit %d):\n%s" %
                             (" ".join(args), proc.returncode, out))
        return proc.stdout if binary else \
            proc.stdout.decode("utf-8", "replace")

    def check(self):
        self.run(["check", self.image])

    def cat(self, path):
        return self.run(["cat", self.image, path], binary=True)

    def expect_missing(self, path):
        proc = subprocess.run([self.exe, "cat", self.image, path],
                              stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        if proc.returncode == 0:
            raise SystemExit("%s is still readable" % path)

    def create(self, path):
        self.run(["create", self.image, path, "--rw-experimental",
                  "--rw-reflink-qualification"])

    def mkdir(self, path):
        self.run(["mkdir", self.image, path, "--rw-experimental",
                  "--rw-reflink-qualification"])

    def put(self, path, data, work):
        temp = os.path.join(work, "input.bin")
        with open(temp, "wb") as f:
            f.write(data)
        self.run(["pwrite", self.image, path, "0", "--rw-experimental",
                  "--rw-reflink-qualification", "--from=" + temp])

    def clone(self, src, dst, length=0):
        self.run(["reflink", self.image, src, dst, "0", "0", str(length),
                  "--rw-experimental", "--rw-reflink-qualification"])

    def truncate(self, path, size):
        self.run(["truncate", self.image, path, str(size),
                  "--rw-experimental", "--rw-reflink-qualification"])

    def unlink(self, path):
        self.run(["unlink", self.image, path, "--rw-experimental",
                  "--rw-reflink-qualification"])


def require(actual, expected, what):
    if actual != expected:
        if len(actual) != len(expected):
            raise SystemExit("%s: size %d != %d" %
                             (what, len(actual), len(expected)))
        for i, (a, b) in enumerate(zip(actual, expected)):
            if a != b:
                raise SystemExit("%s: byte %d is 0x%02x, expected 0x%02x" %
                                 (what, i, a, b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--work-dir", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()

    os.makedirs(args.work_dir, exist_ok=True)
    run = Runner(args.exe, args.image)
    run.check()

    old_source = pattern(16 * BLOCK, 11)
    patch = pattern(BLOCK, 900)
    new_data = pattern(2 * BLOCK, 901)
    partial = pattern(4 * BLOCK + 300, 29)
    cow_patch = pattern(BLOCK, 73)
    wide = bytearray((1200 * 2 - 1) * BLOCK)
    for i in range(1200):
        offset = i * 2 * BLOCK
        wide[offset:offset + BLOCK] = pattern(BLOCK, 300 + i)
    wide = bytes(wide)

    require(run.cat("/base/source.bin"), old_source, "Linux source")
    require(run.cat("/base/clone-a.bin"), old_source, "Linux clone-a")
    require(run.cat("/base/clone-b.bin"), old_source, "Linux clone-b")
    require(run.cat("/base/cow.bin"),
            old_source[:2 * BLOCK] + cow_patch +
            old_source[3 * BLOCK:], "Linux CoW clone")
    require(run.cat("/base/partial.bin"), partial, "Linux partial")
    require(run.cat("/base/partial-clone.bin"), partial,
            "Linux partial clone")
    require(run.cat("/base/wide.bin"), wide, "Linux wide source")
    require(run.cat("/base/wide-clone.bin"), wide, "Linux wide clone")
    require(run.cat("/base/wide-second.bin"), wide, "Linux wide second clone")

    # CoW the first block of a three-way shared source.  The untouched clone
    # must keep the old bytes.
    run.put("/base/source.bin", patch, args.work_dir)
    patched_source = patch + old_source[BLOCK:]
    require(run.cat("/base/source.bin"), patched_source, "patched source")
    require(run.cat("/base/clone-a.bin"), old_source,
            "clone-a after source CoW")

    run.mkdir("/rw6c")
    run.create("/rw6c/from-linux-src.bin")
    run.clone("/base/source.bin", "/rw6c/from-linux-src.bin")
    require(run.cat("/rw6c/from-linux-src.bin"), patched_source,
            "clone of patched source")

    run.create("/rw6c/new.bin")
    run.put("/rw6c/new.bin", new_data, args.work_dir)
    run.create("/rw6c/new-clone.bin")
    run.clone("/rw6c/new.bin", "/rw6c/new-clone.bin")
    require(run.cat("/rw6c/new-clone.bin"), new_data, "new clone")

    run.create("/rw6c/three-clone.bin")
    run.clone("/base/three.bin", "/rw6c/three-clone.bin")
    require(run.cat("/rw6c/three-clone.bin"), old_source,
            "three-way clone")

    run.create("/rw6c/hole-clone.bin")
    run.clone("/base/partial.bin", "/rw6c/hole-clone.bin")
    grown_size = 4 * BLOCK + 400
    run.truncate("/rw6c/hole-clone.bin", grown_size)
    require(run.cat("/rw6c/hole-clone.bin"), partial + bytes(100),
            "grown partial clone")

    run.truncate("/base/clone-a.bin", BLOCK)
    require(run.cat("/base/clone-a.bin"), old_source[:BLOCK],
            "truncated clone-a")
    run.unlink("/base/clone-b.bin")
    run.expect_missing("/base/clone-b.bin")
    run.check()

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    oracle = os.path.join(repo, "tools", "rw6c_refcount_probe.py")
    cmd = [sys.executable, oracle, os.path.abspath(args.image)]
    # Auto-detect rmapbt: the oracle rejects a missing rmapbt by default.
    info = run.run(["info", args.image])
    if "rmapbt=yes" not in info:
        cmd.append("--allow-no-rmapbt")
    oracle_json = os.path.join(args.work_dir, "oracle.json")
    cmd.extend(["--json", oracle_json])
    proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        print((proc.stdout + proc.stderr).decode("utf-8", "replace"),
              file=sys.stderr)
        raise SystemExit("RW-6C independent oracle rejected the mutated image")

    expected = {
        "base/source.bin": patched_source,
        "base/clone-a.bin": old_source[:BLOCK],
        "base/cow.bin": old_source[:2 * BLOCK] + cow_patch +
                        old_source[3 * BLOCK:],
        "base/three.bin": old_source,
        "base/partial.bin": partial,
        "base/partial-clone.bin": partial,
        "rw6c/from-linux-src.bin": patched_source,
        "rw6c/new.bin": new_data,
        "rw6c/new-clone.bin": new_data,
        "rw6c/three-clone.bin": old_source,
        "rw6c/hole-clone.bin": partial + bytes(100),
        "base/wide.bin": wide,
        "base/wide-clone.bin": wide,
        "base/wide-second.bin": wide,
    }
    manifest = {
        "kind": "rw6c-windows-mutated",
        "image": os.path.basename(args.image),
        "deleted": ["base/clone-b.bin"],
        "files": {
            name: {"size": len(data), "sha256": sha(data)}
            for name, data in expected.items()
        },
        "block_size": BLOCK,
        "seeds": {"shared_pairs": 100, "wide_extents": 1200},
    }
    out = args.json or os.path.join(args.work_dir, "mutated.manifest.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    print("RW-6C Windows mutation OK: %s" % args.image)
    print("manifest: %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
