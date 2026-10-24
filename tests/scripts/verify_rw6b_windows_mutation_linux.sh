#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B: Linux verifies an image the Windows writer mutated.  Requirements:
#
#   * xfs_repair -n before the first mount
#   * every original Linux fixture entry unchanged (size/mtime/hash/link)
#   * every Windows mutation expected entry present with the exact bytes,
#     nlink and symlink target
#   * the independent forward-derived ownership oracle
#   * xfs_db AGF/rmap layout dump for the CI artifact
#   * a Linux kernel mount, mutation pass, clean unmount and a second repair
#
# usage:
#   verify_rw6b_windows_mutation_linux.sh <image> <MANIFEST.sha256> \
#       <expected.json> <outdir>
set -euo pipefail

IMG=${1:?usage: verify_rw6b_windows_mutation_linux.sh <image> <manifest> <expected.json> <outdir>}
MANIFEST=${2:?missing manifest}
EXPECTED=${3:?missing expected.json}
OUT=${4:?missing output directory}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
MOUNT="$WORK/mnt"
mkdir -p "$MOUNT" "$OUT"

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
for tool in xfs_repair xfs_db xfs_logprint xfs_info mount python3; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

repair_check() {
    local label=$1
    local out="$OUT/rw6b.${label}.repair.txt"
    set +e
    xfs_repair -n "$IMG" > "$out" 2>&1
    local rc=$?
    set -e
    cat "$out"
    if [ "$rc" -ne 0 ] || grep -qiE \
            'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
            "$out"; then
        echo "xfs_repair -n rejected the image ($label)" >&2
        exit 1
    fi
}

dump_layout() {
    local label=$1
    local out="$OUT/rw6b.${label}.layout.txt"
    {
        echo "== xfs_info =="
        xfs_info "$IMG" || true
        for ag in 0 1 2 3; do
            echo "== AG$ag AGF =="
            xfs_db -r -c "agf $ag" \
                -c 'p rmaproot rmaplevel rmapblocks btreeblks freeblks longest flcount flfirst fllast' \
                "$IMG" || true
        done
        echo "== AG0 rmap root =="
        xfs_db -r -c 'agf 0' -c 'addr rmaproot' -c 'p' "$IMG" || true
        echo "== AG0 bnobt/cntbt roots =="
        xfs_db -r -c 'agf 0' -c 'p bnoroot cntroot bnolevel cntlevel' \
            "$IMG" || true
        echo "== selected rmap records (AG0) =="
        xfs_db -r -c 'agf 0' -c 'addr rmaproot' -c 'p keys[1-10]' \
            "$IMG" || true
    } > "$out" 2>&1
    cat "$out"
}

echo "== xfs_repair -n (before mount) =="
repair_check before

echo "== xfs_logprint =="
xfs_logprint -t "$IMG" > "$OUT/rw6b.logprint.txt" 2>&1 || true

echo "== independent ownership oracle (before Linux mutation) =="
python3 "$REPO/tests/tools/rw6b_rmap_probe.py" "$IMG"

echo "== mount read-only and verify namespace/content =="
mount -o ro,loop "$IMG" "$MOUNT"
python3 "$REPO/tests/tools/rw6b_verify_manifest.py" "$MOUNT" "$MANIFEST" \
    "$EXPECTED"
umount "$MOUNT"

echo "== xfs_db layout =="
dump_layout before-linux-mutation

# ---------------------------------------------------------------------------
# Linux mutates the Windows-mutated image once more.  The Windows reverse job
# consumes the resulting image and checks these exact paths.
# ---------------------------------------------------------------------------
echo "== Linux mutation pass =="
mount -o loop "$IMG" "$MOUNT"
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


def append(path, data):
    with open(path, "ab") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def fsync_dir(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


linux = os.path.join(root, "linux-after")
os.makedirs(linux, exist_ok=True)

# Fragmented file: many rmap EXTENT records in one AG.
frag = os.path.join(linux, "frag.bin")
with open(frag, "wb") as f:
    for i in range(300):
        f.seek(i * 2 * block)
        f.write((b"L%07d" % i) * (block // 8))
    f.flush()
    os.fsync(f.fileno())
fsync_dir(linux)

# Overwrite the first block of a Windows file: new data, same rmap owner.
with open(os.path.join(root, "qual", "create.bin"), "r+b") as f:
    f.write(b"LINUX-OVERWRITE")
    f.flush()
    os.fsync(f.fileno())

# Grow a Windows preallocated file with a hole.
with open(os.path.join(root, "qual", "unwritten.bin"), "r+b") as f:
    f.truncate(2 << 20)
    f.flush()
    os.fsync(f.fileno())

# Namespace churn: replace rename, rename, hardlink, symlink, unlink.
with open(os.path.join(linux, "source.bin"), "wb") as f:
    f.write(b"LINUX-SOURCE")
    f.flush()
    os.fsync(f.fileno())
os.rename(os.path.join(root, "qual", "repl-dst"),
          os.path.join(root, "qual", "repl-linux"))
os.link(os.path.join(linux, "source.bin"), os.path.join(linux, "hard.bin"))
os.symlink("source.bin", os.path.join(linux, "inline.link"))
os.symlink("../" + ("y/" * 120), os.path.join(linux, "remote.link"))
os.unlink(os.path.join(root, "qual", "b", "x.bin"))
fsync_dir(linux)

# Directory growth/shrink and inode allocation/free.
grow = os.path.join(linux, "grow")
os.makedirs(grow, exist_ok=True)
for i in range(500):
    write(os.path.join(grow, "n%04d.bin" % i), b"n%04d" % i)
fsync_dir(grow)
for i in range(0, 400):
    os.unlink(os.path.join(grow, "n%04d.bin" % i))
for i in range(2000):
    path = os.path.join(linux, "i%04d.bin" % i)
    write(path, b"i")
for i in range(0, 2000, 2):
    os.unlink(os.path.join(linux, "i%04d.bin" % i))
for i in range(2000, 2600):
    write(os.path.join(linux, "i%04d.bin" % i), b"r")
for i in range(2000, 2600):
    os.unlink(os.path.join(linux, "i%04d.bin" % i))
fsync_dir(linux)

# Free the fragmented file so a later Windows pass reuses its blocks.
os.unlink(frag)
fsync_dir(linux)

# Delete every link of the hardlinked source except one, then one more.
os.unlink(os.path.join(linux, "hard.bin"))
fsync_dir(linux)

manifest = os.path.join(linux, "LINUX-MUTATION.sha256")
with open(manifest, "w", encoding="utf-8") as out:
    for base, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            rel = os.path.relpath(path, root)
            if rel.startswith("linux-after/LINUX-MUTATION.sha256"):
                continue
            if os.path.islink(path):
                out.write("link %s %s\n" % (rel, os.readlink(path)))
                continue
            with open(path, "rb") as f:
                digest = hashlib.sha256(f.read()).hexdigest()
            out.write("file %s %d %s\n" % (rel, os.path.getsize(path), digest))
    out.flush()
    os.fsync(out.fileno())
fsync_dir(linux)
PY
sync
umount "$MOUNT"

echo "== xfs_repair -n (after Linux mutation) =="
repair_check after

echo "== independent ownership oracle (after Linux mutation) =="
python3 "$REPO/tests/tools/rw6b_rmap_probe.py" "$IMG"

echo "== xfs_db layout (after Linux mutation) =="
dump_layout after-linux-mutation

echo "RW-6B Linux verification of the Windows mutation passed: $IMG"
