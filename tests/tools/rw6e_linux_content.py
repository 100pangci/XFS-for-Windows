#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Deterministic Linux parent-pointer workload and verifier.

Run with the XFS image mounted at <root>:

  rw6e_linux_content.py --make   <mountpoint> --manifest out.json
  rw6e_linux_content.py --verify <mountpoint> --manifest out.json

The workload creates nested directories, regular files, hard links (including
1000 links to one inode and same-name links under different parents), inline
and remote symlinks, Unicode and maximum-length names, user/trusted/security
xattrs, an optional POSIX ACL, a reflink clone and a CoW write.  It is designed
to exercise the Linux kernel's own parent-pointer updates, not the Windows
writer.
"""

import argparse
import hashlib
import json
import os
import stat
import struct
import subprocess
import sys

FICLONE = 0x40049409
REMOTE_ATTR = bytes((i * 37 + 11) & 0xFF for i in range(4000))
FILE_PATTERN = bytes((i * 13 + 7) & 0xFF for i in range(131072))
UNICODE = "日本語-α-ß"


def fail(message):
    raise SystemExit("rw6e linux content: " + message)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            block = f.read(1 << 20)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def set_attr(path, name, value, required=False):
    try:
        os.setxattr(path, name, value, follow_symlinks=False)
    except OSError as exc:
        if required:
            fail("setxattr %s on %s failed: %s" % (name, path, exc))
        return False
    return True


def clone(src, dst):
    with open(src, "rb") as s, open(dst, "wb") as d:
        if hasattr(os, "ioctl") and hasattr(os, "FICLONE"):
            import fcntl
            fcntl.ioctl(d.fileno(), FICLONE, s.fileno())
        else:
            import fcntl
            fcntl.ioctl(d.fileno(), FICLONE, s.fileno())


def make(root, manifest_path):
    if not os.path.isdir(root):
        fail("mountpoint does not exist: " + root)
    for name in os.listdir(root):
        path = os.path.join(root, name)
        if os.path.isdir(path) and not os.path.islink(path):
            subprocess.run(["rm", "-rf", path], check=True)
        else:
            os.unlink(path)

    for d in ("d1", "d2", "d3", "links", "d1/sub"):
        os.makedirs(os.path.join(root, d))

    base = os.path.join(root, "d1", "f")
    with open(base, "wb") as f:
        f.write(FILE_PATTERN)
    os.chmod(base, 0o644)

    with open(os.path.join(root, "d1", "empty"), "wb"):
        pass
    with open(os.path.join(root, "d1", "zero-filled"), "wb") as f:
        f.write(b"\0" * 10000)

    # 1000 hard links to one inode, plus a same-name link under a different
    # parent.  This is the attr-tree/BMBT stress part.
    for i in range(1000):
        os.link(base, os.path.join(root, "links", "h%04d" % i))
    os.link(base, os.path.join(root, "d2", "same-name"))
    os.link(base, os.path.join(root, "d3", "same-name"))

    # Namespace moves and replacement.
    os.rename(os.path.join(root, "d1", "empty"),
              os.path.join(root, "d2", "empty-moved"))
    with open(os.path.join(root, "d2", "victim"), "wb") as f:
        f.write(b"victim")
    os.replace(os.path.join(root, "d1", "zero-filled"),
               os.path.join(root, "d2", "victim"))

    # Inline and remote symlinks.
    os.symlink("d1/f", os.path.join(root, "sym-inline"))
    os.symlink("x" * 400, os.path.join(root, "sym-remote"))

    unicode_path = os.path.join(root, "d3", UNICODE)
    with open(unicode_path, "wb") as f:
        f.write(b"unicode")
    long_name = "L" * 200
    with open(os.path.join(root, "d3", long_name), "wb") as f:
        f.write(b"long")

    # Attributes: local, remote, trusted and security where the host policy
    # permits.  A missing security/trusted namespace is recorded in the
    # manifest instead of being silently ignored as evidence.
    attrs = {}
    attrs["user.alpha"] = set_attr(unicode_path, "user.alpha", b"hello",
                                   required=True)
    attrs["user.remote"] = set_attr(unicode_path, "user.remote", REMOTE_ATTR,
                                    required=True)
    attrs["trusted.owner"] = set_attr(unicode_path, "trusted.owner", b"trust",
                                      required=False)
    attrs["security.selinux"] = set_attr(
        unicode_path, "security.selinux", b"system_u:object_r:unlabeled_t:s0",
        required=False)

    acl = False
    if subprocess.run(["sh", "-c", "command -v setfacl >/dev/null"],
                      check=False).returncode == 0:
        acl = subprocess.run(
            ["setfacl", "-m", "u:12345:rw", unicode_path],
            check=False).returncode == 0

    # Reflink clone + CoW write.  FICLONE must work; otherwise this is not an
    # XFS reflink filesystem and the fixture is wrong.
    clone_src = os.path.join(root, "d1", "reflink-src")
    clone_dst = os.path.join(root, "d3", "reflink-dst")
    with open(clone_src, "wb") as f:
        f.write(FILE_PATTERN)
    clone(clone_src, clone_dst)
    with open(clone_src, "r+b") as f:
        f.seek(4096)
        f.write(b"COW" * 100)

    entries = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()
        rel_dir = os.path.relpath(dirpath, root)
        for name in filenames:
            path = os.path.join(dirpath, name)
            rel = os.path.normpath(os.path.join(rel_dir, name))
            st = os.lstat(path)
            item = {
                "type": ("symlink" if stat.S_ISLNK(st.st_mode) else
                         "file"),
                "ino": st.st_ino,
                "nlink": st.st_nlink,
                "size": st.st_size,
            }
            if stat.S_ISLNK(st.st_mode):
                item["target"] = os.readlink(path)
            elif stat.S_ISREG(st.st_mode):
                item["sha256"] = sha256(path)
            entries[rel] = item
    dirs = {}
    for dirpath, dirnames, _ in os.walk(root):
        dirnames.sort()
        rel_dir = os.path.relpath(dirpath, root)
        for name in dirnames:
            path = os.path.join(dirpath, name)
            rel = os.path.normpath(os.path.join(rel_dir, name))
            st = os.lstat(path)
            dirs[rel] = {"ino": st.st_ino, "nlink": st.st_nlink}

    manifest = {
        "kind": "rw6e-linux-parent-workload",
        "root": root,
        "entries": entries,
        "directories": dirs,
        "attrs": attrs,
        "acl": acl,
        "hardlink_inode": os.stat(base).st_ino,
        "hardlink_count": os.stat(base).st_nlink,
        "reflink_src": os.path.relpath(clone_src, root),
        "reflink_dst": os.path.relpath(clone_dst, root),
    }
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")
    print("rw6e: created %d entries, %d directories, hardlink nlink=%d" %
          (len(entries), len(dirs), manifest["hardlink_count"]))


def verify(root, manifest_path):
    with open(manifest_path, encoding="utf-8") as f:
        manifest = json.load(f)
    if manifest.get("kind") != "rw6e-linux-parent-workload":
        fail("unexpected manifest kind")
    errors = []
    for rel, want in manifest["entries"].items():
        path = os.path.join(root, rel)
        try:
            st = os.lstat(path)
        except FileNotFoundError:
            errors.append("missing " + rel)
            continue
        if want["type"] == "symlink":
            if not stat.S_ISLNK(st.st_mode):
                errors.append("not a symlink: " + rel)
            elif os.readlink(path) != want["target"]:
                errors.append("wrong symlink target: " + rel)
        else:
            if not stat.S_ISREG(st.st_mode):
                errors.append("not a regular file: " + rel)
            elif st.st_size != want["size"]:
                errors.append("wrong size: " + rel)
            elif sha256(path) != want["sha256"]:
                errors.append("wrong content: " + rel)
        if st.st_ino != want["ino"]:
            errors.append("inode changed: " + rel)
        if st.st_nlink != want["nlink"]:
            errors.append("nlink changed: %s (%d != %d)" %
                          (rel, st.st_nlink, want["nlink"]))
    for rel, want in manifest["directories"].items():
        path = os.path.join(root, rel)
        try:
            st = os.lstat(path)
        except FileNotFoundError:
            errors.append("missing directory " + rel)
            continue
        if not stat.S_ISDIR(st.st_mode):
            errors.append("not a directory: " + rel)
        if st.st_ino != want["ino"]:
            errors.append("directory inode changed: " + rel)
        if st.st_nlink != want["nlink"]:
            errors.append("directory nlink changed: " + rel)
    # Symlinks that were added from outside the manifest after the Windows
    # mutation are allowed only if the caller records them; this verifier is
    # strict for the fixture it created.
    for name in ("sym-inline", "sym-remote"):
        path = os.path.join(root, name)
        if not os.path.islink(path):
            errors.append("missing symlink " + name)
    unicode_path = os.path.join(root, "d3", UNICODE)
    for name, value in (("user.alpha", b"hello"),
                        ("user.remote", REMOTE_ATTR)):
        try:
            actual = os.getxattr(unicode_path, name,
                                 follow_symlinks=False)
        except OSError as exc:
            errors.append("missing xattr %s: %s" % (name, exc))
        else:
            if actual != value:
                errors.append("wrong xattr value: " + name)
    if errors:
        for error in errors[:20]:
            print("  " + error, file=sys.stderr)
        fail("%d verification errors" % len(errors))
    print("rw6e: verified %d entries and %d directories" %
          (len(manifest["entries"]), len(manifest["directories"])))


def main(argv):
    parser = argparse.ArgumentParser()
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--make", action="store_true")
    group.add_argument("--verify", action="store_true")
    parser.add_argument("root")
    parser.add_argument("--manifest", required=True)
    args = parser.parse_args(argv)
    if args.make:
        make(args.root, args.manifest)
    else:
        verify(args.root, args.manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
