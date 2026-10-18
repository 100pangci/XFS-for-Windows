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
#     (the transaction is now durable in the log; the metadata behind it is
#     still in the journal - the crash state recovery has to handle)
#   freeze the dm device in the same process as the operation, then copy the
#     loop backing file (bit-for-bit snapshot of the block device)
#   resume, unmount
#   the kernel's own recovery of a copy of the snapshot must reach the manifest
#     state (the fixture is replayable by definition, not just by Windows)
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

# Diagnostics for a captured phase that carries no operation: print the window
# the independent probe found and what the kernel's own dump says.
dump_window() {
    local scenario=$1
    echo "  window for $scenario (probe):"
    python3 - "$OUT/$scenario.journal.json" <<'PY'
import json
import sys

try:
    data = json.load(open(sys.argv[1]))
except Exception as exc:
    print("    no probe data: %s" % exc)
    raise SystemExit(0)
for rec in data["records"]:
    print("    rec lsn=%s block=%d cycle=%d len=%d ops=%d prev=%d tail=%s"
          % (rec["lsn"], rec["block"], rec["cycle"], rec["len"], rec["ops"],
             rec["prev"], rec["tail_lsn"]))
for trans in data["transactions"]:
    items = ", ".join("%s(blkno=%s)" % (item["type"], item.get("blkno", "-"))
                      for item in trans["items"])
    print("    trans tid=%d items=%d: %s"
          % (trans["tid"], len(trans["items"]), items))
print("    head=%s tail=%s" % (data["head"], data["tail"]))
PY
    echo "  window for $scenario (xfs_logprint -t):"
    sed 's/^/    | /' "$OUT/$scenario.logprint.txt" | head -40
}

: > "$OUT/summary.txt"
for scenario in $SCENARIOS; do
    echo "== $scenario =="
    captured=0
    reason=""
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
        # The operation, its manifest and the device freeze all happen inside
        # one process: freezing a couple of milliseconds after the fsync keeps
        # the operation's own transaction in the recovery window instead of
        # letting the AIL write the metadata back first. (It can still win the
        # race every now and then, hence the retry loop.)
        python3 "$REPO/tests/tools/dirty_ops.py" capture "$scenario" "$MOUNT" \
            "$OUT/$scenario.manifest.json" "$DM"
        # The device is frozen: take the bytes.
        dd if="$LIVE" of="$OUT/$scenario.img" bs=1M status=none conv=sparse
        dmsetup resume "$DM"
        umount "$MOUNT"
        # Record what the kernel's own log dump says, then let the independent
        # probe classify the snapshot (it must be dirty, otherwise the capture
        # method is wrong) and check that the window really carries the
        # operation: a window with nothing but one-block superblock buffers is
        # only a counter update, not a namespace/content test.
        xfs_logprint -t "$OUT/$scenario.img" > "$OUT/$scenario.logprint.txt" 2>&1 || true
        python3 "$REPO/tests/tools/recovery_probe.py" \
            "$OUT/$scenario.img" --json > "$OUT/$scenario.journal.json" || true
        STATE=$(python3 -c "import json,sys; print('state: %s' % json.load(open(sys.argv[1]))['state'])" \
                    "$OUT/$scenario.journal.json" 2>/dev/null || true)
        has_op=$(python3 - "$OUT/$scenario.journal.json" <<'PY'
import json
import sys

def sb_only(item):
    return (item["type"] == "XFS_LI_BUF" and item.get("blkno") == 0
            and item.get("len") == 1)

try:
    data = json.load(open(sys.argv[1]))
    items = [item for trans in data["transactions"] for item in trans["items"]]
    print("yes" if any(not sb_only(item) for item in items) else "no")
except Exception:
    print("no")
PY
)
        if [ "$STATE" = "state: dirty" ] && [ "$has_op" = "yes" ]; then
            captured=1
            break
        fi
        reason="$STATE, operation in window: $has_op"
        echo "  attempt $attempt: $reason, retrying"
        dump_window "$scenario"
    done
    echo "$scenario $STATE" >> "$OUT/summary.txt"
    if [ "$captured" -ne 1 ]; then
        echo "ERROR: $scenario did not capture a replayable window ($reason)" >&2
        exit 1
    fi
    # Record the real item inventory of this operation (docs/RECOVERY.md's
    # operation -> item table is built from this evidence, not from guesses).
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
    # Independent fixture sanity: the Linux kernel's own recovery of the
    # snapshot must reach the manifest state as well - a fixture nobody can
    # replay is not a fixture. Done on a copy, so the uploaded image is exactly
    # the bytes that were captured.
    cp --sparse=always "$OUT/$scenario.img" "$WORK/sanity.img"
    mount -o loop "$WORK/sanity.img" "$MOUNT"
    python3 "$REPO/tests/tools/dirty_ops.py" verify "$MOUNT" \
        "$OUT/$scenario.manifest.json"
    umount "$MOUNT"
    rm -f "$WORK/sanity.img"
    echo "  $STATE ($(stat -c %s "$OUT/$scenario.img") bytes, op in window, kernel replay matches the manifest)"
    stop_device
done

echo "wrote $(ls "$OUT"/*.img | wc -l) dirty images to $OUT"
