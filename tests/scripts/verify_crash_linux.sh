#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - Linux side of the crash matrix.
#
#   sudo tests/scripts/verify_crash_linux.sh <crash-dir> <pristine.img> <ino>
#
# <crash-dir> holds crash-<point>.img files produced by
# tests/scripts/verify_crash_windows.ps1 (they are copies of <pristine.img>
# after one xfswin pwrite run that was killed at XFSWIN_CRASH_AT=<point>).
#
# For every image this script requires:
#
#   1. the kernel can mount it - which replays the journal record we wrote,
#   2. after replay the mounted file has exactly one of the two allowed states:
#      the old timestamps (crash before the record was durable) or the logged
#      new ones (crash after it - the kernel applied our record),
#   3. the file content is old or new (data is never journalled),
#   4. the filesystem is clean afterwards: our log parser finds a clean log and
#      xfs_repair -n reports no corruption.
#
# Exit code 0 means every crash window is recoverable by Linux.

set -euo pipefail

crash_dir="${1:?usage: verify_crash_linux.sh <crash-dir> <pristine.img> [ino]}"
pristine="${2:?usage: verify_crash_linux.sh <crash-dir> <pristine.img> [ino]}"
ino="${3:-}"
crash_dir="$(readlink -f "$crash_dir")"
pristine="$(readlink -f "$pristine")"
if [ -z "$ino" ]; then
    manifest="$crash_dir/manifest.json"
    if [ -f "$manifest" ]; then
        ino="$(python3 -c \
            'import json,sys; print(json.load(open(sys.argv[1]))["file_ino"])' \
            "$manifest")"
    else
        echo "usage: verify_crash_linux.sh <crash-dir> <pristine.img> <ino>" >&2
        exit 2
    fi
fi

for tool in mount umount xfs_repair python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool (install xfsprogs/util-linux)" >&2
        exit 1
    }
done
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools_dir="$(cd "$script_dir/../tools" && pwd)"

# What each crash point is allowed to leave behind. "times" is compared against
# the timestamps the kernel shows after replaying the log; the crash points
# before the transaction is durable must not be visible, the ones after it must
# be (the record has to be replayed).
point_rules() {
    case "$1" in
    before_data_write) echo "old|old" ;;
    after_data_write | before_log_write | mid_log_record) echo "old|new" ;;
    after_log_commit | before_metadata_write | after_metadata_write | \
        before_device_flush | after_device_flush) echo "new|new" ;;
    # These points need more than one mapping or more than one metadata buffer
    # to trigger; the current fixture cannot reach them, so both the "completed"
    # and the "crashed" state have to be accepted.
    mid_data_write) echo "old,new|old,new,mixed" ;;
    mid_metadata_write) echo "old,new|new" ;;
    *)
        echo "?"
        return 1
        ;;
    esac
}

failed=0
verified=0
for img in "$crash_dir"/crash-*.img; do
    [ -e "$img" ] || {
        echo "FAILED: no crash images in $crash_dir" >&2
        exit 1
    }
    point="$(basename "$img" .img)"
    point="${point#crash-}"
    rules="$(point_rules "$point")"
    if [ "$rules" = "?" ]; then
        echo "FAILED: unknown crash point $point" >&2
        failed=1
        continue
    fi
    allow_times="${rules%%|*}"
    allow_data="${rules##*|}"
    echo "== $point (times=$allow_times data=$allow_data) =="

    work="$(mktemp -d)"
    mnt="$(mktemp -d)"
    cp "$img" "$work/crash.img"
    cleanup() {
        if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
        rm -rf "$work" "$mnt"
    }
    trap cleanup EXIT

    # Read-write mount: the kernel replays the log and writes its result (and a
    # clean unmount record) back. A mount failure here is a failure of the
    # crash state, not of the script.
    if ! mount -o loop,nouuid "$work/crash.img" "$mnt"; then
        echo "FAILED: the kernel could not mount the $point crash image" >&2
        failed=1
        cleanup
        trap - EXIT
        continue
    fi
    if ! python3 "$tools_dir/rw_replay_check.py" "$mnt/hello.bin" "$img" \
            "$pristine" --ino "$ino" --allow-times "$allow_times" \
            --allow-data "$allow_data"; then
        failed=1
    fi
    umount "$mnt"

    # After the kernel unmounted, the log must be a standard clean XFS log (our
    # own parser reads kernel-written records too).
    if ! python3 "$tools_dir/log_probe.py" "$work/crash.img" --expect clean; then
        echo "FAILED: no clean log after replaying $point" >&2
        failed=1
    fi

    repair_log="$(mktemp)"
    if ! xfs_repair -n "$work/crash.img" > "$repair_log" 2>&1; then
        cat "$repair_log"
        echo "FAILED: xfs_repair -n returned an error for $point" >&2
        failed=1
    elif grep -qiE 'corruption|fatal|would fix|ALERT|valuable metadata' \
            "$repair_log"; then
        cat "$repair_log"
        echo "FAILED: xfs_repair -n found problems in $point" >&2
        failed=1
    else
        tail -2 "$repair_log"
    fi
    rm -f "$repair_log"

    cleanup
    trap - EXIT
    verified=$((verified + 1))
done

if [ "$failed" != "0" ]; then
    echo "CRASH REPLAY MATRIX FAILED" >&2
    exit 1
fi
echo "OK: $verified crash images replayed and verified by Linux"
