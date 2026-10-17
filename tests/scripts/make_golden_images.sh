#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - generate golden images with the real Linux mkfs.xfs.
#
#   sudo tests/scripts/make_golden_images.sh [output-dir] [--ci]
#
# --ci creates only the populated image (used by GitHub Actions), with sizes
# taken from the environment:
#   GOLDEN_POPULATED_SIZE (default 1G)
#   GOLDEN_LARGE_MB       (default 64)
#   GOLDEN_SPARSE_SIZE    (default 1G)
#   GOLDEN_BIGDIR_COUNT   (default 5000)
#   GOLDEN_FRAG_COUNT     (default 300)
#
# Requires: xfsprogs (mkfs.xfs, xfs_db), util-linux (mount), python3.

set -euo pipefail

out="${1:-tests/images}"
mode="${2:-full}"
mkdir -p "$out"

for tool in mkfs.xfs xfs_db truncate dd python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool" >&2
        exit 1
    }
done
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

populated_size="${GOLDEN_POPULATED_SIZE:-1G}"
large_mb="${GOLDEN_LARGE_MB:-64}"
sparse_size="${GOLDEN_SPARSE_SIZE:-1G}"
bigdir_count="${GOLDEN_BIGDIR_COUNT:-5000}"
frag_count="${GOLDEN_FRAG_COUNT:-300}"

make_empty() {
    local name="$1"; shift
    local size="$1"; shift
    local img="$out/$name"
    rm -f "$img"
    truncate -s "$size" "$img"
    if ! mkfs.xfs -f "$@" "$img" >/dev/null; then
        echo "mkfs.xfs failed for $name (unsupported options?)" >&2
        rm -f "$img"
        return 1
    fi
    echo "created $img"
}

if [ "$mode" != "--ci" ]; then
    # Plain distro-default filesystem.
    make_empty empty-default.img 300M

    # Minimal feature set (closest to the Windows formatter profile).
    make_empty empty-minimal.img 300M \
        -m finobt=0,rmapbt=0,reflink=0,bigtime=0,nrext64=0 \
        -i sparse=0 || true

    # Optional features the Windows reader supports.
    make_empty empty-bigtime.img 300M -m bigtime=1 || true
    make_empty empty-nrext64.img 300M -m nrext64=1 || true
    make_empty empty-1g.img 1G
fi

if [ "$(id -u)" -ne 0 ]; then
    echo
    echo "not running as root: skipping the populated image."
    echo "re-run as root (or with sudo) to generate populated.img."
    exit 0
fi

img="$out/populated.img"
rm -f "$img"
truncate -s "$populated_size" "$img"

# Explicit, stable feature set that the Windows reader supports. Fall back to
# the distro defaults if this xfsprogs is too old for the option list.
if ! mkfs.xfs -f \
        -m crc=1,finobt=1,rmapbt=1,reflink=1,bigtime=1 \
        -i sparse=1 "$img" >/dev/null 2>"$out/mkfs.err"; then
    echo "explicit feature set rejected, falling back to mkfs defaults" >&2
    cat "$out/mkfs.err" >&2 || true
    mkfs.xfs -f "$img" >/dev/null
fi
rm -f "$out/mkfs.err"

mnt="$(mktemp -d)"
cleanup() {
    if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
    rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT
mount -o loop "$img" "$mnt"

echo "populating $img (mounted at $mnt)"

# DoD fixtures.
printf 'hello from XFS for Windows\n' > "$mnt/hello.txt"
mkdir -p "$mnt/folder/deep/deeper"
head -c 1048576 /dev/urandom > "$mnt/folder/test.bin"
printf 'unicode filename\n' > "$mnt/中文文件名.txt"
printf 'deep\n' > "$mnt/folder/deep/deeper/file.txt"

# Large file.
dd if=/dev/urandom of="$mnt/large.bin" bs=1M count="$large_mb" status=none

# Sparse file with data at both ends.
truncate -s "$sparse_size" "$mnt/sparse.img"
sparse_bytes="$(stat -c %s "$mnt/sparse.img")"
printf 'HEAD' | dd of="$mnt/sparse.img" bs=1 seek=0 conv=notrunc status=none
printf 'TAIL' | dd of="$mnt/sparse.img" bs=1 seek=$((sparse_bytes - 4)) \
    conv=notrunc status=none

# Symlink + hardlink.
ln -s ../hello.txt "$mnt/folder/link"
ln "$mnt/hello.txt" "$mnt/hello-hardlink.txt"

# Large directory (forces leaf/node directory format).
mkdir -p "$mnt/bigdir"
for i in $(seq 1 "$bigdir_count"); do : > "$mnt/bigdir/entry-$i.txt"; done

# Many small files.
mkdir -p "$mnt/fragdir"
for i in $(seq 1 200); do
    printf 'data-%s' "$i" > "$mnt/fragdir/f-$i"
done

# A file with one byte every 8 KiB: forces an extent B+tree.
for i in $(seq 0 $((frag_count - 1))); do
    printf 'X' | dd of="$mnt/fragmented.bin" bs=1 count=1 \
        seek=$((i * 8192)) conv=notrunc status=none
done

sync

python3 "$script_dir/make_manifest.py" "$mnt" "$out/populated.manifest.json"
ls -laR "$mnt" > "$out/populated.ls.txt" 2>&1 || true

umount "$mnt"
trap - EXIT
rmdir "$mnt"

{
    echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "mkfs.xfs: $(mkfs.xfs -V 2>&1 | head -1)"
    echo "image size: $(stat -c %s "$img") bytes"
    echo "== xfs_db superblock =="
    xfs_db -r -c 'sb 0' -c 'p' "$img"
    echo "== xfs_db root inode =="
    rootino="$(xfs_db -r -c 'sb 0' -c 'p rootino' "$img" | awk -F'= ' '{print $2}' | awk '{print $1}')"
    xfs_db -r -c "inode $rootino" -c 'p core' "$img"
} > "$out/golden-summary.txt" 2>&1

echo "created $img"
du -sh "$img" "$out/populated.manifest.json"
