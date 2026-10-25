#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6D: create Linux-kernel extended-attribute fixtures for both legal
# reflink profiles.
#
# Produces:
#   rw6d.img          reflink=1, rmapbt=1
#   rw6d-normap.img   reflink=1, rmapbt=0
#
# Each image contains shortform, single-leaf, node and multi-level node (level
# 2) DA trees, remote values, an attribute BMBT and attributes on a directory
# and a symlink.  The independent RW-6D oracle must accept the image and see
# every representation before upload.
#
# usage: make_rw6d_linux.sh <outdir>
set -euo pipefail

OUT=${1:?usage: make_rw6d_linux.sh <outdir>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
SIZE_MB=${SIZE_MB:-512}
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
for tool in mkfs.xfs xfs_info xfs_repair xfs_db losetup python3 truncate; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

PARENT_OPT=""
MKFS_HELP=$(mkfs.xfs 2>&1 || true)
if printf '%s\n' "$MKFS_HELP" | grep -q 'parent='; then
    PARENT_OPT="-n parent=0"
fi

build_one() {
    local name=$1
    local rmap=$2
    local img="$OUT/$name.img"
    rm -f "$img" "$img.manifest.json" "$img.oracle.json" \
        "$OUT/$name.xfs_db_attr.txt"
    truncate -s "${SIZE_MB}M" "$img"
    LOOP=$(losetup --find --show "$img")
    mkfs.xfs -f -q -b size=4096 -d agcount=2 \
        -m crc=1,finobt=1,inobtcount=1,bigtime=1,reflink=1,rmapbt="$rmap" \
        -i sparse=1,nrext64=1 $PARENT_OPT "$LOOP"
    mount "$LOOP" "$MOUNT"
    xfs_info "$MOUNT" > "$OUT/$name.xfs_info.txt"
    python3 "$REPO/tests/tools/rw6d_linux_content.py" create "$MOUNT"
    python3 "$REPO/tests/tools/rw6d_linux_content.py" create-manifest \
        "$MOUNT" "$OUT/$name.manifest.json"
    sync
    umount "$MOUNT"
    losetup -d "$LOOP"
    LOOP=""

    xfs_repair -n "$img" > "$OUT/$name.repair.txt" 2>&1
    if grep -Eiq \
            'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
            "$OUT/$name.repair.txt"; then
        echo "xfs_repair rejected $img" >&2
        cat "$OUT/$name.repair.txt" >&2
        exit 1
    fi

    python3 - "$OUT/$name.manifest.json" "$OUT/$name.inodes" <<'PY'
import json
import sys
manifest = json.load(open(sys.argv[1]))
with open(sys.argv[2], "w") as f:
    for item in manifest["inodes"]:
        f.write(str(item["ino"]) + "\n")
print("linux fixture: %d inodes, %d files" %
      (len(manifest["inodes"]), len(manifest.get("files", {}))))
PY
    local inode_args=()
    while read -r ino; do
        inode_args+=(--inode "$ino")
    done < "$OUT/$name.inodes"
    python3 "$REPO/tests/tools/rw6d_xattr_probe.py" "$img" \
        --manifest "$OUT/$name.manifest.json" \
        "${inode_args[@]}" --json "$OUT/$name.oracle.json" --quiet

    python3 - "$OUT/$name.oracle.json" "$name" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1]))
if "inodes" not in data:
    raise SystemExit("%s oracle failed: %s" % (sys.argv[2], data))
inodes = data["inodes"]
formats = {item["format"] for item in inodes}
if not {"shortform", "leaf", "node"} <= formats:
    raise SystemExit("%s is missing an attribute representation: %s" %
                     (sys.argv[2], sorted(formats)))
if not any(item["format"] == "node" and item["root_level"] >= 2
           for item in inodes):
    raise SystemExit("%s never reached a multi-level DA node" % sys.argv[2])
if not any(item["aformat"] == 3 for item in inodes):
    raise SystemExit("%s has no attribute BMBT" % sys.argv[2])
if not any(item["remote_blocks"] > 0 for item in inodes):
    raise SystemExit("%s has no remote attribute value" % sys.argv[2])
print("%s: formats=%s remote=%d attr-bmbt=%s max-level=%d" %
      (sys.argv[2], ",".join(sorted(formats)),
       sum(item["remote_blocks"] for item in inodes),
       any(item["aformat"] == 3 for item in inodes),
       max(item["root_level"] for item in inodes)))
PY

    # Keep a raw xfs_db view for the audit trail.  The oracle above is the
    # independent byte-level check; xfs_db is the reference tool's view.
    while read -r ino; do
        xfs_db -r -c "inode $ino" \
            -c 'p core.forkoff core.aformat core.anextents' "$img" \
            >> "$OUT/$name.xfs_db_attr.txt" 2>&1 || true
    done < "$OUT/$name.inodes"
    echo "wrote $img"
}

build_one rw6d 1
build_one rw6d-normap 0
