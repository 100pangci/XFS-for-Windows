#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-5: generate *real* Linux XFS dirty-log snapshots (runs on Linux, root).
#
# Every scenario is produced on a fresh minimal-profile filesystem:
#
#   mkfs.xfs (crc=1, finobt=0, rmapbt=0, reflink=0, sparse=0, bigtime=0,
#             inobtcount=0)
#   mount on a device-mapper linear device over a loop file
#   build the base tree, run one operation, fsync the affected file/dir
#     (the transaction is now durable in the log; the metadata behind it may
#     still be in the journal - the crash state recovery has to handle)
#   suspend the dm device (completed writes are now the whole truth)
#   copy the loop backing file (bit-for-bit snapshot of the block device)
#   resume, unmount
#
# The device-mapper suspend is what makes this a crash-consistent block-device
# snapshot instead of "cp of a live image": no write can be in flight while the
# snapshot is copied, and the backup file is exactly the bytes that reached the
# device.
#
# Output: <outdir>/<scenario>.img, <scenario>.manifest.json (expected post-op
# state), <scenario>.logprint.txt (xfs_logprint -t) and summary.txt.
#
# usage: make_dirty_logs_linux.sh <outdir>
set -euo pipefail

OUT=${1:?usage: make_dirty_logs_linux.sh <outdir>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
# mkfs.xfs refuses to format anything below 300 MB.
SIZE_MB=512
SCENARIOS="timestamp extend truncate create unlink mkdir rmdir rename-same
rename-cross rename-replace hardlink symlink-inline symlink-remote"

mkdir -p "$OUT"
WORK=$(mktemp -d)
LOOP=""
DM=""
LIVE=""
MOUNT="$WORK/mnt"
mkdir -p "$MOUNT"

cleanup() {
    set +e
    mountpoint -q "$MOUNT" && umount -l "$MOUNT"
    if [ -n "$DM" ]; then dmsetup remove -f "$DM" >/dev/null 2>&1; fi
    if [ -n "$LOOP" ]; then losetup -d "$LOOP" >/dev/null 2>&1; fi
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ "$(id -u)" != "0" ]; then
    echo "must run as root (loop devices, device-mapper, mount)" >&2
    exit 1
fi
for tool in mkfs.xfs xfs_logprint losetup dmsetup dd python3; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

stop_device() {
    set +e
    mountpoint -q "$MOUNT" && umount -l "$MOUNT"
    if [ -n "$DM" ]; then dmsetup remove -f "$DM" >/dev/null 2>&1; fi
    if [ -n "$LOOP" ]; then losetup -d "$LOOP" >/dev/null 2>&1; fi
    set -e
    DM=""
    LOOP=""
}

: > "$OUT/summary.txt"
for scenario in $SCENARIOS; do
    echo "== $scenario =="
    captured=0
    for attempt in 1 2 3; do
        stop_device
        LIVE="$WORK/live.img"
        rm -f "$LIVE"
        truncate -s "${SIZE_MB}M" "$LIVE"
        LOOP=$(losetup --find --show "$LIVE")
        DM="xfswin-rw5-$$"
        dmsetup create "$DM" --table "0 $(blockdev --getsz "$LOOP") linear $LOOP 0"
        DEV="/dev/mapper/$DM"
        # The writable profile of this project, nothing else. (mkfs.xfs: crc /
        # finobt / rmapbt / reflink / inobtcount / bigtime are -m options;
        # sparse is an -i option.)
        mkfs.xfs -f -q \
            -m crc=1,finobt=0,rmapbt=0,reflink=0,inobtcount=0,bigtime=0 \
            -i sparse=0 "$DEV"
        mount "$DEV" "$MOUNT"
        python3 "$REPO/tests/tools/dirty_ops.py" setup "$MOUNT" >/dev/null
        python3 "$REPO/tests/tools/dirty_ops.py" "$scenario" "$MOUNT" \
            "$OUT/$scenario.manifest.json"
        # Freeze a replayable window: one more fsync'd transaction keeps the
        # metadata behind the log records dirty, so the tail cannot have moved
        # past them yet. (The AIL could in principle still win the race, hence
        # the retry loop.)
        python3 "$REPO/tests/tools/dirty_ops.py" pin "$MOUNT"
        dmsetup suspend "$DM"
        dd if="$LIVE" of="$OUT/$scenario.img" bs=1M status=none conv=sparse
        dmsetup resume "$DM"
        umount "$MOUNT"
        # Record what the kernel's own log dump says, then let the independent
        # probe classify the snapshot (it must be dirty, otherwise the capture
        # method is wrong).
        xfs_logprint -t "$OUT/$scenario.img" > "$OUT/$scenario.logprint.txt" 2>&1 || true
        STATE=$(python3 "$REPO/tests/tools/recovery_probe.py" \
                    "$OUT/$scenario.img" | head -1 || true)
        if [ "$STATE" = "state: dirty" ]; then
            captured=1
            break
        fi
        echo "  attempt $attempt: not dirty ($STATE), retrying"
    done
    echo "$scenario $STATE" >> "$OUT/summary.txt"
    if [ "$captured" -ne 1 ]; then
        echo "ERROR: $scenario is not a dirty log ($STATE)" >&2
        exit 1
    fi
    # Record the real item inventory of this operation (docs/RECOVERY.md's
    # operation -> item table is built from this evidence, not from guesses).
    python3 "$REPO/tests/tools/recovery_probe.py" \
        "$OUT/$scenario.img" --json > "$OUT/$scenario.journal.json"
    python3 - "$scenario" "$OUT/$scenario.journal.json" >> "$OUT/inventory.txt" <<'PY'
import json
import sys

scenario, path = sys.argv[1], sys.argv[2]
data = json.load(open(path))
counts = {}
for trans in data["transactions"]:
    for item in trans["items"]:
        counts[item["type"]] = counts.get(item["type"], 0) + 1
print("%-16s records=%d transactions=%d items: %s" % (
    scenario, len(data["records"]), len(data["transactions"]),
    ", ".join("%s=%d" % (k, counts[k]) for k in sorted(counts)) or "none"))
PY
    echo "  $STATE ($(stat -c %s "$OUT/$scenario.img") bytes)"
    stop_device
done

echo "wrote $(ls "$OUT"/*.img | wc -l) dirty images to $OUT"
