#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6A reverse interoperability: after Windows has mutated the image, let the
# Linux kernel allocate/free inodes, rename, truncate and unlink again. The
# Windows verifier then re-reads the result and runs the independent oracle.
set -euo pipefail

IMG=${1:?usage: verify_rw6_linux_reverse.sh <image>}
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
import sys

root = sys.argv[1]


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


if not os.path.isfile(os.path.join(root, "win", "created.bin")):
    raise SystemExit("Windows mutation is missing")
os.makedirs(os.path.join(root, "linux"), exist_ok=True)
write(os.path.join(root, "linux", "reverse.bin"), b"linux-reverse")
os.rename(os.path.join(root, "win", "created.bin"),
          os.path.join(root, "win", "created-renamed.bin"))
os.unlink(os.path.join(root, "win", "hardlink.bin"))
with open(os.path.join(root, "win", "dir", "nested.bin"), "r+b") as f:
    f.truncate(4096)
    f.flush()
    os.fsync(f.fileno())
os.symlink("reverse.bin", os.path.join(root, "linux", "link"))
# Allocate and free a few hundred inodes through the kernel allocator.
for i in range(400):
    write(os.path.join(root, "linux", "churn%03d" % i), b"z")
for i in range(0, 400, 2):
    os.unlink(os.path.join(root, "linux", "churn%03d" % i))
for i in range(400, 520):
    write(os.path.join(root, "linux", "churn%03d" % i), b"q")
for name in ("dir", "linux", "win", "churn"):
    try:
        fd = os.open(os.path.join(root, name), os.O_RDONLY)
        os.fsync(fd)
        os.close(fd)
    except OSError:
        pass
PY
sync
umount "$MOUNT"
xfs_repair -n "$IMG" > "$WORK/repair.txt" 2>&1 || {
    cat "$WORK/repair.txt" >&2
    exit 1
}
if grep -qiE \
        'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
        "$WORK/repair.txt"; then
    cat "$WORK/repair.txt" >&2
    exit 1
fi
echo "RW-6A Linux reverse mutation OK: $IMG"
