#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Checks a mounted FUZZ-2 image against its semantic manifest.

This is the Linux kernel oracle: the image must already be mounted (or the
caller must pass --mount to let this script mount it read-only).  Every path,
type, link count, size, logical content, symlink target, xattr and directory
listing is compared with the independent model manifest.  Hard-link identity is
checked through st_ino, never through an inode number stored in the manifest.

usage:
  stateful_mount_check.py --mount <dir> --manifest <manifest.json>
  stateful_mount_check.py --image <img> --manifest <manifest.json> [--mountpoint <dir>]
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile


def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % value


def fail(message):
    raise SystemExit("stateful mount check: FAIL: %s" % message)


def resolve(root, path):
    if path == "/":
        return root
    return os.path.join(root, path.lstrip("/"))


def check_state(root, state, range_mode, allowed_sizes=None):
    for listing_path, names in state["listings"].items():
        directory = resolve(root, listing_path)
        if not os.path.isdir(directory) or os.path.islink(directory):
            fail("%s is not a directory" % listing_path)
        actual = sorted(os.listdir(directory))
        if actual != sorted(names):
            fail("listing %s = %s != %s" %
                 (listing_path, actual, sorted(names)))
    groups = {}
    for entry in state["entries"]:
        resolved = []
        for path in entry["paths"]:
            full = resolve(root, path)
            try:
                st = os.lstat(full)
            except FileNotFoundError:
                fail("path %s is missing" % path)
            resolved.append(st.st_ino)
            if not range_mode or entry["type"] != "file":
                if st.st_nlink != entry["nlink"]:
                    fail("%s: nlink %d != %d" %
                         (path, st.st_nlink, entry["nlink"]))
            want_type = entry["type"]
            if want_type == "dir":
                if not os.path.isdir(full) or os.path.islink(full):
                    fail("%s: not a directory" % path)
                continue
            if want_type == "file":
                if not os.path.isfile(full) or os.path.islink(full):
                    fail("%s: not a regular file" % path)
                if range_mode:
                    allowed = allowed_sizes.get(path) if allowed_sizes else None
                    if allowed and st.st_size not in allowed:
                        fail("%s: size %d not in allowed set %s" %
                             (path, st.st_size, sorted(allowed)))
                    continue
                if st.st_size != entry["size"]:
                    fail("%s: size %d != %d" %
                         (path, st.st_size, entry["size"]))
                with open(full, "rb") as f:
                    data = f.read()
                if len(data) != entry["size"] or \
                        fnv1a(data) != entry["content_fnv"]:
                    fail("%s: content differs" % path)
            elif want_type == "symlink":
                if not os.path.islink(full):
                    fail("%s: not a symlink" % path)
                target = os.readlink(full).encode()
                if fnv1a(target) != entry["content_fnv"]:
                    fail("%s: symlink target hash differs" % path)
                if target.decode("utf-8", "surrogateescape") != \
                        entry.get("target", ""):
                    fail("%s: symlink target differs" % path)
            else:
                fail("%s: unknown type %s" % (path, want_type))
            actual_attrs = {}
            try:
                for name in os.listxattr(full, follow_symlinks=False):
                    if not name.startswith("user."):
                        continue
                    value = os.getxattr(full, name, follow_symlinks=False)
                    actual_attrs[name] = {
                        "length": len(value),
                        "value_fnv": fnv1a(value),
                    }
            except OSError as exc:
                fail("%s: cannot read xattrs: %s" % (path, exc))
            wanted_attrs = {a["name"]: a for a in entry.get("xattrs", [])}
            if set(actual_attrs) != set(wanted_attrs):
                fail("%s: xattr names %s != %s" %
                     (path, sorted(actual_attrs), sorted(wanted_attrs)))
            for name, wanted in wanted_attrs.items():
                got = actual_attrs[name]
                if got["length"] != wanted["length"] or \
                        got["value_fnv"] != wanted["value_fnv"]:
                    fail("%s: xattr %s differs" % (path, name))
        if len(set(resolved)) != 1:
            fail("hard-link paths %s resolve to different inodes %s" %
                 (entry["paths"], resolved))
        groups.setdefault(resolved[0], len(entry["paths"]))
    return len(state["entries"])


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--mount", help="already-mounted directory")
    parser.add_argument("--image", help="image to mount read-only")
    parser.add_argument("--mountpoint", help="temporary mount point")
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv[1:])

    own_mount = False
    mountpoint = args.mount
    if not mountpoint:
        if not args.image:
            parser.error("--mount or --image is required")
        mountpoint = args.mountpoint or tempfile.mkdtemp(prefix="fuzz2-mnt-")
        os.makedirs(mountpoint, exist_ok=True)
        command = ["mount", "-o", "loop,ro,nouuid", args.image, mountpoint]
        result = subprocess.run(command)
        if result.returncode != 0:
            fail("cannot mount %s" % args.image)
        own_mount = True

    with open(args.manifest, "r", encoding="utf-8-sig") as f:
        manifest = json.load(f)
    if manifest.get("format") != "xfswin-stateful-v1":
        fail("unknown manifest format")
    states = manifest.get("states", [])
    if not states:
        fail("manifest has no states")
    range_mode = manifest.get("state_mode") == "range"
    allowed_sizes = {}
    if range_mode:
        for state in states:
            for entry in state["entries"]:
                if entry["type"] == "file":
                    for path in entry["paths"]:
                        allowed_sizes.setdefault(path, set()).add(entry["size"])
    errors = []
    try:
        for index, state in enumerate(states):
            try:
                count = check_state(mountpoint, state, range_mode, allowed_sizes)
                if not args.quiet:
                    print("mounted state %d matches (%d entries)" %
                          (index, count))
                print("OK: FUZZ-2 mounted state verified")
                return 0
            except SystemExit as exc:
                errors.append(str(exc))
        fail("no mounted state matches:\n  " + "\n  ".join(errors))
    finally:
        if own_mount:
            subprocess.run(["umount", mountpoint], check=False)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except AssertionError as error:
        print("stateful mount check: FAIL: %s" % error, file=sys.stderr)
        sys.exit(1)
