#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - Linux side of the RW-3 crash matrix.
#
#   sudo tests/scripts/verify_crash_rw3_linux.sh <crash-dir>
#
# <crash-dir> holds images produced by
# tests/scripts/verify_crash_rw3_windows.ps1: one image per (create | mkdir |
# unlink | rmdir, crash point, transaction hit) plus manifest.json.
#
# Every image must:
#   1. mount read-write with the real kernel (which replays the record we left),
#   2. show one of the two legal states (rw3_crash_replay_check.py): the name
#      absent or completely created, or the original file/directory intact,
#   3. stay clean: a clean log after the unmount, xfs_repair -n silent and the
#      independent structural oracle satisfied.
set -euo pipefail

crash_dir="${1:?usage: verify_crash_rw3_linux.sh <crash-dir>}"
crash_dir="$(readlink -f "$crash_dir")"
for tool in mount umount xfs_repair python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool (install xfsprogs/util-linux)" >&2
        exit 1
    }
done
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools_dir="$(cd "$script_dir/../tools" && pwd)"
manifest="$crash_dir/manifest.json"

failures=0
verified=0
work="$(mktemp -d)"
mnt="$(mktemp -d)"
cleanup() {
    if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
    rm -rf "$work" "$mnt"
}
trap cleanup EXIT

for img in "$crash_dir"/crash-*.img; do
    [ -e "$img" ] || {
        echo "FAILED: no crash images in $crash_dir" >&2
        exit 1
    }
    base="$(basename "$img" .img)"
    echo "== $base =="
    cp "$img" "$work/crash.img"
    if ! mount -o loop,nouuid "$work/crash.img" "$mnt"; then
        echo "FAILED: the kernel could not mount $base" >&2
        dmesg | tail -40 || true
        python3 "$tools_dir/log_probe.py" "$img" --dump | head -40 || true
        failures=1
        continue
    fi
    if ! python3 "$tools_dir/rw3_crash_replay_check.py" "$mnt" "$manifest" \
            "$(basename "$img")"; then
        failures=1
    fi
    umount "$mnt"

    if ! python3 "$tools_dir/log_probe.py" "$work/crash.img" --expect clean \
            >/dev/null; then
        echo "FAILED: no clean log after replaying $base" >&2
        failures=1
    fi
    repair_log="$(mktemp)"
    if ! xfs_repair -n "$work/crash.img" > "$repair_log" 2>&1; then
        cat "$repair_log"
        echo "FAILED: xfs_repair -n returned an error for $base" >&2
        failures=1
    elif grep -qiE 'corruption|fatal|would fix|ALERT|valuable metadata' \
            "$repair_log"; then
        cat "$repair_log"
        echo "FAILED: xfs_repair -n found problems in $base" >&2
        failures=1
    fi
    rm -f "$repair_log"
    if ! python3 "$tools_dir/rw3_probe.py" "$work/crash.img" --quiet \
            >/dev/null; then
        echo "FAILED: the structural oracle rejected the replayed $base" >&2
        failures=1
    fi
    verified=$((verified + 1))
done

if [ "$failures" != "0" ]; then
    echo "RW-3 CRASH REPLAY MATRIX FAILED" >&2
    exit 1
fi
echo "OK: $verified RW-3 crash images replayed and verified by Linux"
