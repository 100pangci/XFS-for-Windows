#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Dirty-log operation matrix helper (RW-5, runs on Linux).

Three jobs:

  dirty_ops.py setup <mountpoint>
      builds the base tree every scenario starts from.

  dirty_ops.py capture <scenario> <mountpoint> <manifest.json> <dm> <live> <img>
      performs one namespace/content operation, forces the *log* to be durable
      (fsync waits for the log record; the metadata behind it may still be in
      the journal - that is exactly the crash state recovery has to handle),
      writes the expected post-operation state (type, size, sha256, link
      target, nlink, mtime) as JSON, freezes the device-mapper device, copies
      the loop backing file to <img> and resumes the device - all in one
      process, so the operation's transaction is still inside the recovery
      window when the bytes are copied. An unforced noise stream in noise/
      keeps the CIL busy until the freeze, so the kernel cannot cover the log
      (which would move the tail past the operation).

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
import threading
import time

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


def _noise_files(mountpoint):
    """Prepares the noise files (outside base/, so outside the manifest)."""
    directory = os.path.join(mountpoint, "noise")
    os.makedirs(directory, exist_ok=True)
    for i in range(64):
        write_file(os.path.join(directory, "n%d" % i), b"n" * 512)
    return directory


def _noise(directory, stop):
    """Keeps the log busy until `stop` is set.

    The kernel covers an idle log every syncd period: it syncs the superblock,
    pushes the whole AIL and moves the tail past everything that was just
    logged, which leaves the recovery window with nothing but the covering
    transaction (superblock buffers). Covering cannot start while the CIL or
    the AIL is non-empty, so this stream of committed inode-timestamp
    transactions keeps the log busy until the device is frozen; the operation's
    own fsync then pushes the whole CIL - noise and operation together - into
    the log.

    The writes are deliberately *not* fsync'd: they only have to commit
    transactions into the CIL, and not waiting for I/O keeps this thread out of
    uninterruptible sleep, where a suspend of the block device would keep the
    process from ever exiting.
    """
    i = 0
    while not stop.is_set():
        try:
            with open(os.path.join(directory, "n%d" % (i % 64)), "r+b") as handle:
                handle.seek(0)
                handle.write(b"n" * 512)
        except OSError:
            break
        i += 1
        time.sleep(0.002)  # ~500 transactions/s, a few dozen per capture


def _copy_sparse(source, target):
    """Copies `source` to `target`, skipping all-zero chunks (conv=sparse)."""
    offset = 0
    with open(source, "rb") as src, open(target, "wb") as dst:
        while True:
            chunk = src.read(1 << 20)
            if not chunk:
                break
            if chunk.count(0) == len(chunk):
                dst.seek(len(chunk), os.SEEK_CUR)
            else:
                dst.write(chunk)
            offset += len(chunk)
        dst.truncate(offset)


def capture(scenario, mountpoint, manifest_path, dm, live, image, noise=True):
    """Runs the operation, records the expected state, freezes the block device,
    copies the device bytes and resumes it - in one process.

    fsync makes the log record durable but does not stop the kernel from
    syncing the metadata back (which moves the tail past the record). The
    noise stream keeps the CIL/AIL non-empty so the log covering stays away,
    and the device freeze follows the fsync immediately. The noise thread is
    still running when the device is suspended; its next file operation blocks,
    so the device is resumed again before this process exits (a thread left in
    uninterruptible sleep on a frozen device would keep the process - and the
    shell waiting for it - from ever finishing).
    """
    stop = threading.Event()
    worker = None
    if noise:
        directory = _noise_files(mountpoint)
        worker = threading.Thread(target=_noise, args=(directory, stop),
                                  daemon=True)
        worker.start()
        time.sleep(0.2)  # let the log fill before the operation
    try:
        run(scenario, mountpoint)
        data = manifest(mountpoint)
        with open(manifest_path, "w") as handle:
            json.dump({"scenario": scenario, "entries": data}, handle,
                      indent=2, sort_keys=True)
        subprocess.run(["dmsetup", "suspend", dm], check=True)
        # The device is frozen: these are exactly the bytes that reached it.
        _copy_sparse(live, image)
        subprocess.run(["dmsetup", "resume", dm], check=True)
    finally:
        stop.set()
        if worker is not None:
            worker.join(timeout=2.0)
    print("%s: %d entries, device frozen and copied" % (scenario, len(data)))


def main(argv):
    if len(argv) == 3 and argv[1] == "setup":
        setup(argv[2])
        return 0
    if len(argv) == 4 and argv[1] == "verify":
        return verify(argv[2], argv[3])
    if len(argv) in (8, 9) and argv[1] == "capture":
        capture(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7],
                noise=(len(argv) == 8 or argv[8] != "no-noise"))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
