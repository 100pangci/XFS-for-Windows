#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# DEV-1: external Linux XFS oracle for a Windows-written device image.
#
#   sudo tests/scripts/verify_device_linux.sh /path/to/device.vhd [artifacts]
#
# Accepts a fixed VHD (the 512-byte footer is stripped) or a raw image.
# The verification chain is:
#   1. xfs_repair -n
#   2. independent Python raw-image oracle (device_image_probe.py)
#   3. Linux kernel mount, semantic verification of the WinFsp-created files
#   4. xfs_scrub -n while mounted
#   5. clean unmount
#   6. xfs_logprint
#   7. a second xfs_repair -n
#
# The image is copied first; the artifact handed to this script is not
# modified.

set -euo pipefail

src="${1:?usage: verify_device_linux.sh <vhd|image> [artifact-dir]}"
artifacts="${2:-$(mktemp -d)}"
mkdir -p "$artifacts"
src="$(readlink -f "$src")"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"

for tool in xfs_db xfs_repair mount umount truncate dd stat python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool (install xfsprogs/util-linux)" >&2
        exit 1
    }
done

img="$artifacts/device-payload.img"
# A fixed VHD is the raw payload followed by a 512-byte footer whose first
# eight bytes are the "conectix" cookie.
tail_cookie="$(tail -c 512 "$src" | head -c 8 || true)"
if [ "$tail_cookie" = "conectix" ]; then
    size="$(stat -c %s "$src")"
    head -c $((size - 512)) "$src" > "$img"
    echo "stripped the 512-byte fixed-VHD footer ($size -> $((size - 512)) bytes)"
else
    cp "$src" "$img"
fi
ls -l "$img"

echo "== 1. xfs_repair -n (before mount) =="
xfs_repair -n "$img" 2>&1 | tee "$artifacts/xfs-repair-before.log"
if grep -qiE 'corruption|fatal|would fix' "$artifacts/xfs-repair-before.log"; then
    echo "FAILED: xfs_repair -n found problems" >&2
    exit 1
fi

echo "== 2. independent Python raw-image oracle =="
python3 "$repo_root/tests/tools/device_image_probe.py" "$img" \
    --expect-marker "hello from xfswin dev1" \
    2>&1 | tee "$artifacts/device-image-probe.log"

echo "== xfs_db superblock =="
xfs_db -r -c 'sb 0' -c 'p' "$img" 2>&1 | tee "$artifacts/xfs-db-superblock.log" | sed -n '1,45p'
if grep -qiE 'sanity check failed|corruption|crc error' "$artifacts/xfs-db-superblock.log"; then
    echo "FAILED: xfs_db reported superblock problems" >&2
    exit 1
fi

echo "== 3. kernel mount and semantic verification =="
mnt="$(mktemp -d)"
cleanup() {
    if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
    rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT
mount -o loop,rw "$img" "$mnt"

test "$(cat "$mnt/marker.txt")" = "hello from xfswin dev1" || {
    echo "FAILED: marker.txt content mismatch" >&2
    exit 1
}
python3 - "$mnt/dir/data.bin" <<'PY'
import sys
path = sys.argv[1]
with open(path, "rb") as f:
    data = f.read()
assert len(data) == 256 * 1024, f"data.bin length {len(data)} != 262144"
for i, byte in enumerate(data):
    assert byte == ((i * 31) + 7) & 0xFF, f"data.bin mismatch at {i}"
print(f"data.bin OK ({len(data)} bytes)")
PY

# Prove the filesystem is still writable after the Windows pass.
echo "linux-side write" > "$mnt/linux-write.txt"
mkdir "$mnt/linux-dir"
echo "nested" > "$mnt/linux-dir/nested.txt"
ln "$mnt/marker.txt" "$mnt/marker-hardlink.txt"
mv "$mnt/linux-write.txt" "$mnt/linux-renamed.txt"
rm "$mnt/linux-renamed.txt"
rm "$mnt/linux-dir/nested.txt"
rmdir "$mnt/linux-dir"
sync

echo "== 4. xfs_scrub -n =="
if command -v xfs_scrub >/dev/null 2>&1; then
    if ! xfs_scrub -n "$mnt" 2>&1 | tee "$artifacts/xfs-scrub.log"; then
        if [ "${XFSWIN_ALLOW_SKIP_SCRUB:-0}" = "1" ]; then
            echo "warning: xfs_scrub failed (allowed by XFSWIN_ALLOW_SKIP_SCRUB)"
        else
            echo "FAILED: xfs_scrub -n failed" >&2
            exit 1
        fi
    fi
else
    echo "warning: xfs_scrub is not installed; skipping the scrub oracle" | tee "$artifacts/xfs-scrub.log"
    if [ "${XFSWIN_ALLOW_SKIP_SCRUB:-0}" != "1" ]; then
        echo "FAILED: xfs_scrub is required for a complete DEV-1 oracle" >&2
        exit 1
    fi
fi

echo "== 5. clean unmount =="
umount "$mnt"
trap - EXIT
rmdir "$mnt"

echo "== 6. xfs_logprint =="
if command -v xfs_logprint >/dev/null 2>&1; then
    xfs_logprint "$img" 2>&1 | tee "$artifacts/xfs-logprint.log" | tail -40
    if grep -qiE 'bad log|corrupt|error' "$artifacts/xfs-logprint.log"; then
        echo "FAILED: xfs_logprint reported log problems" >&2
        exit 1
    fi
else
    echo "warning: xfs_logprint is not installed" >&2
    if [ "${XFSWIN_ALLOW_SKIP_LOGPRINT:-0}" != "1" ]; then
        echo "FAILED: xfs_logprint is required for a complete DEV-1 oracle" >&2
        exit 1
    fi
fi

echo "== 7. xfs_repair -n (after the Linux writable pass) =="
xfs_repair -n "$img" 2>&1 | tee "$artifacts/xfs-repair-after.log"
if grep -qiE 'corruption|fatal|would fix' "$artifacts/xfs-repair-after.log"; then
    echo "FAILED: the Linux writable pass left repairable damage" >&2
    exit 1
fi

echo "OK: the Windows-written device image passed the Linux XFS oracle"
