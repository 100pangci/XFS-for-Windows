#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# QUAL-2: verify, on real Linux, that the Windows recovery of a
# Linux-generated buffer-cancel fixture is accepted by the kernel and
# xfsprogs.
#
# Every check is an independent oracle; nothing is filtered or downgraded:
#   1. the independent raw-log reader sees a clean log (no replay left),
#   2. xfs_logprint sees <CLEAN> before and after a real kernel mount,
#   3. xfs_repair -n exits 0 and prints no corruption/warning,
#   4. the kernel mounts read-write and xfs_scrub -n (when available) accepts
#      every metadata structure; a canary file is created, read back and
#      removed so allocation freespace metadata is exercised too,
#   5. the clean unmount leaves another clean log and a silent repair.
#
# usage: verify_buffer_cancel_recovery_linux.sh <dir-of-recovered-images>
set -euo pipefail

DIR=${1:?usage: verify_buffer_cancel_recovery_linux.sh <dir>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
MNT="$WORK/mnt"
mkdir -p "$MNT"

cleanup() {
    set +e
    if mountpoint -q "$MNT"; then
        umount "$MNT" >/dev/null 2>&1 ||
            umount -l "$MNT" >/dev/null 2>&1 ||
            echo "WARNING: cannot unmount $MNT" >&2
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ "$(id -u)" != "0" ]; then
    echo "must run as root (loop mount/xfs_scrub)" >&2
    exit 1
fi
for tool in xfs_repair xfs_logprint python3 mount umount; do
    command -v "$tool" >/dev/null || {
        echo "missing tool: $tool" >&2
        exit 1
    }
done

shopt -s nullglob
images=("$DIR"/cancel-*.img)
if [ ${#images[@]} -eq 0 ]; then
    echo "no cancel-*.img in $DIR" >&2
    exit 1
fi

scrub_mount() {
    local base=$1
    if command -v xfs_scrub >/dev/null 2>&1; then
        if ! xfs_scrub -n "$MNT" > "$DIR/$base.scrub.txt" 2>&1; then
            if grep -q 'Kernel metadata scrubbing facility is not available' \
                    "$DIR/$base.scrub.txt"; then
                echo "  kernel has no online scrub ioctl; recording a skip"
            else
                echo "  FAIL: xfs_scrub rejected the recovered filesystem"
                sed 's/^/    /' "$DIR/$base.scrub.txt" | head -40
                return 1
            fi
        fi
    else
        echo "  xfs_scrub is not installed; recording a skip" \
            > "$DIR/$base.scrub.txt"
    fi
    return 0
}

canary() {
    local path="$MNT/qual2-canary.bin"
    python3 - "$path" <<'PY'
import os
import sys

path = sys.argv[1]
pattern = bytes(((i * 167) ^ (i >> 7) ^ 0x5A) & 0xFF for i in range(65536))
with open(path, "wb") as out:
    out.write(pattern)
    out.flush()
    os.fsync(out.fileno())
with open(path, "rb") as check:
    if check.read() != pattern:
        raise SystemExit("canary read-back mismatch")
os.unlink(path)
PY
    sync
}

failures=0
checked=0
for image in "${images[@]}"; do
    name=$(basename "$image")
    base=${name%.img}
    echo "== $name =="

    # 1. The independent oracle must see a clean log.
    if ! python3 "$REPO/tests/tools/recovery_probe.py" "$image" --json \
            > "$DIR/$base.verify.journal.json" 2> "$DIR/$base.verify.journal.err"; then
        echo "  FAIL: independent log reader rejected the image"
        cat "$DIR/$base.verify.journal.err" | sed 's/^/    /' || true
        failures=$((failures + 1))
        continue
    fi
    if ! grep -q '"state": "clean"' "$DIR/$base.verify.journal.json"; then
        echo "  FAIL: log is not clean after Windows recovery"
        failures=$((failures + 1))
        continue
    fi

    # 2. xfs_logprint must agree the log is clean.
    xfs_logprint -t "$image" > "$DIR/$base.verify.logprint.txt" 2>&1 || true
    if ! grep -q 'state: <CLEAN>' "$DIR/$base.verify.logprint.txt"; then
        echo "  FAIL: xfs_logprint does not report a clean log"
        tail -30 "$DIR/$base.verify.logprint.txt" | sed 's/^/    /'
        failures=$((failures + 1))
        continue
    fi

    # 3. Static repair check before mounting.
    if ! xfs_repair -n "$image" > "$DIR/$base.verify.repair1.txt" 2>&1; then
        echo "  FAIL: xfs_repair -n reported errors"
        sed 's/^/    /' "$DIR/$base.verify.repair1.txt" | head -40
        failures=$((failures + 1))
        continue
    fi
    if grep -Eiq 'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
            "$DIR/$base.verify.repair1.txt"; then
        echo "  FAIL: xfs_repair -n was not silent"
        sed 's/^/    /' "$DIR/$base.verify.repair1.txt" | head -40
        failures=$((failures + 1))
        continue
    fi

    # 4. Real kernel mount, online scrub and a write/read canary.
    if ! mount -o loop,nouuid "$image" "$MNT"; then
        echo "  FAIL: the Linux kernel refused to mount the recovered image"
        dmesg | tail -40 | sed 's/^/    /' || true
        failures=$((failures + 1))
        continue
    fi
    if ! scrub_mount "$base"; then
        failures=$((failures + 1))
        umount "$MNT" >/dev/null 2>&1 || umount -l "$MNT" >/dev/null 2>&1 || true
        continue
    fi
    if ! canary; then
        echo "  FAIL: canary write/read on the recovered filesystem failed"
        failures=$((failures + 1))
        umount "$MNT" >/dev/null 2>&1 || umount -l "$MNT" >/dev/null 2>&1 || true
        continue
    fi
    if ! umount "$MNT"; then
        echo "  FAIL: clean unmount failed"
        failures=$((failures + 1))
        continue
    fi

    # 5. After the clean unmount the log must still be clean and repair silent.
    xfs_logprint -t "$image" > "$DIR/$base.verify.logprint-after.txt" 2>&1 || true
    if ! grep -q 'state: <CLEAN>' "$DIR/$base.verify.logprint-after.txt"; then
        echo "  FAIL: log is not clean after the kernel unmount"
        failures=$((failures + 1))
        continue
    fi
    if ! xfs_repair -n "$image" > "$DIR/$base.verify.repair2.txt" 2>&1; then
        echo "  FAIL: xfs_repair -n reported errors after the mount"
        sed 's/^/    /' "$DIR/$base.verify.repair2.txt" | head -40
        failures=$((failures + 1))
        continue
    fi
    if grep -Eiq 'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
            "$DIR/$base.verify.repair2.txt"; then
        echo "  FAIL: xfs_repair -n was not silent after the mount"
        sed 's/^/    /' "$DIR/$base.verify.repair2.txt" | head -40
        failures=$((failures + 1))
        continue
    fi

    checked=$((checked + 1))
    echo "  ok (clean log, kernel mount, scrub, canary, clean unmount, repair)"
done

echo ""
echo "$checked image(s) accepted, $failures failure(s)"
if [ "$failures" -ne 0 ]; then
    exit 1
fi
