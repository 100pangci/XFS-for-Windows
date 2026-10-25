#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6C reverse interoperability: after Windows has mutated a Linux reflink
# image, let the Linux kernel clone, CoW, truncate and unlink in that image.
#
# usage: verify_rw6c_linux_reverse.sh <image> <manifest-out.json>
set -euo pipefail

IMG=${1:?usage: verify_rw6c_linux_reverse.sh <image> <manifest-out.json>}
OUT=${2:?usage: verify_rw6c_linux_reverse.sh <image> <manifest-out.json>}
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

mount -o loop "$IMG" "$MOUNT"
python3 - "$MOUNT" <<'PY'
import os
import subprocess
import sys

root = sys.argv[1]
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


os.makedirs(os.path.join(root, "linux"), exist_ok=True)
source = os.path.join(root, "base", "source.bin")
after = os.path.join(root, "linux", "after.bin")
clone(source, after)
patch(after, 3 * block, pattern(block, 903))
clone(os.path.join(root, "rw6c", "new.bin"),
      os.path.join(root, "linux", "new-clone.bin"))
clone(source, os.path.join(root, "linux", "source-clone.bin"))
patch(os.path.join(root, "base", "cow.bin"), 0, pattern(block, 904))
os.truncate(os.path.join(root, "rw6c", "new-clone.bin"), block)
os.unlink(os.path.join(root, "rw6c", "three-clone.bin"))
# A 1 KiB file clone exercises the smallest possible shared extent.
write(os.path.join(root, "linux", "tiny.bin"), pattern(block, 905))
clone(os.path.join(root, "linux", "tiny.bin"),
      os.path.join(root, "linux", "tiny-clone.bin"))
for dirpath, dirnames, filenames in os.walk(root):
    for name in filenames:
        try:
            fd = os.open(os.path.join(dirpath, name), os.O_RDONLY)
            os.fsync(fd)
            os.close(fd)
        except OSError:
            pass
PY
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
    echo "xfs_repair rejected the Linux-reverse image" >&2
    exit 1
fi
python3 "$REPO/tests/tools/rw6c_refcount_probe.py" "$IMG" \
    --allow-no-rmapbt --json "$WORK/oracle.json"
python3 - "$WORK/oracle.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1]))
if data.get("result") != "ok":
    raise SystemExit("refcount oracle failed after Linux reverse: %s" % data)
print("Linux reverse oracle OK: shared=%d CoW=%d" %
      (data["shared_records"], data["cow_records"]))
PY

python3 - "$OUT" <<'PY'
import hashlib
import json
import sys

block = 1024


def pattern(size, seed):
    return bytes(((i * 37 + seed * 17 + 11) & 0xFF) for i in range(size))


old_source = pattern(16 * block, 11)
patch = pattern(block, 900)
patched_source = patch + old_source[block:]
cow_patch = pattern(block, 73)
new_data = pattern(2 * block, 901)
partial = pattern(4 * block + 300, 29)
after = (patched_source[:3 * block] + pattern(block, 903) +
         patched_source[4 * block:])
cow = pattern(block, 904) + old_source[block:2 * block] + cow_patch + \
    old_source[3 * block:]
new_clone = new_data[:block]
tiny = pattern(block, 905)

expected = {
    "base/source.bin": patched_source,
    "base/clone-a.bin": old_source[:block],
    "base/cow.bin": cow,
    "base/three.bin": old_source,
    "base/partial.bin": partial,
    "base/partial-clone.bin": partial,
    "rw6c/from-linux-src.bin": patched_source,
    "rw6c/new.bin": new_data,
    "rw6c/new-clone.bin": new_clone,
    "rw6c/hole-clone.bin": partial + bytes(100),
    "linux/after.bin": after,
    "linux/new-clone.bin": new_data,
    "linux/source-clone.bin": patched_source,
    "linux/tiny.bin": tiny,
    "linux/tiny-clone.bin": tiny,
}
for i in range(100):
    data = pattern(block, 200 + i)
    expected["base/s%03d.bin" % i] = data
    expected["base/d%03d.bin" % i] = data
    expected["base/g%03d.bin" % i] = pattern(block, 500 + i)
    if i % 2 == 0:
        expected["base/q%03d.bin" % i] = data
manifest = {
    "kind": "rw6c-linux-reverse",
    "deleted": ["base/clone-b.bin", "rw6c/three-clone.bin"],
    "block_size": block,
    "seeds": {"shared_pairs": 100},
    "files": {
        name: {"size": len(data),
               "sha256": hashlib.sha256(data).hexdigest()}
        for name, data in expected.items()
    },
}
with open(sys.argv[1], "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2, sort_keys=True)
print("wrote %s with %d expected files" % (sys.argv[1], len(expected)))
PY
echo "RW-6C Linux reverse mutation OK: $IMG"
