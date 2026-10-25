#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6C: capture a genuine Linux reflink log with a committed CUI that has no
# matching CUD.  The workload keeps clone and CoW work in flight while atomic
# device-mapper snapshots are probed for a surviving refcount intent.
#
# The resulting cui.img is a real kernel log.  A kernel-recovered copy is kept
# as the independent semantic oracle for the Windows recovery result.
#
# usage: make_rw6c_cui_linux.sh <outdir>
set -euo pipefail

OUT=${1:?usage: make_rw6c_cui_linux.sh <outdir>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
SIZE_MB=${SIZE_MB:-768}
COW_MB=${COW_MB:-512}
ROUNDS=${ROUNDS:-400}
THREADS=${THREADS:-8}
MAX_SNAPS=${MAX_SNAPS:-2500}
WORK=$(mktemp -d)
LOOP=""
COW_LOOP=""
SNAP_NAME="rw6c-cui-snap"
ORIGIN_NAME="rw6c-cui-origin"
REAL_NAME="rw6c-cui-real"
MOUNT="$WORK/mnt"
mkdir -p "$MOUNT" "$OUT"

resume_origin() {
    dmsetup resume "$ORIGIN_NAME" >/dev/null 2>&1 || true
}

remove_snapshot() {
    dmsetup suspend --nolockfs "$ORIGIN_NAME" >/dev/null 2>&1 || true
    dmsetup remove --retry "$SNAP_NAME" >/dev/null 2>&1 || true
    resume_origin
}

cleanup() {
    set +e
    if [ -n "${WRITER_PID:-}" ]; then
        kill -CONT "$WRITER_PID" >/dev/null 2>&1
        kill "$WRITER_PID" >/dev/null 2>&1
        wait "$WRITER_PID" >/dev/null 2>&1
    fi
    remove_snapshot
    mountpoint -q "$MOUNT" && umount -l "$MOUNT" >/dev/null 2>&1
    dmsetup remove --retry "$ORIGIN_NAME" >/dev/null 2>&1
    dmsetup remove --retry "$REAL_NAME" >/dev/null 2>&1
    [ -n "$COW_LOOP" ] && losetup -d "$COW_LOOP" >/dev/null 2>&1
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
    echo "dm-snapshot target is unavailable" >&2
    exit 1
fi

PARENT_OPT=""
MKFS_HELP=$(mkfs.xfs 2>&1 || true)
if printf '%s\n' "$MKFS_HELP" | grep -q 'parent='; then
    PARENT_OPT="-n parent=0"
fi
MKFS_OPTS="-m crc=1,rmapbt=1,reflink=1,finobt=1,inobtcount=1,bigtime=1 -i sparse=1,nrext64=1"

LIVE="$WORK/live.img"
COW="$WORK/cow.img"
rm -f "$LIVE" "$COW"
truncate -s "${SIZE_MB}M" "$LIVE"
truncate -s "${COW_MB}M" "$COW"
SECTORS=$((SIZE_MB * 1024 * 1024 / 512))
LOOP=$(losetup --find --show "$LIVE")
udevadm settle --timeout=5 >/dev/null 2>&1 || true
dmsetup create "$REAL_NAME" --table "0 $SECTORS linear $LOOP 0"
dmsetup create "$ORIGIN_NAME" \
    --table "0 $SECTORS snapshot-origin /dev/mapper/$REAL_NAME"
udevadm settle --timeout=5 >/dev/null 2>&1 || true
ORIGIN="/dev/mapper/$ORIGIN_NAME"
for _ in $(seq 1 100); do
    [ -b "$ORIGIN" ] && break
    sleep 0.01
done
[ -b "$ORIGIN" ] || { echo "origin node did not appear" >&2; exit 1; }
mkfs.xfs -f -q $MKFS_OPTS $PARENT_OPT "$ORIGIN"
mount "$ORIGIN" "$MOUNT"

# A shared base file and concurrent clone + CoW + unlink churn.
cat > "$WORK/workload.py" <<'PY'
import os
import subprocess
import sys
import threading

root, threads, rounds = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
block = 4096
base = os.path.join(root, "base.bin")
if not os.path.exists(base):
    with open(base, "wb") as f:
        seed = 12345
        for _ in range(8192):
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            f.write(seed.to_bytes(4, "little"))
        f.flush()
        os.fsync(f.fileno())


def worker(t):
    for r in range(rounds):
        name = os.path.join(root, "c%02d-%05d.bin" % (t, r))
        try:
            subprocess.run(["cp", "--reflink=always", base, name],
                           check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            with open(name, "r+b") as f:
                f.seek(((r * 4096) + t * 131072) % (os.path.getsize(base) - block))
                f.write(bytes(((r * 17 + t * 31 + i) & 0xFF)
                              for i in range(block)))
                f.flush()
                os.fsync(f.fileno())
            if r % 3 == 0:
                clone = name + ".2"
                subprocess.run(["cp", "--reflink=always", name, clone],
                               check=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
                os.unlink(clone)
            os.truncate(name, block if r % 2 else 0)
            os.unlink(name)
        except FileNotFoundError:
            pass


threads = [threading.Thread(target=worker, args=(t,))
           for t in range(threads)]
for t in threads:
    t.start()
for t in threads:
    t.join()
PY

python3 "$WORK/workload.py" "$MOUNT" "$THREADS" "$ROUNDS" &
WRITER_PID=$!

log_has_surviving_cui() {
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
        if item.get("type") not in ("XFS_LI_CUI", "XFS_LI_CUD"):
            continue
        ident = item.get("id")
        if ident is None:
            continue
        if item["type"] == "XFS_LI_CUI":
            active[ident] = item
        else:
            active.pop(ident, None)
if not active:
    raise SystemExit(2)
for ident in sorted(active):
    print("surviving CUI id:", ident,
          "nextents:", active[ident].get("nextents"),
          "extents:", active[ident].get("extents"))
PY
}

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

create_snapshot() {
    for _ in $(seq 1 5); do
        remove_snapshot
        detach_cow
        truncate -s 0 "$COW"
        truncate -s "${COW_MB}M" "$COW"
        COW_LOOP=$(losetup --find --show "$COW")
        udevadm settle --timeout=5 >/dev/null 2>&1 || true
        if ! dmsetup suspend --nolockfs "$ORIGIN_NAME" >/dev/null 2>&1; then
            detach_cow
            return 1
        fi
        if dmsetup create "$SNAP_NAME" \
                --table "0 $SECTORS snapshot /dev/mapper/$REAL_NAME $COW_LOOP P 8" \
                2>"$WORK/dm.err"; then
            resume_origin
            udevadm settle --timeout=5 >/dev/null 2>&1 || true
            return 0
        fi
        cat "$WORK/dm.err" >&2
        resume_origin
        remove_snapshot
        detach_cow
        sleep 0.2
    done
    return 1
}

FOUND=""
attempts=0
cui_seen=0
while [ "$attempts" -lt "$MAX_SNAPS" ]; do
    attempts=$((attempts + 1))
    if ! create_snapshot; then
        echo "could not create a dm snapshot" >&2
        exit 1
    fi
    SNAP="/dev/mapper/$SNAP_NAME"
    for _ in $(seq 1 100); do
        [ -b "$SNAP" ] && break
        sleep 0.01
    done
    if [ ! -b "$SNAP" ]; then
        echo "snapshot node did not appear" >&2
        exit 1
    fi
    if log_has_surviving_cui "$SNAP"; then
        FOUND="$SNAP"
        break
    fi
    if grep -q '"XFS_LI_CUI"' "$WORK/probe.json" 2>/dev/null; then
        cui_seen=$((cui_seen + 1))
    fi
    if [ $((attempts % 100)) -eq 0 ]; then
        echo "capture: $attempts snapshots, $cui_seen with a visible CUI" >&2
        if ! kill -0 "$WRITER_PID" 2>/dev/null; then
            wait "$WRITER_PID" 2>/dev/null || true
            WRITER_PID=""
            break
        fi
    fi
    sleep 0.005
done

if [ -z "$FOUND" ]; then
    echo "no surviving CUI captured in $attempts snapshots" >&2
    echo "diagnostics: $cui_seen snapshots contained a CUI record" >&2
    exit 1
fi

echo "capture: found after $attempts snapshots ($cui_seen with any CUI)" >&2
kill -STOP "$WRITER_PID" >/dev/null 2>&1 || true
dd if="$FOUND" of="$OUT/cui.img" bs=4M conv=sparse status=none
sync
kill -CONT "$WRITER_PID" >/dev/null 2>&1 || true
kill "$WRITER_PID" >/dev/null 2>&1 || true
wait "$WRITER_PID" >/dev/null 2>&1 || true
WRITER_PID=""

python3 "$REPO/tests/tools/recovery_probe.py" "$OUT/cui.img" --json \
    > "$OUT/cui.journal.json"
if ! log_has_surviving_cui "$OUT/cui.img" >/dev/null; then
    echo "copied image lost the surviving CUI" >&2
    exit 1
fi
xfs_logprint -t "$OUT/cui.img" > "$OUT/cui.logprint.txt" 2>&1 || true
if ! grep -q 'CUI' "$OUT/cui.logprint.txt"; then
    echo "xfs_logprint did not report a CUI" >&2
    tail -100 "$OUT/cui.logprint.txt" >&2
    exit 1
fi

cp --sparse=always "$OUT/cui.img" "$WORK/sanity.img"
mkdir -p "$WORK/sanity-mnt"
if ! mount -o loop,nouuid "$WORK/sanity.img" "$WORK/sanity-mnt"; then
    echo "kernel replay of the captured CUI image failed" >&2
    dmesg | tail -40 >&2 || true
    exit 1
fi
umount "$WORK/sanity-mnt"
xfs_repair -n "$WORK/sanity.img" > "$OUT/cui.repair.txt" 2>&1
if grep -Eiq \
        'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
        "$OUT/cui.repair.txt"; then
    echo "kernel-recovered CUI image failed xfs_repair" >&2
    cat "$OUT/cui.repair.txt" >&2
    exit 1
fi
cp --sparse=always "$WORK/sanity.img" "$OUT/cui.kernel-recovered.img"
python3 "$REPO/tests/tools/rw6c_refcount_probe.py" \
    "$OUT/cui.kernel-recovered.img" --json "$OUT/cui.kernel.oracle.json" \
    > "$OUT/cui.kernel.oracle.txt"

python3 - "$OUT/cui.journal.json" "$OUT/cui.manifest.json" "$attempts" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1]))
attempts = int(sys.argv[3])
active = {}
for trans in data.get("transactions", []):
    for item in trans.get("items", []):
        if item.get("type") not in ("XFS_LI_CUI", "XFS_LI_CUD"):
            continue
        ident = item.get("id")
        if ident is None:
            continue
        if item["type"] == "XFS_LI_CUI":
            active[ident] = item
        else:
            active.pop(ident, None)
manifest = {
    "operation": "Linux clone + CoW + truncate/unlink churn",
    "crash_point": "after CUI commit, before matching CUD",
    "snapshot": "atomic device-mapper snapshot of the loop origin",
    "snapshots_examined": attempts,
    "surviving_cui": [
        {"id": ident, "nextents": item.get("nextents"),
         "extents": item.get("extents")}
        for ident, item in active.items()
    ],
    "logprint": "cui.logprint.txt",
    "kernel_recovery": "clean; xfs_repair -n and the refcount oracle passed",
}
json.dump(manifest, open(sys.argv[2], "w"), indent=2, sort_keys=True)
print("wrote %s with %d surviving CUI(s)" %
      (sys.argv[2], len(manifest["surviving_cui"])))
PY

echo "wrote $OUT/cui.img"
