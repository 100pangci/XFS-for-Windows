#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B: capture a genuine Linux rmapbt log with a committed RUI that has no
# matching RUD (a surviving deferred rmap intent).
#
# XFS can roll an rmap update intent across two transactions: the RUI is
# committed first and the rmap buffers + RUD in a later transaction.  A crash
# in between leaves exactly the state Windows recovery must complete.  This
# script runs a fragmented-file truncate/unlink workload while continuously
# copying the loop backing file, then keeps the first snapshot whose log has a
# committed RUI with no matching RUD.  The snapshot is a real kernel log, not a
# synthetic item.
#
# usage: make_rw6b_rui_linux.sh <outdir>
set -euo pipefail

OUT=${1:?usage: make_rw6b_rui_linux.sh <outdir>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
SIZE_MB=${SIZE_MB:-512}
ROUNDS=${ROUNDS:-20}
MAX_SNAPS=${MAX_SNAPS:-240}
WORK=$(mktemp -d)
LOOP=""
MOUNT="$WORK/mnt"
mkdir -p "$MOUNT" "$OUT/snaps"

cleanup() {
    set +e
    mountpoint -q "$MOUNT" && umount -l "$MOUNT"
    [ -n "$LOOP" ] && losetup -d "$LOOP" >/dev/null 2>&1
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ "$(id -u)" != "0" ]; then
    echo "must run as root (loop device/mount)" >&2
    exit 1
fi
for tool in mkfs.xfs xfs_repair xfs_logprint losetup python3; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

PARENT_OPT=""
MKFS_HELP=$(mkfs.xfs 2>&1 || true)
if printf '%s\n' "$MKFS_HELP" | grep -q 'parent='; then
    PARENT_OPT="-n parent=0"
fi
MKFS_OPTS="-m crc=1,rmapbt=1,reflink=0,finobt=1,inobtcount=1,bigtime=1 -i sparse=1,nrext64=1"

LIVE="$WORK/live.img"
rm -f "$LIVE"
truncate -s "${SIZE_MB}M" "$LIVE"
LOOP=$(losetup --find --show "$LIVE")
mkfs.xfs -f -q $MKFS_OPTS $PARENT_OPT "$LOOP"
mount "$LOOP" "$MOUNT"

# Prepare a fragmented file that produces many rmap UNMAP intents when it is
# truncated or unlinked.
python3 - "$MOUNT" <<'PY'
import os
import sys
root = sys.argv[1]
block = 4096
os.makedirs(os.path.join(root, "base"), exist_ok=True)
path = os.path.join(root, "base", "fragmented.bin")
with open(path, "wb") as f:
    for i in range(12000):
        f.seek(i * 2 * block)
        f.write((b"R%07d" % i) * (block // 8))
    f.flush()
    os.fsync(f.fileno())
PY
sync

# Snapshot loop.  A snapshot may catch a torn tail; the scanner trims it.
(
    n=0
    while [ ! -f "$WORK/stop" ]; do
        cp --sparse=always "$LIVE" "$OUT/snaps/s$(printf '%05d' "$n").img" \
            2>/dev/null || true
        n=$((n + 1))
        [ "$n" -ge "$MAX_SNAPS" ] && break
    done
) &
COPY_PID=$!

# Workload: repeatedly roll the fragmented file's rmap intents.
python3 - "$MOUNT" "$ROUNDS" <<'PY'
import os
import sys
root = os.path.join(sys.argv[1], "base")
rounds = int(sys.argv[2])
block = 4096
for r in range(rounds):
    path = os.path.join(root, "frag-%02d.bin" % r)
    with open(path, "wb") as f:
        for i in range(12000):
            f.seek(i * 2 * block)
            f.write((b"X%07d" % i) * (block // 8))
        f.flush()
        os.fsync(f.fileno())
    # The truncate is the deferred rmap-unmap workload.
    os.truncate(path, 0)
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
    os.unlink(path)
    dirfd = os.open(root, os.O_RDONLY)
    try:
        os.fsync(dirfd)
    finally:
        os.close(dirfd)
PY
sync
touch "$WORK/stop"
wait "$COPY_PID" || true
umount "$MOUNT"
losetup -d "$LOOP"
LOOP=""

# Find the first dirty snapshot with an active, committed RUI and no matching
# RUD.  recovery_probe.py inventories the host-order RUI/RUD ids.
FOUND=""
for snap in "$OUT"/snaps/*.img; do
    [ -e "$snap" ] || continue
    json="$WORK/probe.json"
    if ! python3 "$REPO/tests/tools/recovery_probe.py" "$snap" \
            --json > "$json" 2>/dev/null; then
        continue
    fi
    if python3 - "$json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1]))
if data.get("state") != "dirty":
    raise SystemExit(1)
active = set()
for trans in data.get("transactions", []):
    for item in trans.get("items", []):
        if item.get("type") not in ("XFS_LI_RUI", "XFS_LI_RUD"):
            continue
        ident = item.get("id")
        if ident is None:
            continue
        if item["type"] == "XFS_LI_RUI":
            active.add(ident)
        else:
            active.discard(ident)
if not active:
    raise SystemExit(1)
print("surviving RUI id(s):", ",".join(str(x) for x in sorted(active)))
PY
    then
        FOUND="$snap"
        break
    fi
    rm -f "$snap"
done

if [ -z "$FOUND" ]; then
    echo "no surviving RUI captured in $ROUNDS rounds" >&2
    exit 1
fi

cp --sparse=always "$FOUND" "$OUT/rui.img"
python3 "$REPO/tests/tools/recovery_probe.py" "$OUT/rui.img" \
    --json > "$OUT/rui.journal.json"
xfs_logprint -t "$OUT/rui.img" > "$OUT/rui.logprint.txt" 2>&1 || true

# Two independent readers must agree that the snapshot contains a RUI.  The
# recovery_probe inventory below proves the matching RUD is absent; logprint is
# the second source and is archived with the fixture.
if ! grep -q 'RUI' "$OUT/rui.logprint.txt"; then
    echo "xfs_logprint did not report a RUI in the selected snapshot" >&2
    tail -100 "$OUT/rui.logprint.txt" >&2
    exit 1
fi

# The Linux kernel itself must be able to recover the snapshot (otherwise it
# is not a legal crash image).  Do this on a copy, leaving rui.img untouched
# for the Windows recovery test.
cp --sparse=always "$OUT/rui.img" "$WORK/sanity.img"
mkdir -p "$WORK/sanity-mnt"
mount -o loop "$WORK/sanity.img" "$WORK/sanity-mnt"
umount "$WORK/sanity-mnt"
xfs_repair -n "$WORK/sanity.img" > "$OUT/rui.repair.txt" 2>&1
if grep -Eiq 'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
        "$OUT/rui.repair.txt"; then
    echo "kernel-recovered RUI snapshot failed xfs_repair" >&2
    cat "$OUT/rui.repair.txt" >&2
    exit 1
fi

python3 - "$OUT/rui.journal.json" "$OUT/rui.manifest.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1]))
active = {}
for trans in data.get("transactions", []):
    for item in trans.get("items", []):
        if item.get("type") not in ("XFS_LI_RUI", "XFS_LI_RUD"):
            continue
        ident = item.get("id")
        if ident is None:
            continue
        if item["type"] == "XFS_LI_RUI":
            active[ident] = item
        else:
            active.pop(ident, None)
manifest = {
    "operation": "Linux fragmented-file truncate/unlink",
    "crash_point": "after RUI commit, before matching RUD",
    "snapshot": "ring copy of the loop backing file during the workload",
    "surviving_rui": [
        {"id": ident, "nextents": item.get("nextents"),
         "extents": item.get("extents")}
        for ident, item in active.items()
    ],
    "logprint": "rui.logprint.txt",
    "kernel_recovery": "clean; xfs_repair -n passed",
}
json.dump(manifest, open(sys.argv[2], "w"), indent=2, sort_keys=True)
print("wrote %s with %d surviving RUI(s)" %
      (sys.argv[2], len(manifest["surviving_rui"])))
PY

rm -rf "$OUT/snaps"
echo "wrote $OUT/rui.img"
