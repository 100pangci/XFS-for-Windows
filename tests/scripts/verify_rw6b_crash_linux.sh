#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B Linux replay oracle for the writer crash matrix.
#
# Every archived crash image is mounted read-write so the kernel replays its
# log, then unmounted and checked with xfs_repair -n and the independent
# forward-derived rmap ownership oracle.  The log must be clean afterwards.
#
# usage: verify_rw6b_crash_linux.sh <crash-dir>
set -euo pipefail

DIR=${1:?usage: verify_rw6b_crash_linux.sh <crash-dir>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
MNT="$WORK/mnt"
mkdir -p "$MNT"
cleanup() {
    set +e
    mountpoint -q "$MNT" && umount -l "$MNT"
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ ! -f "$DIR/cases.json" ]; then
    echo "missing $DIR/cases.json" >&2
    exit 1
fi
expected=$(python3 -c 'import json,sys;print(len(json.load(open(sys.argv[1]))))' \
    "$DIR/cases.json")
if [ "$expected" -eq 0 ]; then
    echo "crash manifest is empty" >&2
    exit 1
fi

checked=0
failed=0
while IFS= read -r archive; do
    [ -e "$DIR/$archive" ] || { echo "missing archive: $archive" >&2; failed=$((failed + 1)); continue; }
    rm -rf "$WORK/current"
    mkdir -p "$WORK/current"
    unzip -q -o "$DIR/$archive" -d "$WORK/current"
    image=$(find "$WORK/current" -name '*.img' -print -quit)
    if [ -z "$image" ]; then
        echo "FAIL $archive: no image" >&2
        failed=$((failed + 1))
        continue
    fi
    name=$(basename "$image" .img)
    echo "== $name =="
    if ! mount -o loop,nouuid "$image" "$MNT"; then
        echo "  FAIL: kernel replay mount failed" >&2
        failed=$((failed + 1))
        continue
    fi
    umount "$MNT"
    if ! xfs_repair -n "$image" > "$WORK/repair.txt" 2>&1; then
        echo "  FAIL: xfs_repair -n failed" >&2
        sed 's/^/    /' "$WORK/repair.txt" | head -40 >&2
        failed=$((failed + 1))
        continue
    fi
    if grep -Eiq 'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
            "$WORK/repair.txt"; then
        echo "  FAIL: xfs_repair -n was not silent" >&2
        sed 's/^/    /' "$WORK/repair.txt" | head -40 >&2
        failed=$((failed + 1))
        continue
    fi
    if ! state=$(python3 "$REPO/tests/tools/recovery_probe.py" "$image" |
            sed -n '1p'); then
        echo "  FAIL: cannot parse the replayed log" >&2
        failed=$((failed + 1))
        continue
    fi
    if [ "$state" != "state: clean" ]; then
        echo "  FAIL: log is not clean after kernel replay ($state)" >&2
        failed=$((failed + 1))
        continue
    fi
    if ! python3 "$REPO/tests/tools/rw6b_rmap_probe.py" "$image" --quiet; then
        echo "  FAIL: rmap ownership oracle rejected the replayed image" >&2
        failed=$((failed + 1))
        continue
    fi
    checked=$((checked + 1))
done < <(python3 -c '
import json, sys
for case in json.load(open(sys.argv[1])):
    print(case["file"])
' "$DIR/cases.json")

if [ "$checked" -ne "$expected" ] || [ "$failed" -ne 0 ]; then
    echo "RW-6B crash replay FAILED ($failed failures, $checked/$expected clean)" >&2
    exit 1
fi
echo "RW-6B crash replay OK ($checked images)"
