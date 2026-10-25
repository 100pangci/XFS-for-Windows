#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6D: Linux reverse mutation of a Windows-mutated xattr image.
#
# usage: verify_rw6d_linux_reverse.sh <image> <in-manifest> <out-manifest>
set -euo pipefail

IMG=${1:?usage: verify_rw6d_linux_reverse.sh <image> <in> <out>}
IN_MANIFEST=${2:?}
OUT_MANIFEST=${3:?}
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
for tool in mount xfs_repair python3; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

mount -o loop "$IMG" "$MOUNT"
python3 "$REPO/tests/tools/rw6d_linux_content.py" reverse \
    "$MOUNT" "$IN_MANIFEST" "$OUT_MANIFEST"
sync
umount "$MOUNT"

set +e
xfs_repair -n "$IMG" > "$WORK/repair.txt" 2>&1
RC=$?
set -e
cat "$WORK/repair.txt"
if [ "$RC" -ne 0 ] || grep -qiE \
        'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
        "$WORK/repair.txt"; then
    echo "xfs_repair rejected the Linux reverse image" >&2
    exit 1
fi

mapfile -t INODES < <(python3 - "$OUT_MANIFEST" <<'PY'
import json
import sys
for item in json.load(open(sys.argv[1]))["inodes"]:
    print(item["ino"])
PY
)
INODE_ARGS=()
for ino in "${INODES[@]}"; do
    INODE_ARGS+=(--inode "$ino")
done
python3 "$REPO/tests/tools/rw6d_xattr_probe.py" "$IMG" \
    --manifest "$OUT_MANIFEST" "${INODE_ARGS[@]}" \
    --json "$WORK/oracle.json" --quiet
python3 - "$WORK/oracle.json" <<'PY'
import json
import sys
inodes = json.load(open(sys.argv[1]))["inodes"]
if not any(item["format"] == "node" and item["root_level"] >= 2
           for item in inodes):
    raise SystemExit("Linux reverse lost the multi-level DA tree")
if not any(item["aformat"] == 3 for item in inodes):
    raise SystemExit("Linux reverse lost the attribute BMBT")
if not any(item["remote_blocks"] > 0 for item in inodes):
    raise SystemExit("Linux reverse lost remote values")
print("Linux reverse representations OK: max-level=%d" %
      max(item["root_level"] for item in inodes))
PY
echo "RW-6D Linux reverse passed: $IMG"
