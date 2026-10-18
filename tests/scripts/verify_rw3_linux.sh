#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - Linux verification of the RW-3 round trip.
#
#   sudo tests/scripts/verify_rw3_linux.sh <dir>
#
# <dir> holds the images produced by tests/scripts/verify_rw3_windows.ps1
# (create, rm, leafchurn, nodedir) and manifest.json.
#
# Every image must pass:
#   1. the independent structural oracle (rw3_probe.py: inobt/AGI, directory
#      formats, bestfree tables, da btree),
#   2. xfs_repair -n (no corruption),
#   3. a kernel mount (replays whatever the writer left) and the mounted state
#      must match the manifest exactly (names, types, sizes, contents),
#   4. after the kernel's clean unmount, a clean log according to our parser.
#
# Then the kernel modifies the image the other way (creates, writes and removes
# files inside a Windows-created directory); the result is checked again and
# saved as reverse.img for the Windows reverse job.
set -euo pipefail

dir="${1:?usage: verify_rw3_linux.sh <dir>}"
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
images="$(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1]))["images"].keys()))' "$manifest")"

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
    echo "== $(basename "$img") =="
    if ! python3 "$tools_dir/rw3_probe.py" "$img" --quiet; then
        echo "FAILED: oracle rejected $(basename "$img")" >&2
        failures=1
        return
    fi
    local repair_log
    repair_log="$(mktemp)"
    if ! xfs_repair -n "$img" > "$repair_log" 2>&1; then
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
    if ! mount -o loop,nouuid "$img" "$mnt"; then
        echo "FAILED: the kernel could not mount $(basename "$img")" >&2
        dmesg | tail -40 || true
        failures=1
        return
    fi
    if ! python3 "$tools_dir/rw3_content.py" check-mount "$mnt" "$manifest" \
            "$(basename "$img")"; then
        echo "FAILED: mounted state differs for $(basename "$img")" >&2
        failures=1
    fi
    umount "$mnt"
    if ! python3 "$tools_dir/log_probe.py" "$img" --expect clean \
            >/dev/null; then
        echo "FAILED: no clean log after the kernel unmounted $(basename "$img")" \
            >&2
        failures=1
    fi
}

for image in $images; do
    cp "$dir/$image" "$work/$image"
    check_image "$work/$image"
done

# ---------------------------------------------------------------------------
# Reverse interoperability: the kernel creates, writes and removes inside the
# Windows-created /r3dir, then the image goes back to the Windows job.
# ---------------------------------------------------------------------------
echo "== reverse interoperability =="
cp "$dir/create.img" "$work/reverse.img"
if ! mount -o loop,nouuid "$work/reverse.img" "$mnt"; then
    echo "FAILED: cannot mount create.img for the reverse test" >&2
    failures=1
else
    printf 'LINUX-AFTER-WINDOWS' > "$mnt/r3dir/linux-after.txt"
    mkdir "$mnt/r3dir/linuxdir"
    printf 'linux-child-data' > "$mnt/r3dir/linuxdir/linux-child.txt"
    rm "$mnt/r3dir/empty.bin"
    sync
    umount "$mnt"
    cp "$work/reverse.img" "$dir/reverse.img"

    if ! python3 "$tools_dir/rw3_probe.py" "$dir/reverse.img" --quiet; then
        echo "FAILED: oracle rejected the reverse image" >&2
        failures=1
    fi
    if ! xfs_repair -n "$dir/reverse.img" >/dev/null 2>&1; then
        echo "FAILED: xfs_repair after the Linux modifications" >&2
        failures=1
    fi
    # Read the new state back through another mount.
    if mount -o loop,nouuid "$dir/reverse.img" "$mnt"; then
        ok=1
        [ "$(cat "$mnt/r3dir/linux-after.txt")" = "LINUX-AFTER-WINDOWS" ] || ok=0
        [ "$(cat "$mnt/r3dir/linuxdir/linux-child.txt")" = "linux-child-data" ] || ok=0
        [ ! -e "$mnt/r3dir/empty.bin" ] || ok=0
        [ -f "$mnt/r3dir/child.bin" ] || ok=0
        [ -f "$mnt/r3.bin" ] || ok=0
        if [ "$ok" != "1" ]; then
            echo "FAILED: the reverse image does not show the Linux writes" >&2
            failures=1
        fi
        umount "$mnt"
    else
        echo "FAILED: cannot remount the reverse image" >&2
        failures=1
    fi
fi

if [ "$failures" != "0" ]; then
    echo "RW-3 LINUX VERIFICATION FAILED" >&2
    exit 1
fi
echo "OK: every RW-3 image mounted, replayed and verified by Linux"
