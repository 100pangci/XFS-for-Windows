#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-6C Windows verification after the Linux reverse pass.

The image was cloned, CoW-written, truncated and unlinked by Linux after the
Windows mutation.  This tool checks every file against the Linux-produced
manifest, runs the independent oracle, then performs one more clone / CoW /
partial-EOF grow and checks the result again.

usage: rw6c_windows_reverse.py --exe xfswin.exe --image reverse.img
       --manifest reverse.manifest.json
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys

BLOCK = 1024


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

    def cat(self, path):
        return self.run(["cat", self.image, path], binary=True)

    def check(self):
        self.run(["check", self.image])


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
    ap.add_argument("--manifest", required=True)
    args = ap.parse_args()

    manifest = json.load(open(args.manifest, encoding="utf-8"))
    run = Runner(args.exe, args.image)
    run.check()
    for rel in manifest.get("deleted", []):
        proc = subprocess.run([run.exe, "cat", run.image, "/" + rel],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode == 0:
            raise SystemExit("%s should be absent" % rel)
    for rel, want in manifest["files"].items():
        data = run.cat("/" + rel)
        if len(data) != want["size"] or sha(data) != want["sha256"]:
            raise SystemExit("%s does not match the Linux manifest" % rel)

    # More Windows activity on top of the Linux mutations.
    run.run(["create", run.image, "/rw6c/final.bin", "--rw-experimental",
             "--rw-reflink-qualification"])
    run.run(["reflink", run.image, "/linux/after.bin", "/rw6c/final.bin",
             "0", "0", "0", "--rw-experimental",
             "--rw-reflink-qualification"])
    # Make final.bin's last shared block partial, then grow it.  The grow must
    # CoW the shared block and zero the newly visible bytes.
    run.run(["truncate", run.image, "/rw6c/final.bin",
             str(2 * BLOCK + 123), "--rw-experimental",
             "--rw-reflink-qualification"])
    run.run(["truncate", run.image, "/rw6c/final.bin",
             str(2 * BLOCK + 250), "--rw-experimental",
             "--rw-reflink-qualification"])
    run.run(["pwrite", run.image, "/linux/source-clone.bin", "0",
             "--rw-experimental", "--rw-reflink-qualification",
             "--text=windows-final"])
    run.run(["unlink", run.image, "/linux/tiny-clone.bin",
             "--rw-experimental", "--rw-reflink-qualification"])
    proc = subprocess.run([run.exe, "cat", run.image, "/linux/tiny-clone.bin"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode == 0:
        raise SystemExit("tiny-clone should be absent")

    # Reconstruct the Linux files this pass touched.
    old_source = pattern(16 * BLOCK, 11)
    patch = pattern(BLOCK, 900)
    patched_source = patch + old_source[BLOCK:]
    new_data = pattern(2 * BLOCK, 901)
    partial = pattern(4 * BLOCK + 300, 29)
    after = (patched_source[:3 * BLOCK] + pattern(BLOCK, 903) +
             patched_source[4 * BLOCK:])
    expected = {
        "base/source.bin": patched_source,
        "base/clone-a.bin": old_source[:BLOCK],
        "base/cow.bin": pattern(BLOCK, 904) + old_source[BLOCK:2 * BLOCK] +
                        pattern(BLOCK, 73) + old_source[3 * BLOCK:],
        "base/three.bin": old_source,
        "base/partial.bin": partial,
        "base/partial-clone.bin": partial,
        "rw6c/from-linux-src.bin": patched_source,
        "rw6c/new.bin": new_data,
        "rw6c/new-clone.bin": new_data[:BLOCK],
        "rw6c/hole-clone.bin": partial + bytes(100),
        "linux/after.bin": after,
        "linux/new-clone.bin": new_data,
        "linux/source-clone.bin": b"windows-final" +
                                  patched_source[len(b"windows-final"):],
        "linux/tiny.bin": pattern(BLOCK, 905),
        "rw6c/final.bin": after[:2 * BLOCK + 123] + bytes(127),
    }
    for rel, data in expected.items():
        require(run.cat("/" + rel), data, rel)
    run.check()

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    oracle = os.path.join(repo, "tools", "rw6c_refcount_probe.py")
    cmd = [sys.executable, oracle, run.image]
    info = run.run(["info", run.image])
    if "rmapbt=yes" not in info:
        cmd.append("--allow-no-rmapbt")
    cmd.extend(["--json", os.path.join(os.path.dirname(run.image),
                                       "reverse-final.oracle.json")])
    proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        print((proc.stdout + proc.stderr).decode("utf-8", "replace"),
              file=sys.stderr)
        raise SystemExit("RW-6C oracle rejected the reverse-final image")
    print("RW-6C Windows reverse verification OK: %s" % args.image)
    return 0


if __name__ == "__main__":
    sys.exit(main())
