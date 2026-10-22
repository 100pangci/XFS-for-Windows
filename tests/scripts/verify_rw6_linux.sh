#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6A: verify a modern-inode image with the Linux kernel and xfs_repair.
#
# usage:
#   verify_rw6_linux.sh <modern.img> [--windows-mutated]
#
# The image is the output of make_rw6_modern_linux.sh (and optionally one that
# XFS-for-Windows has mutated). The original MANIFEST.sha256 is rechecked; a
# Windows mutation must have added /win without changing the Linux tree.
set -euo pipefail

IMG=${1:?usage: verify_rw6_linux.sh <image> [--windows-mutated]}
MODE=${2:-}
WORK=$(mktemp -d)
MOUNT="$WORK/mnt"
mkdir -p "$MOUNT"
cleanup() {
    set +e
    mountpoint -q "$MOUNT" && umount -l "$MOUNT"
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ "$(id -u)" != "0" ]; then
    echo "must run as root (loop mount)" >&2
    exit 1
fi
for tool in xfs_repair xfs_db mount python3; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

echo "== xfs_repair -n =="
OUT="$WORK/repair.txt"
set +e
xfs_repair -n "$IMG" > "$OUT" 2>&1
RC=$?
set -e
cat "$OUT"
if [ "$RC" -ne 0 ] || grep -qiE \
        'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
        "$OUT"; then
    echo "xfs_repair rejected the image" >&2
    exit 1
fi

mount -o ro,loop "$IMG" "$MOUNT"
python3 - "$MOUNT" "$MODE" <<'PY'
import hashlib
import os
import stat
import sys

root, mode = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else ""
manifest = os.path.join(root, "MANIFEST.sha256")
if not os.path.exists(manifest):
    raise SystemExit("missing MANIFEST.sha256")
checked = 0
with open(manifest, encoding="utf-8") as f:
    for line in f:
        line = line.rstrip("\n")
        if not line:
            continue
        parts = line.split(" ")
        if parts[0] == "link":
            path = os.path.join(root, parts[1])
            if not os.path.islink(path):
                raise SystemExit("%s is no longer a symlink" % parts[1])
            if os.readlink(path) != " ".join(parts[2:]):
                raise SystemExit("%s symlink target changed" % parts[1])
            checked += 1
            continue
        if parts[0] != "file":
            raise SystemExit("bad manifest line: %s" % line)
        rel, size, mtime, digest = parts[1], int(parts[2]), int(parts[3]), parts[4]
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            raise SystemExit("%s disappeared" % rel)
        st = os.stat(path)
        if st.st_size != size:
            raise SystemExit("%s size %d != %d" % (rel, st.st_size, size))
        if int(st.st_mtime) != mtime:
            raise SystemExit("%s mtime %d != %d" %
                             (rel, int(st.st_mtime), mtime))
        with open(path, "rb") as f:
            if hashlib.sha256(f.read()).hexdigest() != digest:
                raise SystemExit("%s content changed" % rel)
        checked += 1
print("manifest OK: %d entries" % checked)

if mode == "--windows-mutated":
    def read(rel):
        with open(os.path.join(root, rel), "rb") as f:
            return f.read()

    if read("win/created.bin") != b"windows-rw6":
        raise SystemExit("/win/created.bin content mismatch")
    if read("win/renamed.bin") != b"renamed-by-windows":
        raise SystemExit("/win/renamed.bin content mismatch")
    if read("win/dir/nested.bin") != b"nested":
        raise SystemExit("/win/dir/nested.bin content mismatch")
    a = os.stat(os.path.join(root, "win/renamed.bin"))
    b = os.stat(os.path.join(root, "win/hardlink.bin"))
    if a.st_ino != b.st_ino or a.st_nlink != 2:
        raise SystemExit("hard link was not preserved")
    link = os.path.join(root, "win/link")
    if not os.path.islink(link) or os.readlink(link) != "created.bin":
        raise SystemExit("symlink mismatch")
    if os.path.getmtime(os.path.join(root, "y2100.stamp")) != 4102444800:
        raise SystemExit("bigtime 2100 timestamp changed")
    if not os.path.isdir(os.path.join(root, "win/dir")):
        raise SystemExit("missing /win/dir")
    print("Windows mutation semantics OK")
PY

umount "$MOUNT"
echo "RW-6A Linux verification passed: $IMG"
