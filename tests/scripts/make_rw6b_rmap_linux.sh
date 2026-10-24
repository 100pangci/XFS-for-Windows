#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B: create the canonical Linux rmapbt fixture.
#
#   mkfs.xfs -m crc=1,rmapbt=1,reflink=0,finobt=1,inobtcount=1,bigtime=1
#             -i sparse=1,nrext64=1
#
# and populates every forward structure the reverse-map writer must preserve:
# data extents (written/unwritten), bmbt blocks, directories in all formats,
# hard links, inline/remote symlinks, inode chunks (including sparse ones),
# finobt growth and a multi-level rmapbt.
#
# usage: make_rw6b_rmap_linux.sh <outdir>
set -euo pipefail

OUT=${1:?usage: make_rw6b_rmap_linux.sh <outdir>}
SIZE_MB=${SIZE_MB:-2048}
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
for tool in mkfs.xfs xfs_info xfs_repair xfs_db xfs_io losetup python3; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

PARENT_OPT=""
MKFS_HELP=$(mkfs.xfs 2>&1 || true)
if printf '%s\n' "$MKFS_HELP" | grep -q 'parent='; then
    PARENT_OPT="-n parent=0"
fi
# reflink must stay off: the writable profile is rmapbt=1,reflink=0.
MKFS_OPTS="-m crc=1,rmapbt=1,reflink=0,finobt=1,inobtcount=1,bigtime=1 -i sparse=1,nrext64=1"

IMG="$OUT/rmap.img"
rm -f "$IMG"
truncate -s "${SIZE_MB}M" "$IMG"
LOOP=$(losetup --find --show "$IMG")
mkfs.xfs -f -q $MKFS_OPTS $PARENT_OPT "$LOOP"
mount "$LOOP" "$MOUNT"
xfs_info "$MOUNT" > "$OUT/rmap.xfs_info.txt"

python3 - "$MOUNT" <<'PY'
import hashlib
import os
import sys

root = sys.argv[1]
block = 4096


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def sync_dir(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


# Small directory (shortform -> block format), medium (leaf), large (node).
small = os.path.join(root, "small")
os.makedirs(small, exist_ok=True)
for i in range(8):
    write(os.path.join(small, "s%02d.bin" % i), b"small-%02d" % i)

blockdir = os.path.join(root, "blockdir")
os.makedirs(blockdir, exist_ok=True)
for i in range(80):
    write(os.path.join(blockdir, "b%03d.bin" % i), b"block-%03d" % i)

bigdir = os.path.join(root, "bigdir")
os.makedirs(bigdir, exist_ok=True)
for i in range(5000):
    write(os.path.join(bigdir, "n%05d.bin" % i), b"n%05d" % i)
sync_dir(bigdir)

# File with an unwritten extent and a later written extent.
unwritten = os.path.join(root, "unwritten.bin")
write(unwritten, b"")
# XFS fallocate (mode 0) creates unwritten extents.  Writing the tail below
# leaves the first MiB unwritten.
if os.system("xfs_io -f -c 'falloc 0 1048576' '%s' >/dev/null" %
             unwritten) != 0:
    raise SystemExit("xfs_io falloc failed")
with open(unwritten, "r+b") as f:
    f.seek(2 * 1024 * 1024)
    f.write(b"written tail")
    f.flush()
    os.fsync(f.fileno())

# Sparse file (holes) and a highly fragmented btree-format file.
sparse = os.path.join(root, "sparse.bin")
with open(sparse, "wb") as f:
    for i in range(64):
        f.seek(i * 128 * block)
        f.write((b"S%07d" % i) * 64)
    f.flush()
    os.fsync(f.fileno())

fragmented = os.path.join(root, "fragmented.bin")
with open(fragmented, "wb") as f:
    for i in range(600):
        f.seek(i * 2 * block)
        f.write((b"F%07d" % i) * (block // 8))
    f.flush()
    os.fsync(f.fileno())

# Hard links and both symlink representations.
os.link(os.path.join(small, "s00.bin"), os.path.join(root, "hardlink.bin"))
os.symlink("small/s00.bin", os.path.join(root, "inline.link"))
os.symlink("../" + ("long/" * 120), os.path.join(root, "remote.link"))

# Inode churn: allocate enough chunks to grow inobt/finobt, then free half so
# the finobt holds records and later allocations can use sparse chunks.
churn = os.path.join(root, "churn")
os.makedirs(churn, exist_ok=True)
for i in range(6000):
    write(os.path.join(churn, "c%05d" % i), b"c")
for i in range(0, 6000, 2):
    os.unlink(os.path.join(churn, "c%05d" % i))
for i in range(6000, 7000):
    write(os.path.join(churn, "c%05d" % i), b"d")
sync_dir(churn)

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
# Keep a host-side copy for the Windows reader differential.
cp "$MOUNT/MANIFEST.sha256" "$OUT/MANIFEST.sha256"
umount "$MOUNT"
losetup -d "$LOOP"
LOOP=""

xfs_repair -n "$IMG" > "$OUT/rmap.repair.txt" 2>&1
if grep -Eiq 'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
        "$OUT/rmap.repair.txt"; then
    echo "xfs_repair rejected the rmap fixture" >&2
    cat "$OUT/rmap.repair.txt" >&2
    exit 1
fi

# Save the layout of the reverse-mapping btree.  The fixture is expected to be
# at least two levels high (thousands of distinct owners); a one-level tree
# would not exercise the writer's node paths.
{
    echo "== AG0 AGF =="
    xfs_db -r -c 'agf 0' -c 'p rmaproot rmaplevel rmapblocks btreeblks' "$IMG"
    echo "== AG0 rmap root =="
    xfs_db -r -c 'agf 0' -c 'addr rmaproot' -c 'p' "$IMG"
    for ag in 0 1 2 3; do
        echo "== AG$ag rmap level/blocks =="
        xfs_db -r -c "agf $ag" -c 'p rmaproot rmaplevel rmapblocks' "$IMG"
    done
} > "$OUT/rmap.layout.txt"
cat "$OUT/rmap.layout.txt"

level=$(xfs_db -r -c 'agf 0' -c 'p rmaplevel' "$IMG" |
        sed -n 's/.*rmaplevel = \([0-9][0-9]*\).*/\1/p')
if [ -z "$level" ] || [ "$level" -lt 2 ]; then
    echo "fixture rmapbt did not reach level 2 (got '${level}')" >&2
    exit 1
fi

echo "wrote $IMG"

# A second genuine Linux fixture makes internal rmap splits affordable to
# exercise: 1 KiB blocks give max_leaf=40, max_node=22. This does not replace
# the populated 2 GiB/4 KiB fixture above.
tree_img="$OUT/rmap-tree.img"
truncate -s 512M "$tree_img"
mkfs.xfs -f -b size=1024 -n size=1024 \
    -m crc=1,rmapbt=1,reflink=0,finobt=1,inobtcount=1,bigtime=1 \
    -i sparse=1,nrext64=1 "$tree_img"
xfs_repair -n "$tree_img" > "$OUT/rmap-tree.repair.txt" 2>&1
python3 "$(cd "$(dirname "$0")/../.." && pwd)/tests/tools/rw6b_rmap_probe.py" "$tree_img"
