#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""RW-STABLE-1 fail-closed refusal matrix.

Each case copies a valid image, patches one superblock field into a
profile/geometry this writer does not support, repairs the superblock CRC and
runs a normal mutation with `--rw`.  The refusal must happen before the first
write, so the whole copy is byte-identical afterwards.

The supported counterpart is checked too: the unpatched copy accepts the same
mutation.  That separates "the writer refuses everything" from "the writer
refuses exactly the unsupported profile".

usage: rw_stable_refusal_matrix.py --exe <xfswin> --image <formatted.img>
       [--json out]
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
    return proc.returncode, (proc.stdout + proc.stderr).decode("utf-8",
                                                               "replace")


def patch(path, mutate):
    with open(path, "r+b") as stream:
        prefix = stream.read(4096)
        if len(prefix) < 512:
            raise SystemExit("short superblock")
        sectsize = struct.unpack_from(">H", prefix, 102)[0]
        if sectsize < 512 or sectsize > len(prefix):
            raise SystemExit("unsupported sector size %d" % sectsize)
        sb = bytearray(prefix[:sectsize])
        mutate(sb)
        struct.pack_into("<I", sb, 224, probe.xfs_crc(bytes(sb), 224))
        stream.seek(0)
        stream.write(sb)


def be16(sb, off, value):
    struct.pack_into(">H", sb, off, value)


def be32(sb, off, value):
    struct.pack_into(">I", sb, off, value)


def be64(sb, off, value):
    struct.pack_into(">Q", sb, off, value)


def or_be32(sb, off, value):
    current = struct.unpack_from(">I", sb, off)[0]
    be32(sb, off, current | value)


def case_set_incompat(bit):
    return lambda sb: or_be32(sb, 216, bit)


def case_set_rocompat(bit):
    return lambda sb: or_be32(sb, 212, bit)


def case_set_log_incompat(bit):
    return lambda sb: or_be32(sb, 220, bit)


def case_clear_ftype(sb):
    features2 = struct.unpack_from(">I", sb, 200)[0]
    be32(sb, 200, features2 & ~0x00000200)
    incompat = struct.unpack_from(">I", sb, 216)[0]
    be32(sb, 216, incompat & ~0x00000001)


CASES = [
    ("metadir", "metadir", case_set_incompat(1 << 8)),
    ("zoned", "zoned", case_set_incompat(1 << 9)),
    ("zone-gaps", "zoned", case_set_incompat(1 << 10)),
    ("needs-repair", "needsrepair", case_set_incompat(1 << 4)),
    ("unknown-incompat", "unknown incompatible", case_set_incompat(1 << 31)),
    ("unknown-ro-compat", "read-only compatible feature",
     case_set_rocompat(1 << 31)),
    ("unknown-log-incompat", "log-incompatible",
     case_set_log_incompat(1 << 1)),
    ("quota-flags", "quota", lambda sb: be16(sb, 176, 1)),
    ("quota-inode", "quota", lambda sb: be64(sb, 160, 70)),
    ("realtime", "realtime",
     lambda sb: (be64(sb, 16, 1024), be64(sb, 24, 8))),
    ("asciici", "asciici",
     lambda sb: be16(sb, 100,
                     struct.unpack_from(">H", sb, 100)[0] | 0x4000)),
    ("ftype-off", "ftype", case_clear_ftype),
    ("sector-4k", "sector", lambda sb: be16(sb, 102, 4096)),
    ("log-stripe", "log stripe", lambda sb: be32(sb, 196, 4096)),
    ("dirblklog", "directory block", lambda sb: sb.__setitem__(192, 1)),
    ("external-log", "external log", lambda sb: be64(sb, 48, 0)),
    ("inopblock", "inodes-per-block", lambda sb: be16(sb, 106, 128)),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    source = os.path.abspath(args.image)
    source_digest = digest(source)
    work = source + ".refusal-matrix"
    failures = []
    results = []

    # Control: the unpatched copy must accept the mutation.  Otherwise the
    # matrix would silently prove only that a broken fixture refuses writes.
    shutil.copyfile(source, work)
    rc, text = run(exe, ["create", work, "/control.bin", "--rw"])
    if rc != 0:
        failures.append("control mutation failed (exit %d):\n%s" % (rc, text))
    results.append({"case": "control", "exit": rc, "changed": False})
    os.unlink(work)

    # sb_logsunit 0 and 1 both encode "no log stripe unit": Linux mkfs writes 1
    # for a v2 log, 0 is the legacy encoding, and the kernel only treats values
    # above 1 as a real stripe unit.  Both encodings must stay writable; a real
    # stripe unit (4096) is refused below.
    for value in (0, 1):
        shutil.copyfile(source, work)
        patch(work, lambda sb, v=value: be32(sb, 196, v))
        rc, text = run(exe, ["create", work, "/no-stripe.bin", "--rw"])
        if rc != 0:
            failures.append("logsunit=%d unexpectedly refused:\n%s" %
                            (value, text))
        results.append({"case": "logsunit-none-%d" % value, "exit": rc,
                        "changed": True})
        os.unlink(work)

    for name, needle, mutate in CASES:
        shutil.copyfile(source, work)
        patch(work, mutate)
        before = digest(work)
        rc, text = run(exe, ["create", work, "/refused.bin", "--rw"])
        after = digest(work)
        changed = after != before
        low = text.lower()
        if rc == 0:
            failures.append("%s unexpectedly accepted the mutation:\n%s" %
                            (name, text))
        if changed:
            failures.append("%s changed the image on refusal: %s -> %s" %
                            (name, before, after))
        if needle and needle.lower() not in low:
            failures.append("%s refusal does not mention %r:\n%s" %
                            (name, needle, text))
        results.append({"case": name, "exit": rc, "changed": changed,
                        "digest": before})
        os.unlink(work)

    # The original image is read-only input for this tool; prove it was not
    # touched either.
    if digest(source) != source_digest:
        failures.append("the source image changed")

    result = {"image": source, "sha256": source_digest,
              "result": "ok" if not failures else "failed",
              "cases": results, "failures": failures}
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)
    if failures:
        for failure in failures:
            print("rw-stable refusal: FAIL: %s" % failure, file=sys.stderr)
        return 1
    print("rw-stable refusal matrix: ok (%d unsupported profiles refused "
          "without a write; control mutation and both no-stripe-unit "
          "encodings accepted)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
