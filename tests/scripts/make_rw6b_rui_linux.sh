#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B: capture a genuine Linux rmapbt log with a committed RUI that has no
# matching RUD (a surviving deferred rmap intent).
#
# XFS rolls an rmap update intent across two transactions: the RUI is committed
# by the truncate system call, while the rmap buffers and RUD are committed
# later by deferred work.  A crash in between leaves exactly the state Windows
# recovery must complete.  The window is short, so this script:
#
#   * keeps a backlog of concurrent fragmented-file truncates, so several RUIs
#     are normally outstanding at once;
#   * takes atomic device-mapper snapshots of the loop origin (no copying race,
#     no filesystem freeze that would checkpoint the intent);
#   * probes the snapshot's log directly, reading only the superblock and the
#     log ring, and keeps the first snapshot whose committed RUI has no RUD.
#
# The resulting rui.img is a real kernel log, not a synthetic item.
#
# usage: make_rw6b_rui_linux.sh <outdir>
set -euo pipefail

OUT=${1:?usage: make_rw6b_rui_linux.sh <outdir>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
SIZE_MB=${SIZE_MB:-512}
FILES=${FILES:-6}
EXTENTS=${EXTENTS:-12000}
ROUNDS=${ROUNDS:-40}
MAX_SNAPS=${MAX_SNAPS:-1200}
COW_MB=${COW_MB:-256}
WORK=$(mktemp -d)
LOOP=""
COW_LOOP=""
SNAP_NAME="rw6b-rui-snap"
MOUNT="$WORK/mnt"
mkdir -p "$MOUNT" "$OUT"

cleanup() {
    set +e
    if [ -n "${WRITER_PID:-}" ]; then
        kill -CONT "$WRITER_PID" >/dev/null 2>&1
        kill "$WRITER_PID" >/dev/null 2>&1
        wait "$WRITER_PID" >/dev/null 2>&1
    fi
    dmsetup remove --retry "$SNAP_NAME" >/dev/null 2>&1
    [ -n "$COW_LOOP" ] && losetup -d "$COW_LOOP" >/dev/null 2>&1
    mountpoint -q "$MOUNT" && umount -l "$MOUNT" >/dev/null 2>&1
    [ -n "$LOOP" ] && losetup -d "$LOOP" >/dev/null 2>&1
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ "$(id -u)" != "0" ]; then
    echo "must run as root (loop device/dm-snapshot/mount)" >&2
    exit 1
fi
for tool in mkfs.xfs xfs_repair xfs_logprint losetup dmsetup python3 dd; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done
if ! dmsetup targets 2>/dev/null | grep -q '^snapshot '; then
    modprobe dm_snapshot >/dev/null 2>&1 || true
fi
if ! dmsetup targets 2>/dev/null | grep -q '^snapshot '; then
    echo "dm-snapshot target is unavailable; refusing a racy capture" >&2
    exit 1
fi

PARENT_OPT=""
MKFS_HELP=$(mkfs.xfs 2>&1 || true)
if printf '%s\n' "$MKFS_HELP" | grep -q 'parent='; then
    PARENT_OPT="-n parent=0"
fi
MKFS_OPTS="-m crc=1,rmapbt=1,reflink=0,finobt=1,inobtcount=1,bigtime=1 -i sparse=1,nrext64=1"

LIVE="$WORK/live.img"
COW="$WORK/cow.img"
rm -f "$LIVE" "$COW"
truncate -s "${SIZE_MB}M" "$LIVE"
truncate -s "${COW_MB}M" "$COW"
LOOP=$(losetup --find --show "$LIVE")
mkfs.xfs -f -q $MKFS_OPTS $PARENT_OPT "$LOOP"
mount "$LOOP" "$MOUNT"
SECTORS=$((SIZE_MB * 1024 * 1024 / 512))

# Pre-create the fragmented files.  Each write leaves a hole between 4 KiB
# blocks, so every file is a btree of thousands of single-block extents.
prepare() {
    python3 - "$MOUNT" "$FILES" "$EXTENTS" <<'PY'
import os
import sys

root, files, extents = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
block = 4096
os.makedirs(os.path.join(root, "base"), exist_ok=True)
for f in range(files):
    path = os.path.join(root, "base", "frag-%02d.bin" % f)
    with open(path, "wb") as out:
        for i in range(extents):
            out.seek(i * 2 * block)
            out.write((b"F%02d%05d" % (f, i)) * (block // 8))
        out.flush()
        os.fsync(out.fileno())
PY
}
prepare
sync

# Workload: repeatedly truncate the whole set concurrently.  Each truncate
# commits an RUI; the matching RUD is deferred, so bursts keep intents in the
# log without their completion.
cat > "$WORK/workload.py" <<'PY'
import os
import sys
import threading

root = os.path.join(sys.argv[1], "base")
rounds = int(sys.argv[2])
files = int(sys.argv[3])
extents = int(sys.argv[4])
block = 4096

def path(f):
    return os.path.join(root, "frag-%02d.bin" % f)

def write_file(f):
    with open(path(f), "wb") as out:
        for i in range(extents):
            out.seek(i * 2 * block)
            out.write((b"R%02d%05d" % (f, i)) * (block // 8))
        out.flush()
        os.fsync(out.fileno())

def truncate_file(f):
    try:
        os.truncate(path(f), 0)
    except FileNotFoundError:
        pass

def run_all(fn):
    threads = [threading.Thread(target=fn, args=(f,)) for f in range(files)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

for r in range(rounds):
    # The prepared files are truncated first; the rewrite at the end of each
    # round prepares the next burst.
    run_all(truncate_file)
    run_all(write_file)
PY

python3 "$WORK/workload.py" "$MOUNT" "$ROUNDS" "$FILES" "$EXTENTS" &
WRITER_PID=$!

log_has_surviving_rui() {
    python3 "$REPO/tests/tools/recovery_probe.py" "$1" --json \
        > "$WORK/probe.json" 2>"$WORK/probe.err" || return 2
    python3 - "$WORK/probe.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1]))
if data.get("state") != "dirty":
    raise SystemExit(2)
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
if not active:
    raise SystemExit(2)
for ident in sorted(active):
    print("surviving RUI id:", ident,
          "nextents:", active[ident].get("nextents"))
PY
}

FOUND=""
attempts=0
rui_seen=0
found_id=""

detach_cow() {
    [ -n "$COW_LOOP" ] || return 0
    losetup -d "$COW_LOOP" >/dev/null 2>&1 || true
    for _ in $(seq 1 100); do
        if ! losetup -j "$COW" 2>/dev/null | grep -q .; then
            COW_LOOP=""
            return 0
        fi
        sleep 0.01
    done
    echo "COW loop device did not detach" >&2
    exit 1
}

while [ "$attempts" -lt "$MAX_SNAPS" ]; do
    attempts=$((attempts + 1))
    dmsetup remove --retry "$SNAP_NAME" >/dev/null 2>&1 || true
    detach_cow
    truncate -s 0 "$COW"
    truncate -s "${COW_MB}M" "$COW"
    COW_LOOP=$(losetup --find --show "$COW")
    if ! dmsetup create "$SNAP_NAME" \
            --table "0 $SECTORS snapshot $LOOP $COW_LOOP P 8" 2>"$WORK/dm.err"; then
        cat "$WORK/dm.err" >&2
        exit 1
    fi
    udevadm settle --timeout=5 >/dev/null 2>&1 || true
    SNAP="/dev/mapper/$SNAP_NAME"
    for _ in $(seq 1 100); do
        [ -b "$SNAP" ] && break
        sleep 0.01
    done
    if [ ! -b "$SNAP" ]; then
        echo "device-mapper snapshot node $SNAP did not appear" >&2
        exit 1
    fi
    if found_id=$(log_has_surviving_rui "$SNAP"); then
        FOUND="$SNAP"
        break
    fi
    # A returning 0 means active RUI; nonzero means no intent yet.  Count how
    # often any RUI (with or without RUD) was visible for diagnostics.
    if grep -q '"XFS_LI_RUI"' "$WORK/probe.json" 2>/dev/null; then
        rui_seen=$((rui_seen + 1))
    fi
    if [ $((attempts % 50)) -eq 0 ]; then
        echo "capture: $attempts snapshots, $rui_seen with a visible RUI" >&2
        # A dead workload cannot produce new intents.
        if ! kill -0 "$WRITER_PID" 2>/dev/null; then
            wait "$WRITER_PID" 2>/dev/null || true
            WRITER_PID=""
            break
        fi
    fi
    sleep 0.01
done

if [ -z "$FOUND" ]; then
    echo "no surviving RUI captured in $attempts snapshots" >&2
    echo "diagnostics: $rui_seen snapshots contained an RUI record" >&2
    exit 1
fi

echo "capture: found after $attempts snapshots ($rui_seen with any RUI)" >&2
# The workload cannot invalidate the snapshot, but stopping it keeps the COW
# small while the snapshot is copied out.
kill -STOP "$WRITER_PID" >/dev/null 2>&1 || true
if ! dd if="$FOUND" of="$OUT/rui.img" bs=4M conv=sparse status=none; then
    echo "copying the snapshot failed (COW overflow?)" >&2
    exit 1
fi
sync
kill -CONT "$WRITER_PID" >/dev/null 2>&1 || true
kill "$WRITER_PID" >/dev/null 2>&1 || true
wait "$WRITER_PID" >/dev/null 2>&1 || true
WRITER_PID=""

python3 "$REPO/tests/tools/recovery_probe.py" "$OUT/rui.img" --json \
    > "$OUT/rui.journal.json"
# Re-check the copied image itself, not just the live snapshot.
if ! log_has_surviving_rui "$OUT/rui.img" >/dev/null; then
    echo "copied image lost the surviving RUI" >&2
    exit 1
fi

xfs_logprint -t "$OUT/rui.img" > "$OUT/rui.logprint.txt" 2>&1 || true
# Two independent readers must agree that the snapshot contains a RUI.  The
# recovery_probe inventory below proves the matching RUD is absent; logprint is
# the second source and archived with the fixture.
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

python3 - "$OUT/rui.journal.json" "$OUT/rui.manifest.json" "$attempts" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1]))
attempts = int(sys.argv[3])
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
    "operation": "Linux concurrent fragmented-file truncate",
    "crash_point": "after RUI commit, before matching RUD",
    "snapshot": "atomic device-mapper snapshot of the loop origin",
    "snapshots_examined": attempts,
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

echo "wrote $OUT/rui.img"
