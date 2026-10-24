#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B final acceptance on Linux after the Windows -> Linux -> Windows round
# trip:
#
#   * xfs_repair -n
#   * original Linux fixture manifest, unchanged
#   * the final Windows expected state (bytes/nlink/symlink targets)
#   * independent ownership oracle
#   * one last Linux mutation pass (new allocations/frees/rename/unlink), then
#     clean unmount, xfs_repair -n, oracle and xfs_db layout dump
#
# usage: verify_rw6b_final_linux.sh <image> <MANIFEST.sha256> \
#            <round2.expected.json> <outdir>
set -euo pipefail

IMG=${1:?usage: verify_rw6b_final_linux.sh <image> <manifest> <expected.json> <outdir>}
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
    local out="$OUT/rw6b-final.${label}.repair.txt"
    set +e
    xfs_repair -n "$IMG" > "$out" 2>&1
    local rc=$?
    set -e
    cat "$out"
    if [ "$rc" -ne 0 ] || grep -qiE \
            'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
            "$out"; then
        echo "xfs_repair -n rejected the final image ($label)" >&2
        exit 1
    fi
}

dump_layout() {
    local label=$1
    local out="$OUT/rw6b-final.${label}.layout.txt"
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
        echo "== selected AG0 rmap records =="
        xfs_db -r -c 'agf 0' -c 'addr rmaproot' -c 'p keys[1-10]' \
            "$IMG" || true
    } > "$out" 2>&1
    cat "$out"
}

echo "== xfs_repair -n (final, before mount) =="
repair_check before
echo "== xfs_logprint (final) =="
xfs_logprint -t "$IMG" > "$OUT/rw6b-final.logprint.txt" 2>&1 || true

echo "== independent ownership oracle (before final Linux mutation) =="
python3 "$REPO/tests/tools/rw6b_rmap_probe.py" "$IMG"

echo "== mount read-only and verify final namespace/content =="
mount -o ro,loop "$IMG" "$MOUNT"
python3 "$REPO/tests/tools/rw6b_verify_manifest.py" "$MOUNT" "$MANIFEST" \
    "$EXPECTED"
umount "$MOUNT"

# One last kernel mutation on the image that has now crossed Linux and Windows
# several times.  It must not leave any ownership drift.
mount -o loop "$IMG" "$MOUNT"
python3 - "$MOUNT" <<'PY'
import os
import sys

root = sys.argv[1]
final = os.path.join(root, "linux-final")
os.makedirs(final, exist_ok=True)
for i in range(300):
    path = os.path.join(final, "f%04d.bin" % i)
    with open(path, "wb") as f:
        f.write((b"F%07d" % i) * 128)
        f.flush()
        os.fsync(f.fileno())
for i in range(0, 300, 2):
    os.unlink(os.path.join(final, "f%04d.bin" % i))
with open(os.path.join(final, "source.bin"), "wb") as f:
    f.write(b"FINAL")
    f.flush()
    os.fsync(f.fileno())
os.link(os.path.join(final, "source.bin"), os.path.join(final, "hard.bin"))
os.symlink("source.bin", os.path.join(final, "inline.link"))
os.rename(os.path.join(root, "qual", "r2.bin"),
          os.path.join(final, "r2-from-linux.bin"))
fd = os.open(final, os.O_RDONLY)
try:
    os.fsync(fd)
finally:
    os.close(fd)
PY
sync
umount "$MOUNT"

echo "== xfs_repair -n (final, after Linux mutation) =="
repair_check after
echo "== independent ownership oracle (after final Linux mutation) =="
python3 "$REPO/tests/tools/rw6b_rmap_probe.py" "$IMG"
echo "== xfs_db layout (final) =="
dump_layout final

echo "RW-6B final Linux acceptance passed: $IMG"
