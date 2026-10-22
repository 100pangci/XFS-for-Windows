#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6A: create a Linux-generated modern-inode XFS image.
#
# The filesystem is formatted by the distribution's mkfs.xfs with:
#   finobt, inobtcount, sparse inodes, bigtime, NREXT64
# and with rmapbt/reflink/parent-pointers disabled (the remaining blockers of
# this milestone). It is then populated with ordinary files, directories,
# hard links, symlinks, pre-1970 / post-2038 / far-future timestamps, an
# inode-churn tree (allocation + free so the finobt changes), and a file with
# hundreds of extents.
#
# usage: make_rw6_modern_linux.sh <outdir>
set -euo pipefail

OUT=${1:?usage: make_rw6_modern_linux.sh <outdir>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
SIZE_MB=${SIZE_MB:-1536}
WORK=$(mktemp -d)
LOOP=""
MOUNT="$WORK/mnt"
mkdir -p "$MOUNT" "$OUT"

cleanup() {
    set +e
    mountpoint -q "$MOUNT" && umount -l "$MOUNT"
    [ -n "$LOOP" ] && losetup -d "$LOOP" >/dev/null 2>&1
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ "$(id -u)" != "0" ]; then
    echo "must run as root (loop device/mount)" >&2
    exit 1
fi
for tool in mkfs.xfs xfs_info xfs_repair losetup python3; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

PARENT_OPT=""
if mkfs.xfs 2>&1 | grep -q 'parent='; then
    PARENT_OPT="-n parent=0"
fi
MKFS_OPTS="-m crc=1,finobt=1,inobtcount=1,bigtime=1,rmapbt=0,reflink=0 -i sparse=1,nrext64=1"

IMG="$OUT/modern.img"
rm -f "$IMG"
truncate -s "${SIZE_MB}M" "$IMG"
LOOP=$(losetup --find --show "$IMG")
mkfs.xfs -f -q $MKFS_OPTS $PARENT_OPT "$LOOP"
mount "$LOOP" "$MOUNT"
xfs_info "$MOUNT" > "$OUT/modern.xfs_info.txt"

python3 - "$MOUNT" <<'PY'
import calendar
import hashlib
import os
import sys
import time

root = sys.argv[1]
now = int(time.time())
block = 4096


def write(path, data, mode="wb"):
    with open(path, mode) as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


# Ordinary tree.
for d in range(8):
    os.makedirs(os.path.join(root, "dir%02d" % d), exist_ok=True)
for i in range(1200):
    d = "dir%02d" % (i % 8)
    path = os.path.join(root, d, "file%04d.bin" % i)
    payload = ("rw6-%04d-" % i).encode() * (32 + (i % 17))
    write(path, payload)
# Hard link and symlinks.
os.link(os.path.join(root, "dir00", "file0000.bin"),
        os.path.join(root, "hardlink.bin"))
os.symlink("dir00/file0000.bin", os.path.join(root, "inline.link"))
os.symlink("../" + ("long/" * 100), os.path.join(root, "remote.link"))

# Timestamp matrix (pre-1970, 2038 boundary, post-2038, far future).
for name, stamp in (
        ("old.stamp", "1965-01-01 00:00:00"),
        ("y2038.stamp", "2038-01-19 03:14:08"),
        ("y2100.stamp", "2100-01-01 00:00:00"),
        ("future.stamp", "2400-01-01 00:00:00")):
    path = os.path.join(root, name)
    write(path, name.encode())
    epoch = calendar.timegm(time.strptime(stamp, "%Y-%m-%d %H:%M:%S"))
    os.utime(path, (epoch, epoch))

# A large extent map: 400 written blocks separated by holes.
path = os.path.join(root, "fragmented.bin")
with open(path, "wb") as f:
    for i in range(400):
        f.seek(i * 2 * block)
        f.write((b"%08d" % i) * (block // 8))
    f.flush()
    os.fsync(f.fileno())

# Inode churn: allocate more inodes than the initial chunks, then free every
# second one so the finobt carries records with free inodes and inode chunks
# can become sparse under fragmentation.
churn = os.path.join(root, "churn")
os.makedirs(churn, exist_ok=True)
for i in range(2500):
    with open(os.path.join(churn, "c%05d" % i), "wb") as f:
        f.write(b"x")
for i in range(0, 2500, 2):
    os.unlink(os.path.join(churn, "c%05d" % i))
for i in range(2500, 3200):
    with open(os.path.join(churn, "c%05d" % i), "wb") as f:
        f.write(b"y")

# Deterministic manifest of the files Windows must preserve.
manifest = os.path.join(root, "MANIFEST.sha256")
with open(manifest, "w", encoding="utf-8") as out:
    for base, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            rel = os.path.relpath(path, root)
            if rel == "MANIFEST.sha256":
                continue
            if os.path.islink(path):
                out.write("link %s %s\n" % (rel, os.readlink(path)))
                continue
            with open(path, "rb") as f:
                digest = hashlib.sha256(f.read()).hexdigest()
            st = os.stat(path)
            out.write("file %s %d %d %s\n" %
                      (rel, st.st_size, st.st_mtime, digest))
    out.flush()
    os.fsync(out.fileno())
PY

sync
umount "$MOUNT"
losetup -d "$LOOP"
LOOP=""
xfs_repair -n "$IMG" > "$OUT/modern.repair.txt" 2>&1
if grep -qi 'corrupt\|would fix' "$OUT/modern.repair.txt"; then
    echo "xfs_repair rejected the fixture" >&2
    cat "$OUT/modern.repair.txt" >&2
    exit 1
fi
echo "wrote $IMG"
