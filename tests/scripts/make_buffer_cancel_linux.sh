#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# QUAL-2: capture genuine Linux XFS_XFS_BLF_CANCEL dirty logs.
#
# The Linux kernel only writes a buffer cancellation when metadata that was
# logged earlier is freed (xfs_trans_binval).  The stale item is special: its
# transaction usually also carries a deferred-free intent (EFI/RUI), and those
# intents pin the log tail long enough for a device-mapper snapshot to observe
# the cancel while it is still inside the recovery window.  The script:
#
#   * mounts a rmapbt filesystem through a snapshot-origin target;
#   * runs a metadata-reuse workload (buffer_cancel_workload.py) that frees
#     bmbt blocks, directory blocks and complete inode chunks;
#   * takes atomic dm snapshots of the loop origin (no filesystem freeze that
#     would checkpoint the tail);
#   * probes every snapshot with cancel_probe.py, an independent raw-log
#     reader, and keeps snapshots whose stale items target the requested
#     metadata classes;
#   * confirms the copied fixture with cancel_probe.py and xfs_logprint,
#     then lets the Linux kernel itself replay a copy and verifies the result
#     with xfs_scrub and xfs_repair before handing the dirty image on.
#
# usage: make_buffer_cancel_linux.sh <outdir> [workloads]
#   workloads defaults to "bmbt,dir,inode"; "mixed" runs everything.
# environment: SIZE_MB (512), COW_MB (256), MAX_SNAPS (900)
set -euo pipefail

OUT=${1:?usage: make_buffer_cancel_linux.sh <outdir> [workloads]}
WORKLOADS=${2:-${WORKLOADS:-bmbt,dir,inode}}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
SIZE_MB=${SIZE_MB:-512}
COW_MB=${COW_MB:-256}
MAX_SNAPS=${MAX_SNAPS:-900}
WORK=$(mktemp -d)
LOOP=""
COW_LOOP=""
SNAP_NAME="qual2-cancel-snap"
ORIGIN_NAME="qual2-cancel-origin"
REAL_NAME="qual2-cancel-real"
MOUNT="$WORK/mnt"
SANITY_MNT="$WORK/sanity-mnt"
WRITER_PID=""
mkdir -p "$MOUNT" "$SANITY_MNT" "$OUT" "$OUT/diagnostics"
OUT=$(cd "$OUT" && pwd)

resume_origin() {
    dmsetup resume "$ORIGIN_NAME" >/dev/null 2>&1 || true
}

remove_snapshot() {
    dmsetup suspend --nolockfs "$ORIGIN_NAME" >/dev/null 2>&1 || true
    dmsetup remove --retry "$SNAP_NAME" >/dev/null 2>&1 || true
    resume_origin
}

detach_cow() {
    [ -n "$COW_LOOP" ] || return 0
    losetup -d "$COW_LOOP" >/dev/null 2>&1 || true
    for _ in $(seq 1 100); do
        if ! losetup -j "$WORK/cow.img" 2>/dev/null | grep -q .; then
            COW_LOOP=""
            return 0
        fi
        sleep 0.01
    done
    echo "COW loop device did not detach" >&2
    return 1
}

cleanup() {
    set +e
    if [ -n "$WRITER_PID" ]; then
        kill -CONT "$WRITER_PID" >/dev/null 2>&1
        kill "$WRITER_PID" >/dev/null 2>&1
        wait "$WRITER_PID" >/dev/null 2>&1
    fi
    remove_snapshot
    if mountpoint -q "$MOUNT"; then
        umount "$MOUNT" >/dev/null 2>&1 ||
            umount -l "$MOUNT" >/dev/null 2>&1 ||
            echo "WARNING: cannot unmount $MOUNT; run: umount -l $MOUNT" >&2
    fi
    if mountpoint -q "$SANITY_MNT"; then
        umount "$SANITY_MNT" >/dev/null 2>&1 ||
            umount -l "$SANITY_MNT" >/dev/null 2>&1 ||
            echo "WARNING: cannot unmount $SANITY_MNT" >&2
    fi
    dmsetup remove --retry "$ORIGIN_NAME" >/dev/null 2>&1 ||
        echo "WARNING: dm device $ORIGIN_NAME leaked" >&2
    dmsetup remove --retry "$REAL_NAME" >/dev/null 2>&1 ||
        echo "WARNING: dm device $REAL_NAME leaked" >&2
    detach_cow
    if [ -n "$LOOP" ]; then
        losetup -d "$LOOP" >/dev/null 2>&1 ||
            echo "WARNING: loop device $LOOP leaked" >&2
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ "$(id -u)" != "0" ]; then
    echo "must run as root (loop device/dm-snapshot/mount)" >&2
    exit 1
fi
for tool in mkfs.xfs xfs_repair xfs_logprint xfs_db xfs_info losetup \
            dmsetup python3 dd sha256sum; do
    command -v "$tool" >/dev/null || {
        echo "missing tool: $tool" >&2
        exit 1
    }
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
# rmapbt and the deferred-free intents are what keep the cancels inside the
# replay window long enough to observe them.
MKFS_OPTS="-m crc=1,rmapbt=1,reflink=0,finobt=1,inobtcount=1,bigtime=1 -i sparse=0,nrext64=1"

LIVE="$WORK/live.img"
COW="$WORK/cow.img"
rm -f "$LIVE" "$COW"
truncate -s "${SIZE_MB}M" "$LIVE"
truncate -s "${COW_MB}M" "$COW"
SECTORS=$((SIZE_MB * 1024 * 1024 / 512))
LOOP=$(losetup --find --show "$LIVE")
udevadm settle --timeout=5 >/dev/null 2>&1 || true
if ! dmsetup create "$REAL_NAME" \
        --table "0 $SECTORS linear $LOOP 0" 2>"$WORK/real.err"; then
    cat "$WORK/real.err" >&2
    exit 1
fi
if ! dmsetup create "$ORIGIN_NAME" \
        --table "0 $SECTORS snapshot-origin /dev/mapper/$REAL_NAME" \
        2>"$WORK/origin.err"; then
    cat "$WORK/origin.err" >&2
    exit 1
fi
udevadm settle --timeout=5 >/dev/null 2>&1 || true
ORIGIN="/dev/mapper/$ORIGIN_NAME"
for _ in $(seq 1 100); do
    [ -b "$ORIGIN" ] && break
    sleep 0.01
done
if [ ! -b "$ORIGIN" ]; then
    echo "device-mapper origin node $ORIGIN did not appear" >&2
    exit 1
fi
mkfs.xfs -f -q $MKFS_OPTS $PARENT_OPT "$ORIGIN"
mount "$ORIGIN" "$MOUNT"
xfs_info "$MOUNT" > "$OUT/xfs_info.txt" 2>&1

python3 "$REPO/tests/tools/buffer_cancel_workload.py" "$MOUNT" "$WORKLOADS" &
WRITER_PID=$!
sleep 0.2
if ! kill -0 "$WRITER_PID" 2>/dev/null; then
    echo "buffer_cancel_workload.py exited immediately" >&2
    exit 1
fi
echo "capture: workload '$WORKLOADS' on $MOUNT" >&2

create_snapshot() {
    for _ in $(seq 1 5); do
        remove_snapshot
        detach_cow
        truncate -s 0 "$COW"
        truncate -s "${COW_MB}M" "$COW"
        COW_LOOP=$(losetup --find --show "$COW")
        udevadm settle --timeout=5 >/dev/null 2>&1 || true
        if ! dmsetup suspend --nolockfs "$ORIGIN_NAME" >/dev/null 2>&1; then
            echo "could not suspend the snapshot origin (nolockfs)" >&2
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

# Prints the requested classes whose cancels are present in this probe.
classify_probe() {
    python3 - "$1" "$2" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1]))
remaining = [x for x in sys.argv[2].split(",") if x]
kinds = {
    "bmbt": {"bmbt"},
    "dir": {"dir block", "dir data", "dir free", "dir leaf1", "dir leafn",
            "da node (dir/attr)"},
    "inode": {"inode cluster", "inode cluster (dino)"},
    "symlink": {"symlink"},
    "attr": {"attr leaf", "attr remote", "remote attr value",
             "da node (dir/attr)"},
}
seen = set()
for cancel in data.get("cancels", []):
    target = cancel.get("target") or {}
    seen.add(target.get("kind", "unknown"))
for cls in remaining:
    if cls in kinds and seen & kinds[cls]:
        print(cls)
PY
}

# A fixture is only legal if two independent readers see at least one cancel
# and the Linux kernel can recover it without repair or scrub complaints.
validate_copy() {
    local image=$1
    local base=$2
    local classes=$3

    if ! python3 "$REPO/tests/tools/cancel_probe.py" "$image" --json \
            --require-cancel > "$OUT/$base.probe.json"; then
        echo "FAIL: cancel_probe rejected the copied fixture" >&2
        return 1
    fi
    python3 "$REPO/tests/tools/cancel_probe.py" "$image" \
        > "$OUT/$base.probe.txt" 2>&1 || true
    local found
    found=$(classify_probe "$OUT/$base.probe.json" "$classes")
    if [ "$found" != "$(printf '%s\n' "$classes" | tr ',' '\n')" ]; then
        echo "FAIL: copied fixture lost a requested cancel class" >&2
        echo "  requested: $classes; found: ${found:-none}" >&2
        return 1
    fi
    if ! grep -Eq '^[[:space:]]*BUF:.*flags:0x2([[:space:]]|$)' \
            "$OUT/$base.logprint.txt"; then
        echo "FAIL: xfs_logprint did not report a cancel (flags:0x2)" >&2
        return 1
    fi

    local sanity="$WORK/$base.sanity.img"
    cp --sparse=always "$image" "$sanity"
    if ! mount -o loop,nouuid "$sanity" "$SANITY_MNT"; then
        echo "FAIL: the Linux kernel refused to replay $base" >&2
        dmesg | tail -40 >&2 || true
        return 1
    fi
    local scrub_status=0
    if command -v xfs_scrub >/dev/null 2>&1; then
        if ! xfs_scrub -n "$SANITY_MNT" > "$OUT/$base.scrub.txt" 2>&1; then
            if grep -q 'Kernel metadata scrubbing facility is not available' \
                    "$OUT/$base.scrub.txt"; then
                echo "capture: kernel has no online scrub ioctl; recording a skip" \
                    | tee -a "$OUT/$base.scrub.txt" >&2
            else
                scrub_status=1
            fi
        fi
    else
        echo "capture: xfs_scrub is not installed; recording a skip" \
            > "$OUT/$base.scrub.txt"
    fi
    if ! umount "$SANITY_MNT"; then
        echo "FAIL: clean unmount of the kernel-recovered copy failed" >&2
        return 1
    fi
    xfs_repair -n "$sanity" > "$OUT/$base.repair.txt" 2>&1 || true
    if [ "$scrub_status" != 0 ] ||
       grep -Eiq 'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
            "$OUT/$base.repair.txt"; then
        echo "FAIL: the kernel-recovered copy failed repair/scrub" >&2
        cat "$OUT/$base.repair.txt" >&2
        return 1
    fi
    rm -f "$sanity"
    return 0
}

remaining="$WORKLOADS"
attempts=0
captured=0
declare -A class_image=()

while [ -n "$remaining" ] && [ "$attempts" -lt "$MAX_SNAPS" ]; do
    attempts=$((attempts + 1))
    if ! create_snapshot; then
        echo "could not create a dm snapshot after 5 attempts" >&2
        exit 1
    fi
    SNAP="/dev/mapper/$SNAP_NAME"
    for _ in $(seq 1 100); do
        [ -b "$SNAP" ] && break
        sleep 0.01
    done
    if [ ! -b "$SNAP" ]; then
        echo "device-mapper snapshot node $SNAP did not appear" >&2
        exit 1
    fi
    probe_rc=0
    python3 "$REPO/tests/tools/cancel_probe.py" "$SNAP" --json \
        --require-cancel > "$WORK/probe.json" 2>"$WORK/probe.err" || probe_rc=$?
    if [ "$probe_rc" = 0 ]; then
        classes=$(classify_probe "$WORK/probe.json" "$remaining" | paste -sd, -)
        if [ -n "$classes" ]; then
            name="cancel-$(printf '%s' "$classes" | tr ',' '-').img"
            # The snapshot is immutable, but stopping the workload keeps the
            # COW below its limit while the (potentially 512 MiB) image is
            # copied out.
            kill -STOP "$WRITER_PID" >/dev/null 2>&1 || true
            if ! dd if="$SNAP" of="$OUT/$name" bs=4M conv=sparse status=none; then
                kill -CONT "$WRITER_PID" >/dev/null 2>&1 || true
                echo "copying the snapshot failed (COW overflow?)" >&2
                exit 1
            fi
            cp "$WORK/probe.json" "$OUT/${name%.img}.snapshot.probe.json"
            xfs_logprint -t "$OUT/$name" > "$OUT/${name%.img}.logprint.txt" 2>&1 || true
            sha256=$(sha256sum "$OUT/$name" | awk '{print $1}')
            cp "$WORK/probe.json" "$OUT/${name%.img}.probe.json"
            kill -CONT "$WRITER_PID" >/dev/null 2>&1 || true
            if ! validate_copy "$OUT/$name" "${name%.img}" "$classes"; then
                echo "fixture $name failed validation; artifacts kept in $OUT" >&2
                exit 1
            fi
            python3 - "$OUT/${name%.img}.manifest.json" "$name" "$classes" \
                "$sha256" "$attempts" "$MKFS_OPTS" "$PARENT_OPT" \
                "$WORKLOADS" "$SIZE_MB" "$OUT/${name%.img}.probe.json" <<'PY'
import json
import os
import platform
import subprocess
import sys

out, image, classes, sha, attempts, mkfs_opts, parent_opt, workloads, size_mb, probe = \
    sys.argv[1:]

def version(command):
    try:
        text = subprocess.run(command, capture_output=True, text=True,
                              timeout=10).stdout
    except Exception:
        return ""
    return text.splitlines()[0] if text else ""

data = {
    "kind": "linux-buffer-cancel-fixture",
    "image": image,
    "classes": classes.split(","),
    "sha256": sha,
    "size_bytes": int(size_mb) * 1024 * 1024,
    "snapshot_attempt": int(attempts),
    "workloads": workloads,
    "mkfs_options": (mkfs_opts + " " + parent_opt).strip(),
    "kernel": platform.release(),
    "xfsprogs": version(["xfs_db", "-V"]),
    "probe": json.load(open(probe)),
}
with open(out, "w") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
PY
            captured=$((captured + 1))
            for cls in ${classes//,/ }; do
                remaining=$(printf '%s' "$remaining" | tr ',' '\n' |
                            grep -vx "$cls" | paste -sd, -)
            done
            echo "capture: $name covers '$classes' (attempt $attempts, remaining '${remaining:-none}')" >&2
        fi
    elif [ "$probe_rc" = 1 ]; then
        # Keep the parser's diagnostic for the first few anomalous snapshots;
        # they are evidence about real Linux encodings, not noise.
        if [ "$(ls "$OUT/diagnostics" | wc -l)" -lt 20 ]; then
            cp "$WORK/probe.json" \
                "$OUT/diagnostics/anomaly-$attempts.probe.json" 2>/dev/null || true
            cp "$WORK/probe.err" \
                "$OUT/diagnostics/anomaly-$attempts.err" 2>/dev/null || true
        fi
    else
        # No cancel (usually a clean window or one without metadata reuse).
        :
    fi
    if [ $((attempts % 50)) -eq 0 ]; then
        echo "capture: $attempts snapshots, $captured fixture(s), remaining '${remaining:-none}'" >&2
        if ! kill -0 "$WRITER_PID" 2>/dev/null; then
            wait "$WRITER_PID" 2>/dev/null || true
            WRITER_PID=""
            echo "workload exited before all classes were captured" >&2
            break
        fi
    fi
    sleep 0.01
done

if [ -n "$remaining" ]; then
    echo "no cancel captured for class(es): $remaining after $attempts snapshots" >&2
    echo "diagnostics are in $OUT/diagnostics" >&2
    exit 1
fi

kill -TERM "$WRITER_PID" >/dev/null 2>&1 || true
wait "$WRITER_PID" >/dev/null 2>&1 || true
WRITER_PID=""
umount "$MOUNT"
{
    echo "kernel: $(uname -r)"
    echo "xfsprogs: $(xfs_db -V 2>&1 | head -1)"
    echo "mkfs: $MKFS_OPTS $PARENT_OPT"
    echo "image_size_mb: $SIZE_MB"
    echo "workloads: $WORKLOADS"
    echo "snapshots: $attempts"
    echo "fixtures: $captured"
} > "$OUT/summary.txt"
echo "capture: complete ($captured fixture(s) from $attempts snapshots)" >&2
