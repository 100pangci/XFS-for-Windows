#!/usr/bin/env bash
# XFS for Windows - verify an image created by `xfswin format` on Linux.
#
#   sudo tests/scripts/verify_windows_image_linux.sh /path/to/windows.img
#
# Steps:
#   1. xfs_db reads the superblock and root inode ("oracle" dump).
#   2. xfs_repair -n must not report corruption.
#   3. the image must mount read-only through the loop driver.
#   4. a writable copy must accept normal Linux filesystem operations.
#
# Exit code 0 means the image is a valid XFS v5 filesystem.

set -euo pipefail

img="${1:?usage: verify_windows_image_linux.sh <image>}"
img="$(readlink -f "$img")"

for tool in xfs_db xfs_repair mount umount truncate; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool (install xfsprogs/util-linux)" >&2
        exit 1
    }
done

echo "== xfs_db: superblock =="
xfs_db -r -c 'sb 0' -c 'p' "$img" | sed -n '1,45p'

rootino="$(xfs_db -r -c 'sb 0' -c 'p rootino' "$img" | awk -F'= ' '{print $2}' | awk '{print $1}')"
echo "== xfs_db: root inode ($rootino) =="
xfs_db -r -c "inode $rootino" -c 'p core.mode core.size core.nlink core.format' "$img"

echo "== xfs_repair -n =="
if ! xfs_repair -n "$img" > /tmp/xfswin-repair.log 2>&1; then
    cat /tmp/xfswin-repair.log
    echo "FAILED: xfs_repair -n returned an error" >&2
    exit 1
fi
cat /tmp/xfswin-repair.log
if grep -qiE 'corruption|fatal|would fix' /tmp/xfswin-repair.log; then
    echo "FAILED: xfs_repair -n found problems" >&2
    exit 1
fi

echo "== read-only mount =="
mnt="$(mktemp -d)"
cleanup() {
    if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
    rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT
mount -o loop,ro "$img" "$mnt"
ls -la "$mnt"
umount "$mnt"
trap - EXIT
rmdir "$mnt"

echo "== writable copy: create/remove/mkdir/write =="
copy="$(mktemp /tmp/xfswin-copy-XXXXXX.img)"
truncate -s "$(stat -c %s "$img")" "$copy"
dd if="$img" of="$copy" conv=notrunc status=none
mnt="$(mktemp -d)"
cleanup_copy() {
    if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
    rmdir "$mnt" 2>/dev/null || true
    rm -f "$copy"
}
trap cleanup_copy EXIT
mount -o loop "$copy" "$mnt"
echo test > "$mnt/a"
printf 'unicode' > "$mnt/中文文件名.txt"
mkdir -p "$mnt/dir/sub"
dd if=/dev/urandom of="$mnt/dir/big.bin" bs=1M count=16 status=none
ln -s /a "$mnt/link"
ln "$mnt/a" "$mnt/a-hard"
rm "$mnt/a"
sync
umount "$mnt"
trap - EXIT
rmdir "$mnt"
rm -f "$copy"

echo "OK: $img is a valid XFS v5 filesystem"
