#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Linux-side RW-6D fixture content, verification and reverse mutation.

Used by tests/scripts/make_rw6d_linux.sh,
verify_rw6d_linux.sh and verify_rw6d_linux_reverse.sh.  It only uses the
kernel xattr syscalls; it never parses on-disk metadata.
"""

import argparse
import errno
import hashlib
import json
import os
import sys

from rw6d_common import (DEEP_ATTRS, deep_name, deep_value, pattern, sha256)


def fail(message):
    raise SystemExit("rw6d linux content: " + message)


def write_file(path, data):
    with open(path, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def setx(path, name, value, follow=False):
    try:
        os.setxattr(path, name, value, follow_symlinks=follow)
    except OSError as error:
        fail("setxattr(%s, %s): %s" % (path, name, error))


def getx(path, name):
    try:
        return os.getxattr(path, name, follow_symlinks=False)
    except OSError as error:
        fail("getxattr(%s, %s): %s" % (path, name, error))


def listx(path):
    return os.listxattr(path, follow_symlinks=False)


def describe_attributes(path, known_names=None):
    attr_names = known_names if known_names is not None else listx(path)
    attrs = []
    for name in sorted(attr_names):
        value = getx(path, name)
        attrs.append({
            "name": name,
            "length": len(value),
            "sha256": sha256(value),
        })
    return attrs


def is_file(path):
    return os.path.isfile(path) and not os.path.islink(path)


def build_manifest(root, paths, deleted=None, include_files=True,
                   kind="rw6d-linux", known_names=None):
    known_names = known_names or {}
    inodes = []
    files = {}
    for path, ino in sorted(paths.items()):
        full = os.path.join(root, path.lstrip("/"))
        if not os.path.lexists(full):
            continue
        inodes.append({
            "ino": int(ino),
            "path": path,
            "attributes": describe_attributes(full,
                                             known_names.get(path)),
        })
        if include_files and is_file(full):
            size = os.path.getsize(full)
            if size <= (4 << 20):
                data = read_file(full)
                files[path.lstrip("/")] = {
                    "size": size,
                    "sha256": sha256(data),
                }
    manifest = {"kind": kind, "inodes": inodes, "deleted": deleted or []}
    if files:
        manifest["files"] = files
    return manifest


def create(root):
    block = 4096
    for name in ("short", "leaf", "node", "remote", "data", "filler", "bmbt",
                 "del", "rename", "linkme"):
        write_file(os.path.join(root, name), pattern(block, len(name)))
    short = os.path.join(root, "short")
    write_file(short, pattern(4096, 1))
    setx(short, "user.alpha", b"hello")
    setx(short, "user.beta", pattern(23, 0x0101))
    setx(short, "trusted.owner", b"trust")

    leaf = os.path.join(root, "leaf")
    write_file(leaf, pattern(4096, 2))
    for i in range(20):
        setx(leaf, "user.l%02d" % i, pattern(100, 0x2000 + i))

    node = os.path.join(root, "node")
    write_file(node, pattern(4096, 3))
    for i in range(300):
        setx(node, "user.n%03d" % i, pattern(100, 0x3000 + i))

    remote = os.path.join(root, "remote")
    write_file(remote, pattern(4096, 4))
    setx(remote, "user.local", pattern(31, 0x7100))
    setx(remote, "user.remote", pattern(6000, 0x6d02))
    setx(remote, "user.remote2", pattern(20000, 0x6d03))

    # 100 separated data extents make Linux convert the data fork to a btree
    # when the attribute fork appears.
    data = os.path.join(root, "data")
    fd = os.open(data, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        for i in range(100):
            os.pwrite(fd, pattern(4096, 0x5000 + i), i * 2 * 4096)
        os.fsync(fd)
    finally:
        os.close(fd)
    setx(data, "user.keep", pattern(200, 0xd00d))

    # A fragmented attribute fork: a filler data block between leaf
    # allocations pushes the attribute extent map into a real BMBT.
    bmbt = os.path.join(root, "bmbt")
    filler = os.path.join(root, "filler")
    write_file(bmbt, pattern(4096, 6))
    ffd = os.open(filler, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        for i in range(80):
            if i >= 8:
                os.pwrite(ffd, pattern(512, 0x6600 + i),
                          (i - 8) * 2 * 4096)
                os.fsync(ffd)
            setx(bmbt, "user.b%03d" % i, pattern(180, 0xe000 + i))
        os.fsync(ffd)
    finally:
        os.close(ffd)

    # A level-2 DA node tree: 240-byte names, 15 entries per 4 KiB leaf, and
    # more than 504 leaves.  The filler write forces non-contiguous leaves so
    # the attribute fork also needs a BMBT.
    deep = os.path.join(root, "deep")
    write_file(deep, pattern(4096, 7))
    deep_filler = os.path.join(root, "deep-filler")
    dfd = os.open(deep_filler, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        for i in range(DEEP_ATTRS):
            setx(deep, deep_name(i), deep_value(i))
            if i % 15 == 14:
                os.pwrite(dfd, pattern(4096, 0x7700 + i), (i // 15) * 4096)
                os.fsync(dfd)
        os.fsync(dfd)
    finally:
        os.close(dfd)

    os.mkdir(os.path.join(root, "dir"))
    directory = os.path.join(root, "dir")
    setx(directory, "user.dir", pattern(80, 0x3300))
    setx(directory, "trusted.dir", pattern(40, 0x3301))

    os.symlink("target", os.path.join(root, "link"))
    setx(os.path.join(root, "link"), "trusted.link", pattern(32, 0x4401),
         follow=False)

    delete = os.path.join(root, "del")
    write_file(delete, pattern(2048, 8))
    for i in range(30):
        setx(delete, "user.d%02d" % i, pattern(300, 0xf000 + i))

    rename = os.path.join(root, "rename")
    write_file(rename, pattern(1024, 9))
    setx(rename, "user.r0", pattern(50, 0x8800))
    setx(rename, "user.r1", pattern(50, 0x8801))

    linkme = os.path.join(root, "linkme")
    write_file(linkme, pattern(1024, 10))
    setx(linkme, "user.h0", pattern(50, 0x9900))

    os.sync()


def create_paths(root):
    names = ["short", "leaf", "node", "remote", "data", "bmbt", "deep",
             "del", "rename", "linkme", "dir", "link"]
    return {"/" + name: os.stat(os.path.join(root, name),
                                follow_symlinks=False).st_ino
            for name in names}


def verify(root, manifest_path):
    manifest = json.load(open(manifest_path, encoding="utf-8"))
    checked = 0
    for item in manifest.get("inodes", []):
        path = os.path.join(root, item["path"].lstrip("/"))
        if not os.path.lexists(path):
            fail("%s is missing" % item["path"])
        expected = {a["name"]: a for a in item.get("attributes", [])}
        symlink = os.path.islink(path)
        try:
            listed = listx(path)
        except OSError as error:
            if error.errno != errno.E2BIG:
                raise
            # A user-space listxattr buffer is capped at XATTR_LIST_MAX
            # (64 KiB); the deep fixture names exceed that.  The raw oracle
            # still proves there are no unexpected attributes; here every
            # expected name is fetched individually.
            listed = None
        if listed is not None:
            visible = {name for name in listed
                       if name.startswith("trusted.") or
                       (name.startswith("user.") and not symlink)}
            want_visible = {name for name in expected
                            if name.startswith("trusted.") or
                            (name.startswith("user.") and not symlink)}
            if visible != want_visible:
                fail("%s attributes %s != %s" %
                     (item["path"], sorted(visible), sorted(want_visible)))
        for name, want in expected.items():
            # The kernel owns the security.*/system.* encodings; the
            # independent raw oracle still verifies their stored bytes.
            # user.* on a symlink is stored by XFS but VFS getxattr returns
            # ENODATA (only regular files/directories/sockets allow user.*).
            if not (name.startswith("trusted.") or
                    (name.startswith("user.") and not symlink)):
                continue
            value = getx(path, name)
            if "value_hex" in want:
                expected_value = bytes.fromhex(want["value_hex"])
                if value != expected_value:
                    fail("%s %s value differs" % (item["path"], name))
            elif len(value) != want["length"] or \
                    sha256(value) != want["sha256"]:
                fail("%s %s value differs" % (item["path"], name))
            checked += 1
    for rel in manifest.get("deleted", []):
        if os.path.lexists(os.path.join(root, rel)):
            fail("%s should have been deleted" % rel)
    for rel, want in manifest.get("files", {}).items():
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            fail("%s is missing" % rel)
        data = read_file(path)
        if len(data) != want["size"] or sha256(data) != want["sha256"]:
            fail("%s content differs" % rel)
    print("kernel xattr/content check OK: %d attributes, %d files" %
          (checked, len(manifest.get("files", {}))))


def reverse(root, manifest_path, out_path):
    manifest = json.load(open(manifest_path, encoding="utf-8"))
    paths = {item["path"]: int(item["ino"])
             for item in manifest.get("inodes", [])}
    deleted = []

    short = os.path.join(root, "short")
    setx(short, "user.alpha", pattern(5000, 0xa101))
    setx(short, "user.linux", pattern(64, 0xa102))
    try:
        os.removexattr(short, "user.beta")
    except OSError:
        pass

    leaf = os.path.join(root, "leaf")
    setx(leaf, "user.l99", pattern(100, 0xa200))
    for name in ("user.l00", "user.l01"):
        try:
            os.removexattr(leaf, name)
        except OSError:
            pass

    node = os.path.join(root, "node")
    setx(node, "user.linux-node", pattern(100, 0xa300))
    for name in ("user.n000", "user.n001"):
        try:
            os.removexattr(node, name)
        except OSError:
            pass

    deep = os.path.join(root, "deep")
    setx(deep, "user.linux-deep", pattern(300, 0xa400))
    try:
        os.removexattr(deep, deep_name(4000))
    except OSError:
        pass

    remote = os.path.join(root, "remote")
    setx(remote, "user.remote", pattern(12000, 0xa500))
    setx(remote, "user.linux-remote", pattern(7777, 0xa501))
    try:
        os.removexattr(remote, "user.local")
    except OSError:
        pass

    directory = os.path.join(root, "dir")
    setx(directory, "user.linux-dir", pattern(90, 0xa600))

    link = os.path.join(root, "link")
    setx(link, "trusted.linux-link", pattern(48, 0xa700), follow=False)

    old_rename = next((p for p in ("/rename", "/renamed-win",
                                   "/renamed-linux", "/renamed")
                       if os.path.lexists(os.path.join(root,
                                                       p.lstrip("/")))),
                      None)
    if old_rename is None:
        fail("no rename source is present")
    new_rename = os.path.join(root, "renamed-linux")
    if old_rename != "/renamed-linux":
        os.rename(os.path.join(root, old_rename.lstrip("/")), new_rename)
    setx(new_rename, "user.linux-renamed", pattern(70, 0xa800))

    linkme = os.path.join(root, "linkme")
    os.link(linkme, os.path.join(root, "linkme2"))
    setx(linkme, "user.linux-linkme", pattern(60, 0xa900))

    data = os.path.join(root, "data")
    with open(data, "r+b") as f:
        f.seek(0)
        f.write(pattern(4096, 0xaa00))
        f.flush()
        os.fsync(f.fileno())

    delete = os.path.join(root, "del")
    if os.path.lexists(delete):
        os.unlink(delete)
    deleted.append("/del")

    created = os.path.join(root, "linux-new")
    write_file(created, pattern(4096, 0xab00))
    for i in range(60):
        setx(created, "user.c%02d" % i, pattern(100, 0xac00 + i))

    os.sync()

    paths["/renamed-linux"] = os.stat(new_rename).st_ino
    for name in ("/rename", "/renamed-win", "/renamed-linux", "/renamed"):
        if name != "/renamed-linux":
            paths.pop(name, None)
    paths.pop("/del", None)
    paths["/linux-new"] = os.stat(created).st_ino
    # /linkme2 is a second name of the /linkme inode; one path is enough.
    known = {}
    deep_item = next((item for item in manifest.get("inodes", [])
                      if item.get("path") == "/deep"), None)
    if deep_item is not None:
        deep_names = {a["name"] for a in deep_item.get("attributes", [])}
        deep_names.discard(deep_name(4000))
        deep_names.add("user.linux-deep")
        known["/deep"] = deep_names
    final = build_manifest(root, paths, deleted=deleted,
                           kind="rw6d-linux-reverse",
                           known_names=known)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(final, f, indent=2, sort_keys=True)
        f.write("\n")
    print("Linux reverse mutation wrote %s (%d inodes)" %
          (out_path, len(final["inodes"])))


def main(argv):
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("create")
    p.add_argument("root")
    p = sub.add_parser("create-manifest")
    p.add_argument("root")
    p.add_argument("out")
    p = sub.add_parser("verify")
    p.add_argument("root")
    p.add_argument("manifest")
    p = sub.add_parser("reverse")
    p.add_argument("root")
    p.add_argument("manifest")
    p.add_argument("out")
    args = parser.parse_args(argv)
    if args.command == "create":
        create(args.root)
    elif args.command == "create-manifest":
        known = {"/deep": [deep_name(i) for i in range(DEEP_ATTRS)]}
        manifest = build_manifest(args.root, create_paths(args.root),
                                  known_names=known)
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2, sort_keys=True)
            f.write("\n")
        print("Linux fixture manifest: %d inodes" % len(manifest["inodes"]))
    elif args.command == "verify":
        verify(args.root, args.manifest)
    elif args.command == "reverse":
        reverse(args.root, args.manifest, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
