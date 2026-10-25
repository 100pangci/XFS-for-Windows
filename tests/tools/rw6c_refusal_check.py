#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-6C no-write refusal matrix.

Ordinary `--rw-experimental` must keep refusing a reflink filesystem, and every
refusal must leave the image byte-identical.  The hidden qualification switch
must not be usable without `--rw-experimental` either.

usage: rw6c_refusal_check.py --exe <xfswin> --image <reflink.img>
       [--require-rmapbt] [--json out]

By default the image must contain /src, /dst and an empty /emptydir; use
--src/--dst/--empty-dir to point at another fixture.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(exe, args):
    proc = subprocess.run([exe] + args, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    text = (proc.stdout + proc.stderr).decode("utf-8", "replace")
    return proc.returncode, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--json")
    ap.add_argument("--require-rmapbt", action="store_true")
    ap.add_argument("--src", default="/src")
    ap.add_argument("--dst", default="/dst")
    ap.add_argument("--empty-dir", default="/emptydir")
    args = ap.parse_args()
    src, dst = args.src, args.dst
    empty = args.empty_dir

    image = os.path.abspath(args.image)
    exe = os.path.abspath(args.exe)
    original = digest(image)
    failures = []

    def check_read(name, argv, required_text=None):
        rc, text = run(exe, argv)
        if rc != 0:
            failures.append("%s failed (exit %d):\n%s" % (name, rc, text))
        if required_text and required_text not in text:
            failures.append("%s output lacks %r:\n%s" %
                            (name, required_text, text))
        after = digest(image)
        if after != original:
            failures.append("%s modified the image" % name)

    def check_refused(name, argv):
        rc, text = run(exe, argv)
        if rc == 0:
            failures.append("%s unexpectedly succeeded:\n%s" % (name, text))
        after = digest(image)
        if after != original:
            failures.append("%s modified the image on refusal:\n%s" %
                            (name, text))
        if "reflink" not in text.lower() and \
                "read-write" not in text.lower() and \
                "modifies the image" not in text.lower() and \
                "replays the journal" not in text.lower() and \
                "--rw-experimental" not in text:
            failures.append("%s failed for an unrelated reason:\n%s" %
                            (name, text))

    check_read("info", ["info", image], "reflink=yes")
    check_read("check", ["check", image])
    check_read("journal", ["journal", image, "--json"])
    check_read("ls", ["ls", image, "/"])
    check_read("stat", ["stat", image, src])
    check_read("bmap", ["bmap", image, src, "--json"])
    check_read("refcount", ["refcount", image], "refcount")
    rc, text = run(exe, ["cat", image, src])
    if rc != 0:
        failures.append("cat failed (exit %d):\n%s" % (rc, text))
    if digest(image) != original:
        failures.append("cat modified the image")
    if args.require_rmapbt:
        check_read("rmap", ["rmap", image], "AG 0")

    rw = "--rw-experimental"
    check_refused("create with rw", ["create", image, "/refused.bin", rw])
    check_refused("mkdir with rw", ["mkdir", image, "/refused-dir", rw])
    check_refused("pwrite with rw",
                  ["pwrite", image, src, "0", rw, "--text=X"])
    check_refused("fallocate with rw",
                  ["fallocate", image, src, "0", "4096", rw])
    check_refused("convert-unwritten with rw",
                  ["convert-unwritten", image, src, "0", "4096", rw])
    check_refused("extend with rw",
                  ["extend", image, src, "8192", rw])
    check_refused("truncate with rw",
                  ["truncate", image, src, "0", rw])
    check_refused("unlink with rw", ["unlink", image, dst, rw])
    check_refused("rmdir with rw", ["rmdir", image, empty, rw])
    check_refused("rename with rw",
                  ["rename", image, src, "/renamed", rw])
    check_refused("link with rw",
                  ["link", image, src, "/hard", rw])
    check_refused("symlink with rw",
                  ["symlink", image, "target", "/link", rw])
    check_refused("reflink with rw",
                  ["reflink", image, src, dst, "0", "0", "0", rw])
    check_refused("inner qualification without rw",
                  ["reflink", image, src, dst, "0", "0", "0",
                   "--rw-reflink-qualification"])
    check_refused("create without rw",
                  ["create", image, "/no-flag.bin"])
    check_refused("recover without confirmation", ["recover", image])

    result = {"image": image, "sha256": original, "result":
              "ok" if not failures else "failed",
              "failures": failures}
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)
    if failures:
        for failure in failures:
            print("rw6c refusal: FAIL: %s" % failure, file=sys.stderr)
        return 1
    print("rw6c refusal matrix: ok (image unchanged)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
