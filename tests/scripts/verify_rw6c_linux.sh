#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6C: verify a reflink image with Linux xfs_repair, the independent oracle
# and a read-only kernel mount.
#
# usage: verify_rw6c_linux.sh <image> [--manifest <json>] [--label <text>]
#
# The manifest may be the original Linux fixture manifest or the
# mutated.manifest.json written by rw6c_windows_mutate.py.
set -euo pipefail

IMG=${1:?usage: verify_rw6c_linux.sh <image> [--manifest <json>]}
shift
MANIFEST=""
LABEL=""
while [ $# -gt 0 ]; do
    case "$1" in
        --manifest) MANIFEST=${2:?}; shift 2 ;;
        --label) LABEL=${2:?}; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

REPO=$(cd "$(dirname "$0")/../.." && pwd)
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
for tool in xfs_repair mount python3; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

echo "== RW-6C Linux verify ${LABEL:-$IMG} =="
set +e
xfs_repair -n "$IMG" > "$WORK/repair.txt" 2>&1
RC=$?
set -e
cat "$WORK/repair.txt"
if [ "$RC" -ne 0 ] || grep -qiE \
        'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
        "$WORK/repair.txt"; then
    echo "xfs_repair rejected $IMG" >&2
    exit 1
fi

python3 "$REPO/tests/tools/rw6c_refcount_probe.py" "$IMG" \
    --allow-no-rmapbt --json "$WORK/oracle.json"
python3 - "$WORK/oracle.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1]))
if data.get("result") != "ok":
    raise SystemExit("refcount oracle failed: %s" % data)
print("oracle OK: shared records=%d blocks=%d CoW=%d rmapbt=%s" %
      (data["shared_records"], data["shared_blocks"], data["cow_records"],
       data["rmapbt"]))
PY

mount -o ro,loop "$IMG" "$MOUNT"
python3 - "$MOUNT" "$MANIFEST" <<'PY'
import hashlib
import json
import os
import sys

root = sys.argv[1]
manifest_path = sys.argv[2] if len(sys.argv) > 2 else ""
if not manifest_path:
    print("mounted read-only; no manifest supplied")
    raise SystemExit(0)
manifest = json.load(open(manifest_path))
for rel in manifest.get("deleted", []):
    if os.path.lexists(os.path.join(root, rel)):
        raise SystemExit("%s should have been deleted" % rel)
for rel, want in manifest["files"].items():
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        raise SystemExit("%s is missing" % rel)
    st = os.stat(path)
    if st.st_size != want["size"]:
        raise SystemExit("%s size %d != %d" %
                         (rel, st.st_size, want["size"]))
    with open(path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    if digest != want["sha256"]:
        raise SystemExit("%s content hash mismatch" % rel)
print("mounted content OK: %d files, %d deletions" %
      (len(manifest["files"]), len(manifest.get("deleted", []))))

seeds = manifest.get("seeds", {})
if "shared_pairs" in seeds:
    block = int(manifest.get("block_size", 1024))
    def pattern(size, seed):
        return bytes(((i * 37 + seed * 17 + 11) & 0xFF)
                     for i in range(size))
    pairs = int(seeds["shared_pairs"])
    checked = 0
    for i in range(pairs):
        src = os.path.join(root, "base", "s%03d.bin" % i)
        dst = os.path.join(root, "base", "d%03d.bin" % i)
        if not os.path.isfile(src) or not os.path.isfile(dst):
            # These are removed only by an explicit churn stage; the base
            # fixture and the Windows mutation keep them.
            raise SystemExit("shared pair %d is missing" % i)
        with open(src, "rb") as f:
            a = f.read()
        with open(dst, "rb") as f:
            b = f.read()
        if a != pattern(block, 200 + i) or b != a:
            raise SystemExit("shared pair %d content mismatch" % i)
        if i % 2 == 0:
            q = os.path.join(root, "base", "q%03d.bin" % i)
            if not os.path.isfile(q):
                raise SystemExit("shared triple %d is missing" % i)
            with open(q, "rb") as f:
                if f.read() != a:
                    raise SystemExit("shared triple %d content mismatch" % i)
        checked += 1
    print("shared pair content OK: %d pairs" % checked)
PY
umount "$MOUNT"
echo "RW-6C Linux verify passed: $IMG"
