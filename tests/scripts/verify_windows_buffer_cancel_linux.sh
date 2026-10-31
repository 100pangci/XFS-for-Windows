#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# QUAL-2: the Windows writer must emit the Linux XFS_BLF_CANCEL encoding, and
# the real Linux kernel must replay the dirty image.
#
# For every Windows-generated cancel-*.img:
#   1. cancel_probe.py (independent raw-log reader) requires a cancellation
#      with the exact Linux stale encoding and reports its target geometry;
#   2. xfs_logprint must independently print the cancel (flags:0x2) and a
#      <DIRTY> log;
#   3. if the Windows journal dump is present, compare_logprint.py must agree
#      on the record/transaction structure;
#   4. a real Linux kernel mount of a copy replays the log; xfs_scrub -n
#      (when available) must accept the recovered metadata, a canary write
#      must round-trip, and the unmount must be clean;
#   5. xfs_repair -n must be silent and the log must end <CLEAN>.
#
# usage: verify_windows_buffer_cancel_linux.sh <dir> [journal-json-dir]
set -euo pipefail

DIR=${1:?usage: verify_windows_buffer_cancel_linux.sh <dir> [journal-dir]}
JOURNAL_DIR=${2:-$DIR}
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
                echo "  FAIL: xfs_scrub rejected the kernel-recovered image"
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
pattern = bytes(((i * 211) ^ (i >> 5) ^ 0xA5) & 0xFF for i in range(65536))
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

    # 1. Independent raw-log probe: Linux encoding, geometry and ordering.
    if ! python3 "$REPO/tests/tools/cancel_probe.py" "$image" --json \
            --require-cancel > "$DIR/$base.verify.probe.json" \
            2> "$DIR/$base.verify.probe.err"; then
        echo "  FAIL: cancel_probe rejected the Windows dirty image"
        cat "$DIR/$base.verify.probe.err" | sed 's/^/    /' || true
        python3 "$REPO/tests/tools/cancel_probe.py" "$image" 2>/dev/null |
            sed 's/^/    /' || true
        failures=$((failures + 1))
        continue
    fi
    python3 "$REPO/tests/tools/cancel_probe.py" "$image" \
        > "$DIR/$base.verify.probe.txt" 2>&1 || true

    # 2. xfs_logprint must see a dirty log and the cancel flag itself.
    xfs_logprint -t "$image" > "$DIR/$base.verify.logprint.txt" 2>&1 || true
    if ! grep -q 'state: <DIRTY>' "$DIR/$base.verify.logprint.txt"; then
        echo "  FAIL: xfs_logprint does not report a dirty log"
        tail -30 "$DIR/$base.verify.logprint.txt" | sed 's/^/    /'
        failures=$((failures + 1))
        continue
    fi
    if ! grep -Eq '^[[:space:]]*BUF:.*flags:0x2([[:space:]]|$)' \
            "$DIR/$base.verify.logprint.txt"; then
        echo "  FAIL: xfs_logprint did not report a cancel (flags:0x2)"
        failures=$((failures + 1))
        continue
    fi

    # 3. Structural differential against the Windows journal dump, if any.
    journal="$JOURNAL_DIR/$base.journal.json"
    if [ -f "$journal" ]; then
        if ! python3 "$REPO/tests/tools/compare_logprint.py" \
                "$journal" "$DIR/$base.verify.logprint.txt"; then
            echo "  FAIL: xfs_logprint disagrees with the Windows journal"
            failures=$((failures + 1))
            continue
        fi
    fi

    # 4. The kernel itself replays a copy, then scrub/canary/unmount.
    sanity="$WORK/$base.sanity.img"
    cp --sparse=always "$image" "$sanity"
    if ! mount -o loop,nouuid "$sanity" "$MNT"; then
        echo "  FAIL: the Linux kernel could not replay the Windows dirty log"
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
        echo "  FAIL: canary write/read after kernel replay failed"
        failures=$((failures + 1))
        umount "$MNT" >/dev/null 2>&1 || umount -l "$MNT" >/dev/null 2>&1 || true
        continue
    fi
    if ! umount "$MNT"; then
        echo "  FAIL: clean unmount of the kernel-recovered image failed"
        failures=$((failures + 1))
        continue
    fi

    # 5. Repair and a final clean-log check on the kernel-recovered copy.
    if ! xfs_repair -n "$sanity" > "$DIR/$base.verify.repair.txt" 2>&1; then
        echo "  FAIL: xfs_repair -n reported errors after kernel replay"
        sed 's/^/    /' "$DIR/$base.verify.repair.txt" | head -40
        failures=$((failures + 1))
        continue
    fi
    if grep -Eiq 'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
            "$DIR/$base.verify.repair.txt"; then
        echo "  FAIL: xfs_repair -n was not silent after kernel replay"
        sed 's/^/    /' "$DIR/$base.verify.repair.txt" | head -40
        failures=$((failures + 1))
        continue
    fi
    xfs_logprint -t "$sanity" > "$DIR/$base.verify.logprint-after.txt" 2>&1 || true
    if ! grep -q 'state: <CLEAN>' "$DIR/$base.verify.logprint-after.txt"; then
        echo "  FAIL: log is not clean after kernel replay and unmount"
        failures=$((failures + 1))
        continue
    fi

    checked=$((checked + 1))
    echo "  ok (cancel + logprint agree, kernel replayed, scrub, canary, repair)"
done

echo ""
echo "$checked image(s) replayed by Linux, $failures failure(s)"
if [ "$failures" -ne 0 ]; then
    exit 1
fi
