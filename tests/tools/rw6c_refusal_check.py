#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-6C no-write refusal matrix.

Ordinary `--rw-experimental` now accepts reflink, so this matrix checks the
refusals that must remain: read-only commands never write, mutation without
`--rw-experimental` is refused, and a reflink image whose directory blocks span
more than one filesystem block is still outside the writable profile.  The
unsupported-geometry case uses a patched copy, and every refusal must leave
that copy byte-identical.

usage: rw6c_refusal_check.py --exe <xfswin> --image <reflink.img>
       [--require-rmapbt] [--json out]

By default the image must contain /src, /dst and an empty /emptydir; use
--src/--dst/--empty-dir to point at another fixture.
"""

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rw6b_rmap_probe as probe  # noqa: E402


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


def patch_dirblklog(path):
    """Make a copy that is structurally readable but outside the writer's
    directory-block geometry (dirblklog=2), with a valid superblock CRC."""
    with open(path, "r+b") as stream:
        prefix = stream.read(4096)
        if len(prefix) < 512:
            raise SystemExit("short superblock")
        sectsize = struct.unpack_from(">H", prefix, 102)[0]
        if sectsize < 512 or sectsize > len(prefix):
            raise SystemExit("unsupported sector size %d" % sectsize)
        sb = bytearray(prefix[:sectsize])
        sb[192] = 2  # sb_dirblklog
        struct.pack_into("<I", sb, 224, probe.xfs_crc(bytes(sb), 224))
        stream.seek(0)
        stream.write(sb)


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
    blocked = image + ".dirblk"
    shutil.copyfile(image, blocked)
    patch_dirblklog(blocked)
    blocked_original = digest(blocked)
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

    def check_refused(name, argv, target, baseline, needles):
        rc, text = run(exe, argv)
        if rc == 0:
            failures.append("%s unexpectedly succeeded:\n%s" % (name, text))
        after = digest(target)
        if after != baseline:
            failures.append("%s modified the image on refusal:\n%s" %
                            (name, text))
        low = text.lower()
        if not any(needle in low for needle in needles):
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

    # Still read-only without --rw-experimental, and the legacy qualification
    # switch must not be usable as a substitute for the public opt-in.
    check_refused("create without rw", ["create", image, "/no-flag.bin"],
                  image, original,
                  ("--rw-experimental", "modifies the image", "read-write"))
    check_refused("inner qualification without rw",
                  ["reflink", image, src, dst, "0", "0", "0",
                   "--rw-reflink-qualification"],
                  image, original,
                  ("--rw-experimental", "modifies the image", "read-write"))
    check_refused("recover without confirmation", ["recover", image],
                  image, original,
                  ("yes-i-really-mean-it", "confirm", "read-write"))

    # dirblklog != 0 remains outside the writable profile.  Every mutation
    # command must fail during the mount preflight, before the first write.
    rw = "--rw-experimental"
    geometry = ("directory block", "read-write", "not supported")
    for name, argv in (
            ("create on unsupported geometry",
             ["create", blocked, "/refused.bin", rw]),
            ("mkdir on unsupported geometry",
             ["mkdir", blocked, "/refused-dir", rw]),
            ("pwrite on unsupported geometry",
             ["pwrite", blocked, src, "0", rw, "--text=X"]),
            ("fallocate on unsupported geometry",
             ["fallocate", blocked, src, "0", "4096", rw]),
            ("convert-unwritten on unsupported geometry",
             ["convert-unwritten", blocked, src, "0", "4096", rw]),
            ("extend on unsupported geometry",
             ["extend", blocked, src, "8192", rw]),
            ("truncate on unsupported geometry",
             ["truncate", blocked, src, "0", rw]),
            ("unlink on unsupported geometry",
             ["unlink", blocked, dst, rw]),
            ("rmdir on unsupported geometry",
             ["rmdir", blocked, empty, rw]),
            ("rename on unsupported geometry",
             ["rename", blocked, src, "/renamed", rw]),
            ("link on unsupported geometry",
             ["link", blocked, src, "/hard", rw]),
            ("symlink on unsupported geometry",
             ["symlink", blocked, "target", "/link", rw]),
            ("reflink on unsupported geometry",
             ["reflink", blocked, src, dst, "0", "0", "0", rw])):
        check_refused(name, argv, blocked, blocked_original, geometry)

    os.unlink(blocked)
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
    print("rw6c refusal matrix: ok (ordinary gate open, unsupported geometry "
          "and read-only paths unchanged)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
