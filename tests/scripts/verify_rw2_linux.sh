#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - Linux verification of the RW-2 round trip.
#
#   sudo tests/scripts/verify_rw2_linux.sh <dir>
#
# <dir> holds images produced by tests/scripts/verify_rw2_windows.ps1:
#   fixture.img     the pristine fixture (one 32 KiB file)
#   extend.img      fixture after `xfswin extend /hello.bin 45000`
#   truncate.img    fixture after `xfswin truncate /hello.bin 20000`
#   reuse.img       fixture after truncate to 0 and a 3-block write
#   btree.img       fixture after 30 sparse one-block writes (btree format)
#   manifest.json   sizes and inode number
#
# Every image must pass:
#   1. xfs_repair -n (no corruption),
#   2. a kernel mount (replays whatever the writer left, then verifies the
#      content through the kernel),
#   3. the independent Python oracle (rw2_probe.py) on the on-disk image,
#   4. after the kernel's clean unmount, a clean log according to our parser.
set -euo pipefail

dir="${1:?usage: verify_rw2_linux.sh <dir>}"
dir="$(readlink -f "$dir")"
for tool in mount umount xfs_repair python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool (install xfsprogs/util-linux)" >&2
        exit 1
    }
done
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools_dir="$(cd "$script_dir/../tools" && pwd)"
manifest="$dir/manifest.json"
ino="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["file_ino"])' "$manifest")"
name="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["file_name"])' "$manifest")"

failures=0
work="$(mktemp -d)"
mnt="$(mktemp -d)"
cleanup() {
    if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
    rm -rf "$work" "$mnt"
}
trap cleanup EXIT

check_image() {
    local img="$1"
    shift
    local extra=("$@")
    cp "$img" "$work/current.img"
    echo "== $(basename "$img") =="
    if ! python3 "$tools_dir/rw2_probe.py" "$work/current.img" \
            --inode "$ino" "${extra[@]}"; then
        echo "FAILED: oracle rejected $(basename "$img")" >&2
        failures=1
        return
    fi
    local repair_log
    repair_log="$(mktemp)"
    if ! xfs_repair -n "$work/current.img" > "$repair_log" 2>&1; then
        cat "$repair_log"
        echo "FAILED: xfs_repair -n returned an error for $(basename "$img")" >&2
        failures=1
    elif grep -qiE 'corruption|fatal|would fix|ALERT|valuable metadata' \
            "$repair_log"; then
        cat "$repair_log"
        echo "FAILED: xfs_repair -n found problems in $(basename "$img")" >&2
        failures=1
    else
        tail -1 "$repair_log"
    fi
    rm -f "$repair_log"
    # Mount (the kernel replays any dirty log), verify through the kernel, then
    # unmount: the kernel writes its own clean log record.
    if ! mount -o loop,nouuid "$work/current.img" "$mnt"; then
        echo "FAILED: the kernel could not mount $(basename "$img")" >&2
        dmesg | tail -40 || true
        failures=1
        return
    fi
    if ! python3 "$tools_dir/rw2_content.py" "$mnt/$name" "$manifest" \
            "$(basename "$img")"; then
        echo "FAILED: content verification failed for $(basename "$img")" >&2
        failures=1
    fi
    umount "$mnt"
    if ! python3 "$tools_dir/log_probe.py" "$work/current.img" --expect clean \
            >/dev/null; then
        echo "FAILED: no clean log after the kernel unmounted $(basename "$img")" \
            >&2
        failures=1
    fi
}

# 1. Extend: the file grew from 32 KiB to 45000 bytes. The first 32 KiB are
#    the original fixture pattern, the rest is the extend fill pattern
#    byte(i) = ((i * 131) + 7) & 0xFF.
check_image "$dir/extend.img" --expect-size 45000

# 2. Truncate: 20000 bytes left, the first 20000 bytes unchanged.
check_image "$dir/truncate.img" --expect-size 20000

# 3. Reuse: truncate to zero freed the original extent, the 3-block write
#    reallocated blocks from the freed run. Content is the 0x5A fill.
check_image "$dir/reuse.img" --expect-size 12288

# 4. Btree: 31 extents in the data fork (30 sparse one-block writes).
check_image "$dir/btree.img"

# 5. Reverse interoperability: the kernel appends to the Windows-extended
#    file. The Windows job downloads the image and reads it back with xfswin.
cp "$dir/extend.img" "$work/reverse.img"
if ! mount -o loop,nouuid "$work/reverse.img" "$mnt"; then
    echo "FAILED: cannot mount the extend image for the reverse test" >&2
    failures=1
else
    printf 'LINUX-AFTER-WINDOWS' >> "$mnt/$name"
    sync
    umount "$mnt"
    cp "$work/reverse.img" "$dir/reverse.img"
    if ! xfs_repair -n "$work/reverse.img" >/dev/null 2>&1; then
        echo "FAILED: xfs_repair after the Linux append" >&2
        failures=1
    fi
    if ! python3 "$tools_dir/rw2_reverse_check.py" "$dir/reverse.img" "$ino"; then
        echo "FAILED: reverse interoperability check" >&2
        failures=1
    fi
fi

check_image "$dir/reverse.img" --expect-size 45019

if [ "$failures" != "0" ]; then
    echo "RW-2 LINUX VERIFICATION FAILED" >&2
    exit 1
fi
echo "OK: every RW-2 image mounted, replayed and verified by Linux"
