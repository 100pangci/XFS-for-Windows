#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Content expectations for the RW-4 round trip.

Two roles:

  make-payload <size> <seed> <path>
      (Windows side) writes the deterministic payload pattern that the Linux
      side reconstructs; the manifest carries (size, seed), never the bytes.

  check-mount <mount-point> <manifest.json> <image-name>
      (Linux side) verifies, through the mounted filesystem, everything the
      manifest expects from that image: exact listings, file sizes and
      contents, directory and symlink types, symlink targets, absent names,
      hard-link identity (two names share one inode with the expected link
      count). This runs after the kernel replayed the journal, so it doubles
      as the post-replay state check.

Payload pattern (reproduced independently on both sides):
  byte(i) = ((i * 181) ^ (i >> 5) ^ seed) & 0xFF
"""

import argparse
import json
import os
import stat
import sys


def payload(size, seed):
    return bytes(((i * 181) ^ (i >> 5) ^ seed) & 0xFF for i in range(size))


def write_payload(args):
    data = payload(args.size, args.seed)
    with open(args.path, "wb") as f:
        f.write(data)
    print(f"wrote {len(data)} pattern bytes (seed {args.seed}) to {args.path}")
    return 0


def check_listing(mount, rel, expected, failures):
    path = os.path.join(mount, rel) if rel else mount
    try:
        got = sorted(os.listdir(path))
    except OSError as error:
        failures.append(f"cannot list {rel or '/'}: {error}")
        return
    want = sorted(expected)
    if got != want:
        missing = sorted(set(want) - set(got))
        extra = sorted(set(got) - set(want))
        failures.append(
            f"listing of {rel or '/'} differs: missing {missing[:10]}, "
            f"unexpected {extra[:10]}")


def main(argv):
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    make = sub.add_parser("make-payload")
    make.add_argument("size", type=int)
    make.add_argument("seed", type=int)
    make.add_argument("path")

    check = sub.add_parser("check-mount")
    check.add_argument("mount")
    check.add_argument("manifest")
    check.add_argument("image")

    args = parser.parse_args(argv[1:])
    if args.command == "make-payload":
        return write_payload(args)

    with open(args.manifest, "r", encoding="utf-8-sig") as f:
        manifest = json.load(f)
    spec = manifest["images"].get(args.image)
    if spec is None:
        print(f"FAILED: no manifest entry for {args.image}", file=sys.stderr)
        return 1

    failures = []
    mount = args.mount
    for rel in spec.get("dirs", []):
        path = os.path.join(mount, rel)
        if not os.path.isdir(path):
            failures.append(f"{rel} is not a directory")
    for entry in spec.get("files", []):
        path = os.path.join(mount, entry["path"])
        try:
            st = os.stat(path)
        except OSError as error:
            failures.append(f"{entry['path']}: {error}")
            continue
        if not stat.S_ISREG(st.st_mode):
            failures.append(f"{entry['path']} is not a regular file")
            continue
        if st.st_size != entry["size"]:
            failures.append(
                f"{entry['path']}: size {st.st_size} != {entry['size']}")
            continue
        if "seed" in entry:
            with open(path, "rb") as f:
                data = f.read()
            if data != payload(entry["size"], entry["seed"]):
                failures.append(f"{entry['path']}: content differs from the "
                                f"pattern (seed {entry['seed']})")
    for entry in spec.get("symlinks", []):
        path = os.path.join(mount, entry["path"])
        try:
            st = os.lstat(path)
        except OSError as error:
            failures.append(f"{entry['path']}: {error}")
            continue
        if not stat.S_ISLNK(st.st_mode):
            failures.append(f"{entry['path']} is not a symlink")
            continue
        target = os.readlink(path)
        if target != entry["target"]:
            failures.append(
                f"{entry['path']}: target {target!r} != {entry['target']!r}")
    for rel in spec.get("absent", []):
        if os.path.lexists(os.path.join(mount, rel)):
            failures.append(f"{rel} must not exist")
    for rel, names in spec.get("listings", {}).items():
        check_listing(mount, rel, names, failures)
    if "root_listing" in spec:
        check_listing(mount, "", spec["root_listing"], failures)
    for group in spec.get("same_inode", []):
        inos = []
        for rel in group["paths"]:
            path = os.path.join(mount, rel)
            try:
                inos.append(os.lstat(path).st_ino)
            except OSError as error:
                failures.append(f"{rel}: {error}")
        if len(set(inos)) > 1:
            failures.append(f"{group['paths']} do not share an inode "
                            f"(inodes {inos})")
        if "nlink" in group:
            for rel in group["paths"]:
                path = os.path.join(mount, rel)
                try:
                    st = os.lstat(path)
                except OSError:
                    continue
                if st.st_nlink != group["nlink"]:
                    failures.append(f"{rel}: nlink {st.st_nlink} != "
                                    f"{group['nlink']}")

    if failures:
        for failure in failures:
            print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"{args.image}: mounted state matches the manifest")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
