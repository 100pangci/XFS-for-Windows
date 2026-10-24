#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B writer crash fixture: a small, real Linux-created rmapbt filesystem
# with one prepared object for every crash scenario.
#
#   * base.bin        two-block file (write/append, link)
#   * rewrite.bin     append target
#   * frag.bin        128 single-block extents (truncate/unlink)
#   * unwritten.bin   one MiB of unwritten extents (convert)
#   * falloc.bin      empty allocation target
#   * dirA, dirB      block-format directories (create/rename/symlink)
#   * emptydir        rmdir target
#
# The image is created by mkfs.xfs with rmapbt=1,reflink=0 and is never
# touched by a Windows writer here, so it is a genuine Linux fixture.
#
# usage: make_rw6b_crash_linux.sh <outdir>
set -euo pipefail

OUT=${1:?usage: make_rw6b_crash_linux.sh <outdir>}
SIZE_MB=${SIZE_MB:-384}
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
for tool in mkfs.xfs xfs_info xfs_repair xfs_io xfs_bmap losetup python3; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

PARENT_OPT=""
MKFS_HELP=$(mkfs.xfs 2>&1 || true)
if printf '%s\n' "$MKFS_HELP" | grep -q 'parent='; then
    PARENT_OPT="-n parent=0"
fi
MKFS_OPTS="-m crc=1,rmapbt=1,reflink=0,finobt=1,inobtcount=1,bigtime=1 -i sparse=1,nrext64=1"

IMG="$OUT/rmap-crash.img"
rm -f "$IMG"
truncate -s "${SIZE_MB}M" "$IMG"
LOOP=$(losetup --find --show "$IMG")
mkfs.xfs -f -q $MKFS_OPTS $PARENT_OPT "$LOOP"
mount "$LOOP" "$MOUNT"
xfs_info "$MOUNT" > "$OUT/rmap-crash.xfs_info.txt"

python3 - "$MOUNT" <<'PY'
import os
import sys

root = sys.argv[1]
block = 4096


def write(path, data):
    with open(path, "wb") as out:
        out.write(data)
        out.flush()
        os.fsync(out.fileno())


def write_at(path, offset, data):
    with open(path, "r+b") as out:
        out.seek(offset)
        out.write(data)
        out.flush()
        os.fsync(out.fileno())


write(os.path.join(root, "base.bin"), b"A" * 8192)
write(os.path.join(root, "rewrite.bin"), b"R" * 8192)
write(os.path.join(root, "link.bin"), b"L" * 4096)

frag = os.path.join(root, "frag.bin")
with open(frag, "wb") as out:
    for i in range(128):
        out.seek(i * 2 * block)
        out.write((b"F%07d" % i) * (block // 8))
    out.flush()
    os.fsync(out.fileno())

write(os.path.join(root, "falloc.bin"), b"")
write(os.path.join(root, "unwritten.bin"), b"")
if os.system("xfs_io -f -c 'falloc 0 1048576' '%s' >/dev/null" %
             os.path.join(root, "unwritten.bin")) != 0:
    raise SystemExit("xfs_io falloc failed")

for name in ("dirA", "dirB", "emptydir"):
    os.makedirs(os.path.join(root, name), exist_ok=True)
for name in ("dirA", "dirB"):
    for i in range(300):
        write(os.path.join(root, name, "f%05d.bin" % i), b"d%05d" % i)
PY

sync
umount "$MOUNT"
losetup -d "$LOOP"
LOOP=""

xfs_repair -n "$IMG" > "$OUT/rmap-crash.repair.txt" 2>&1
if grep -Eiq 'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
        "$OUT/rmap-crash.repair.txt"; then
    echo "xfs_repair rejected the crash fixture" >&2
    cat "$OUT/rmap-crash.repair.txt" >&2
    exit 1
fi

grep -q 'rmapbt=1' "$OUT/rmap-crash.xfs_info.txt" || {
    echo "crash fixture is not rmapbt=1" >&2
    exit 1
}
grep -q 'reflink=0' "$OUT/rmap-crash.xfs_info.txt" || {
    echo "crash fixture unexpectedly has reflink" >&2
    exit 1
}

# Temporary mount for bmap/oracle inspection.
LOOP=$(losetup --find --show "$IMG")
mkdir -p "$WORK/check"
mount -o ro "$LOOP" "$WORK/check"
frag_extents=$(xfs_bmap "$WORK/check/frag.bin" | grep -c '^ *[0-9]' || true)
unwritten=$(xfs_bmap -v "$WORK/check/unwritten.bin")
umount "$WORK/check"
losetup -d "$LOOP"
LOOP=""
if [ "$frag_extents" -lt 100 ]; then
    echo "frag.bin has only $frag_extents extents (need >=100)" >&2
    exit 1
fi
if ! printf '%s\n' "$unwritten" | grep -q 'unwritten'; then
    echo "unwritten.bin has no unwritten extent" >&2
    printf '%s\n' "$unwritten" >&2
    exit 1
fi

python3 "$(cd "$(dirname "$0")/../.." && pwd)/tests/tools/rw6b_rmap_probe.py" \
    "$IMG" --quiet
echo "crash fixture extents: frag=$frag_extents"
echo "wrote $IMG"
