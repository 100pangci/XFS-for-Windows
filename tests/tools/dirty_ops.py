#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Dirty-log operation matrix helper (RW-5, runs on Linux).

Three jobs:

  dirty_ops.py setup <mountpoint>
      builds the base tree every scenario starts from.

  dirty_ops.py capture <scenario> <mountpoint> <manifest.json> <dm-name>
      performs one namespace/content operation, forces the *log* to be durable
      (fsync waits for the log record; the metadata behind it may still be in
      the journal - that is exactly the crash state recovery has to handle),
      writes the expected post-operation state (type, size, sha256, link
      target, nlink, mtime) as JSON, and then suspends the device-mapper device
      right away, so the operation's transaction is still inside the recovery
      window when the device bytes are copied.

  dirty_ops.py verify <mountpoint> <manifest.json>
      compares a mounted filesystem against a manifest (used by the Linux
      verifier and by the generator's own kernel-replay sanity check).

The snapshot itself is taken by make_dirty_logs_linux.sh after capture
returns, so the manifest always describes the state at capture time.
"""

import hashlib
import json
import os
import stat
import subprocess
import sys

BASE = "base"


def sha256(path, limit=1 << 20):
    try:
        if os.path.getsize(path) > limit:
            return "size>%d" % limit
        digest = hashlib.sha256()
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return "unreadable"


def manifest(mountpoint):
    entries = {}
    root = os.path.join(mountpoint, BASE)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(dirnames + filenames):
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, mountpoint)
            st = os.lstat(path)
            entry = {"mode": stat.S_IMODE(st.st_mode), "nlink": st.st_nlink,
                     # mtime changes on every scenario (utime, rename,
                     # create/unlink through the parent, extend, truncate) and
                     # is never changed by reading, so comparing it proves the
                     # timestamp bytes were replayed, not just the namespace.
                     "mtime_ns": st.st_mtime_ns}
            if stat.S_ISDIR(st.st_mode):
                entry["type"] = "dir"
            elif stat.S_ISLNK(st.st_mode):
                entry["type"] = "symlink"
                entry["target"] = os.readlink(path)
            elif stat.S_ISREG(st.st_mode):
                entry["type"] = "file"
                entry["size"] = st.st_size
                entry["sha256"] = sha256(path)
            else:
                entry["type"] = "other"
            entries[rel.replace(os.sep, "/")] = entry
    return entries


def fsync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_file(path, data):
    with open(path, "wb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())


def setup(mountpoint):
    root = os.path.join(mountpoint, BASE)
    os.makedirs(os.path.join(root, "dir-a"), exist_ok=True)
    os.makedirs(os.path.join(root, "dir-b"), exist_ok=True)
    write_file(os.path.join(root, "file-a"), b"file-a:" + b"A" * 200)
    write_file(os.path.join(root, "file-b"), b"file-b:" + b"B" * 300)
    write_file(os.path.join(root, "dir-a", "nested"),
               b"nested:" + b"N" * 100)
    write_file(os.path.join(root, "empty"), b"")
    write_file(os.path.join(root, "replace-me"), b"old target\n")
    os.symlink("file-a", os.path.join(root, "link-short"))
    fsync_dir(root)
    fsync_dir(os.path.join(root, "dir-a"))


def run(scenario, mountpoint):
    root = os.path.join(mountpoint, BASE)
    if scenario == "timestamp":
        target = os.path.join(root, "file-a")
        os.utime(target, ns=(1700000000123456789, 1700000000987654321))
        fd = os.open(target, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    elif scenario == "extend":
        target = os.path.join(root, "file-b")
        with open(target, "ab") as handle:
            handle.write(b"extend:" + bytes(range(256)) * 512)
            handle.flush()
            os.fsync(handle.fileno())
    elif scenario == "truncate":
        target = os.path.join(root, "file-b")
        with open(target, "r+b") as handle:
            handle.truncate(64)
            handle.flush()
            os.fsync(handle.fileno())
    elif scenario == "create":
        write_file(os.path.join(root, "created"),
                   b"created:" + b"C" * 4096)
        fsync_dir(root)
    elif scenario == "unlink":
        os.unlink(os.path.join(root, "file-b"))
        fsync_dir(root)
    elif scenario == "mkdir":
        os.mkdir(os.path.join(root, "new-dir"))
        fsync_dir(root)
    elif scenario == "rmdir":
        os.rmdir(os.path.join(root, "dir-b"))
        fsync_dir(root)
    elif scenario == "rename-same":
        os.rename(os.path.join(root, "file-a"),
                  os.path.join(root, "file-a-renamed"))
        fsync_dir(root)
    elif scenario == "rename-cross":
        os.rename(os.path.join(root, "file-a"),
                  os.path.join(root, "dir-a", "file-a-moved"))
        fsync_dir(root)
        fsync_dir(os.path.join(root, "dir-a"))
    elif scenario == "rename-replace":
        os.rename(os.path.join(root, "file-b"),
                  os.path.join(root, "replace-me"))
        fsync_dir(root)
    elif scenario == "hardlink":
        os.link(os.path.join(root, "file-a"),
                os.path.join(root, "dir-b", "file-a-link"))
        fsync_dir(os.path.join(root, "dir-b"))
    elif scenario == "symlink-inline":
        os.symlink("file-b", os.path.join(root, "link-inline"))
        fsync_dir(root)
    elif scenario == "symlink-remote":
        target = "remote-" + "x" * 600  # far beyond the 156-byte inline limit
        os.symlink(target, os.path.join(root, "link-remote"))
        fsync_dir(root)
    else:
        raise SystemExit("unknown scenario: %s" % scenario)


def verify(mountpoint, manifest_path):
    with open(manifest_path, "r") as handle:
        expected = json.load(handle)["entries"]
    actual = manifest(mountpoint)
    problems = []
    for path, entry in expected.items():
        if path not in actual:
            problems.append("missing: %s" % path)
            continue
        for key, value in entry.items():
            if actual[path].get(key) != value:
                problems.append("%s: %s expected %r got %r" %
                                (path, key, value, actual[path].get(key)))
    for path in actual:
        if path not in expected:
            problems.append("unexpected: %s" % path)
    for problem in problems:
        print("DIFF: " + problem)
    return 0 if not problems else 1


def capture(scenario, mountpoint, manifest_path, dm):
    """Runs the operation, records the expected state, then freezes the block
    device - in one process.

    fsync makes the log record durable but does not stop the AIL from writing
    the metadata back, which would move the tail past the record. Freezing the
    dm device a couple of milliseconds after the fsync (instead of after
    another tool start) keeps the operation's transaction in the recovery
    window.
    """
    run(scenario, mountpoint)
    data = manifest(mountpoint)
    with open(manifest_path, "w") as handle:
        json.dump({"scenario": scenario, "entries": data}, handle,
                  indent=2, sort_keys=True)
    subprocess.run(["dmsetup", "suspend", dm], check=True)
    print("%s: %d entries, device frozen" % (scenario, len(data)))


def main(argv):
    if len(argv) == 3 and argv[1] == "setup":
        setup(argv[2])
        return 0
    if len(argv) == 4 and argv[1] == "verify":
        return verify(argv[2], argv[3])
    if len(argv) == 6 and argv[1] == "capture":
        capture(argv[2], argv[3], argv[4], argv[5])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
