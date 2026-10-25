#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Verify a mounted XFS image against the Linux fixture manifest and the
Windows mutation expectation file.

Runs on the Linux verifier only.  It reads the mounted tree with ordinary
filesystem calls; it never invokes xfswin, so a bug in the Windows reader
cannot make the check pass.

usage:
  rw6b_verify_manifest.py <mount-root> <MANIFEST.sha256> [expected.json]
"""

import hashlib
import json
import os
import sys


def fail(message):
    raise SystemExit(message)


def check_original(root, manifest_path):
    checked = 0
    with open(manifest_path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split(" ")
            if parts[0] == "link":
                rel = parts[1]
                target = " ".join(parts[2:])
                path = os.path.join(root, rel)
                if not os.path.islink(path):
                    fail("%s is no longer a symlink" % rel)
                if os.readlink(path) != target:
                    fail("%s symlink target changed" % rel)
                checked += 1
                continue
            if parts[0] != "file":
                fail("bad manifest line: %s" % line)
            rel, size, mtime, digest = parts[1], int(parts[2]), int(parts[3]), parts[4]
            path = os.path.join(root, rel)
            if not os.path.isfile(path):
                fail("%s disappeared" % rel)
            st = os.stat(path)
            if st.st_size != size:
                fail("%s size %d != %d" % (rel, st.st_size, size))
            if int(st.st_mtime) != mtime:
                fail("%s mtime %d != %d" % (rel, int(st.st_mtime), mtime))
            with open(path, "rb") as handle:
                actual = hashlib.sha256(handle.read()).hexdigest()
            if actual != digest:
                fail("%s content changed" % rel)
            checked += 1
    return checked


def check_expected(root, expected_path):
    with open(expected_path, encoding="utf-8") as f:
        data = json.load(f)
    checked = 0
    for item in data["entries"]:
        rel = item["path"].lstrip("/")
        path = os.path.join(root, rel)
        kind = item["type"]
        if kind == "dir":
            if not os.path.isdir(path):
                fail("%s is not a directory" % rel)
        elif kind == "symlink":
            if not os.path.islink(path):
                fail("%s is not a symlink" % rel)
            if os.readlink(path) != item["target"]:
                fail("%s symlink target %r != %r" %
                     (rel, os.readlink(path), item["target"]))
            if item.get("nlink", 1) != os.lstat(path).st_nlink:
                fail("%s symlink nlink mismatch" % rel)
        elif kind == "file":
            if not os.path.isfile(path):
                fail("%s is not a regular file" % rel)
            st = os.stat(path)
            if st.st_size != item["size"]:
                fail("%s size %d != %d" % (rel, st.st_size, item["size"]))
            if st.st_nlink != item.get("nlink", 1):
                fail("%s nlink %d != %d" %
                     (rel, st.st_nlink, item.get("nlink", 1)))
            with open(path, "rb") as handle:
                actual = hashlib.sha256(handle.read()).hexdigest()
            if actual != item["sha256"]:
                fail("%s content hash mismatch" % rel)
        else:
            fail("unknown expected entry type %r" % kind)
        checked += 1
    return checked


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 2
    root, manifest = sys.argv[1], sys.argv[2]
    original = check_original(root, manifest)
    expected = 0
    if len(sys.argv) == 4:
        expected = check_expected(root, sys.argv[3])
    print("manifest OK: %d original entries, %d Windows-mutated entries" %
          (original, expected))
    return 0


if __name__ == "__main__":
    sys.exit(main())
