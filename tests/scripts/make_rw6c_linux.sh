#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6C: create Linux-kernel reflink fixtures for both legal profiles.
#
# Produces:
#   rw6c.img          reflink=1, rmapbt=1
#   rw6c-normap.img   reflink=1, rmapbt=0
#
# Both images contain a deep clone tree (three-way clones, a CoW-modified
# clone, holes, a partial EOF block and 100 separate shared extents that make
# the refcount btree at least two levels deep).  The independent RW-6C oracle
# must accept each image before it is uploaded.
#
# usage: make_rw6c_linux.sh <outdir>
set -euo pipefail

OUT=${1:?usage: make_rw6c_linux.sh <outdir>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
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
for tool in mkfs.xfs xfs_info xfs_repair xfs_db losetup python3 cp; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

PARENT_OPT=""
MKFS_HELP=$(mkfs.xfs 2>&1 || true)
if printf '%s\n' "$MKFS_HELP" | grep -q 'parent='; then
    PARENT_OPT="-n parent=0"
fi

write_content() {
    python3 - "$1" "$2" <<'PY'
import hashlib
import json
import os
import subprocess
import sys

root, manifest_path = sys.argv[1], sys.argv[2]
block = 1024


def pattern(size, seed):
    return bytes(((i * 37 + seed * 17 + 11) & 0xFF) for i in range(size))


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def patch(path, offset, data):
    with open(path, "r+b") as f:
        f.seek(offset)
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def clone(src, dst):
    subprocess.run(["cp", "--reflink=always", src, dst], check=True)


os.makedirs(os.path.join(root, "base"), exist_ok=True)
os.makedirs(os.path.join(root, "emptydir"), exist_ok=True)
base = os.path.join(root, "base")

source_data = pattern(16 * block, 11)
source = os.path.join(base, "source.bin")
write(source, source_data)
clone(source, os.path.join(base, "clone-a.bin"))
clone(source, os.path.join(base, "clone-b.bin"))
clone(source, os.path.join(base, "cow.bin"))
patch(os.path.join(base, "cow.bin"), 2 * block, pattern(block, 73))
clone(source, os.path.join(base, "three.bin"))

partial_data = pattern(4 * block + 300, 29)
partial = os.path.join(base, "partial.bin")
write(partial, partial_data)
clone(partial, os.path.join(base, "partial-clone.bin"))

sparse = os.path.join(base, "sparse.bin")
with open(sparse, "wb") as f:
    f.seek(2 * block)
    f.write(pattern(2 * block, 41))
    f.flush()
    os.fsync(f.fileno())
clone(sparse, os.path.join(base, "sparse-clone.bin"))

# 100 shared 1-block extents separated by private gap blocks.  At 1 KiB the
# refcount leaf holds 80 records, so this forces a non-root refcount block.
pairs = 100
for i in range(pairs):
    src = os.path.join(base, "s%03d.bin" % i)
    gap = os.path.join(base, "g%03d.bin" % i)
    dst = os.path.join(base, "d%03d.bin" % i)
    write(src, pattern(block, 200 + i))
    write(gap, pattern(block, 500 + i))
    clone(src, dst)
    # Alternate refcounts as well as physical spacing, so adjacent allocator
    # runs can never merge two test records accidentally.
    if i % 2 == 0:
        clone(src, os.path.join(base, "q%03d.bin" % i))

entries = {}
for dirpath, dirnames, filenames in os.walk(root):
    dirnames.sort()
    for name in sorted(filenames):
        path = os.path.join(dirpath, name)
        rel = os.path.relpath(path, root)
        with open(path, "rb") as f:
            data = f.read()
        entries[rel] = {
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
manifest = {
    "block_size": block,
    "pattern": "byte(i) = (i*37 + seed*17 + 11) & 0xff",
    "files": entries,
    "seeds": {
        "source": 11,
        "cow_patch": 73,
        "partial": 29,
        "sparse": 41,
        "shared_pairs": pairs,
    },
}
with open(manifest_path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2, sort_keys=True)
print("fixture: %d files, source=%d bytes, pairs=%d" %
      (len(entries), len(source_data), pairs))
PY
}

build_one() {
    local name=$1
    local rmap=$2
    local img="$OUT/$name.img"
    rm -f "$img"
    truncate -s "${SIZE_MB}M" "$img"
    LOOP=$(losetup --find --show "$img")
    # The writable profile requires one filesystem block per directory block
    # (xfs_win policy); mkfs.xfs would otherwise pick a 4 KiB directory block
    # for this 1 KiB filesystem.
    local mkfs_opts="-b size=1024 -n size=1024 -m crc=1,finobt=1,inobtcount=1,bigtime=1,reflink=1,rmapbt=$rmap -i sparse=1,nrext64=1"
    mkfs.xfs -f -q $mkfs_opts $PARENT_OPT "$LOOP"
    mount "$LOOP" "$MOUNT"
    xfs_info "$MOUNT" > "$OUT/$name.xfs_info.txt"
    write_content "$MOUNT" "$OUT/$name.manifest.json"
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
    python3 "$REPO/tests/tools/rw6c_refcount_probe.py" "$img" \
        --allow-no-rmapbt --json "$OUT/$name.oracle.json" \
        > "$OUT/$name.oracle.txt"
    python3 - "$OUT/$name.oracle.json" "$name" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1]))
if data.get("result") != "ok":
    raise SystemExit("%s oracle failed: %s" % (sys.argv[2], data))
levels = [ag["level"] for ag in data["ags"]]
if max(levels) < 2:
    raise SystemExit("%s refcount tree never reached level 2" % sys.argv[2])
if data["shared_records"] < 100:
    raise SystemExit("%s has only %d shared records" %
                     (sys.argv[2], data["shared_records"]))
print("%s: shared=%d blocks=%d max-refcount-level=%d" %
      (sys.argv[2], data["shared_records"], data["shared_blocks"],
       max(levels)))
PY
    echo "wrote $img"
}

build_one rw6c 1
build_one rw6c-normap 0
