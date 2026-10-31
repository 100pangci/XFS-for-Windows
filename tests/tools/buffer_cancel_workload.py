#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""QUAL-2 metadata-reuse workload for real Linux buffer cancellations.

Runs against a mounted XFS filesystem until killed.  Each worker frees
metadata that was logged earlier, which makes the kernel call
xfs_trans_binval() and write an XFS_BLF_CANCEL stale item:

  bmbt     fragmented files with a multi-level bmbt, alternately written and
           truncated to zero; freeing the leaves and collapsing the root
           invalidates bmbt blocks (xfs_bmap_btree_to_extents).
  dir      node-format directories filled with many entries and removed;
           freeing the data/leaf/free blocks invalidates them
           (xfs_dir2_shrink_inode / xfs_da_shrink_inode).
  inode    large batches of empty files created and unlinked.  When a chunk
           becomes completely free the kernel deletes the inobt record, marks
           every inode cluster buffer stale and cancels it (xfs_ifree_cluster).
  symlink  remote symlinks (target longer than the inline area) created and
           removed (xfs_inactive_symlink_rmt).
  attr     files carrying a remote attribute value, unlinked so
           xfs_attr_inactive frees the DA/leaf blocks.

All workloads also free extents through deferred operations, so their
transactions carry EFI/RUI intents.  Those intents pin the log tail long
enough for a device-mapper snapshot to observe the cancellation before the
record leaves the recovery window.  That is why the capture script mounts
with rmapbt enabled.

usage: buffer_cancel_workload.py <mountpoint> [mode]

mode is "mixed" (default) or a comma separated subset of
bmbt,dir,inode,symlink,attr.  The process only stops when it receives
SIGTERM/SIGINT.
"""

import os
import signal
import sys
import threading

BLOCK = 4096
STRIDE = 8192
STOP = threading.Event()


def stop(_signum, _frame):
    STOP.set()


def safe_unlink(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    except OSError:
        pass


def safe_rmdir(path):
    try:
        os.rmdir(path)
    except OSError:
        pass


def sparse_extents(path, count, tag):
    """Write `count` single-block extents separated by holes."""
    with open(path, "wb") as out:
        out.truncate(count * STRIDE)
        for i in range(count):
            out.seek(i * STRIDE)
            out.write(tag + i.to_bytes(8, "little") + bytes(BLOCK - len(tag) - 8))
        out.flush()
        os.fsync(out.fileno())


def bmbt_worker(root, worker):
    """Keep a multi-level bmbt and collapse it repeatedly."""
    path = os.path.join(root, "bc-bmbt-%d.bin" % worker)
    big = 3200      # ~13 data MiB over a 26 MiB sparse file
    small = 1100
    toggle = False
    while not STOP.is_set():
        sparse_extents(path, big if toggle else small, b"BM" + bytes([worker]))
        toggle = not toggle
        try:
            os.truncate(path, 0)
        except OSError:
            pass
        safe_unlink(path)
        # The write deliberately fsyncs: it pushes the log records for the
        # whole filesystem, including the cancels other workers just queued.
        STOP.wait(0.005)
    safe_unlink(path)


def dir_worker(root):
    """Create node-format directories, then remove every entry and the dir."""
    base = os.path.join(root, "bc-dir")
    os.makedirs(base, exist_ok=True)
    name = 0
    while not STOP.is_set():
        path = os.path.join(base, "d%06d" % (name & 0xffff))
        name += 1
        if not os.path.isdir(path):
            os.mkdir(path)
        entries = 1200
        for i in range(entries):
            entry = os.path.join(path, "e%05d" % i)
            try:
                fd = os.open(entry, os.O_CREAT | os.O_WRONLY, 0o600)
                os.close(fd)
            except OSError:
                break
        # The directory now has data, leaf and free blocks.  Removing the
        # entries shrinks it back, and each freed block is cancelled.
        try:
            names = os.listdir(path)
        except OSError:
            names = []
        for entry in names:
            safe_unlink(os.path.join(path, entry))
        safe_rmdir(path)
        STOP.wait(0.005)


def inode_worker(root):
    """Allocate complete inode chunks and free them again."""
    base = os.path.join(root, "bc-inode")
    os.makedirs(base, exist_ok=True)
    batch = 2048
    round_number = 0
    while not STOP.is_set():
        paths = []
        for i in range(batch):
            entry = os.path.join(base, "i%06d-%05d" % (round_number, i))
            try:
                fd = os.open(entry, os.O_CREAT | os.O_WRONLY, 0o600)
                os.close(fd)
                paths.append(entry)
            except OSError:
                break
        for entry in paths:
            safe_unlink(entry)
        # xfs_fs_sync_fs() flushes inodegc, which runs the chunk freeing
        # transactions and then the log force that writes their cancels.
        try:
            os.sync()
        except OSError:
            pass
        round_number += 1
        STOP.wait(0.01)


def symlink_worker(root):
    """Create and remove remote symlinks (target beyond the inline area)."""
    base = os.path.join(root, "bc-sym")
    os.makedirs(base, exist_ok=True)
    target = "s" * 900
    name = 0
    while not STOP.is_set():
        path = os.path.join(base, "l%06d" % (name & 0xffff))
        name += 1
        try:
            os.symlink(target, path)
            os.unlink(path)
        except OSError:
            pass
        STOP.wait(0.002)


def attr_worker(root):
    """Create and remove files with remote attribute values."""
    base = os.path.join(root, "bc-attr")
    os.makedirs(base, exist_ok=True)
    value = b"A" * 60000
    name = 0
    while not STOP.is_set():
        path = os.path.join(base, "a%06d" % (name & 0xffff))
        name += 1
        try:
            fd = os.open(path, os.O_CREAT | os.O_WRONLY, 0o600)
            os.close(fd)
            os.setxattr(path, "user.qual2", value)
            os.unlink(path)
        except OSError:
            pass
        STOP.wait(0.002)


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    root = argv[1]
    mode = argv[2] if len(argv) > 2 else "mixed"
    if mode == "mixed":
        modes = {"bmbt", "dir", "inode", "symlink", "attr"}
    else:
        modes = {m for m in mode.split(",") if m}
    known = {"bmbt", "dir", "inode", "symlink", "attr"}
    unknown = modes - known
    if unknown or not modes:
        print("unknown workload mode(s): %s" % ",".join(sorted(unknown)),
              file=sys.stderr)
        return 2
    if not os.path.isdir(root):
        print("not a directory: %s" % root, file=sys.stderr)
        return 2

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    threads = []
    if "bmbt" in modes:
        for worker in range(2):
            threads.append(threading.Thread(target=bmbt_worker,
                                            args=(root, worker)))
    if "dir" in modes:
        threads.append(threading.Thread(target=dir_worker, args=(root,)))
    if "inode" in modes:
        threads.append(threading.Thread(target=inode_worker, args=(root,)))
    if "symlink" in modes:
        threads.append(threading.Thread(target=symlink_worker, args=(root,)))
    if "attr" in modes:
        threads.append(threading.Thread(target=attr_worker, args=(root,)))

    for thread in threads:
        thread.start()
    while not STOP.is_set():
        STOP.wait(0.1)
    for thread in threads:
        thread.join(timeout=30)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
