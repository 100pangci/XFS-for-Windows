#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-5: verify images recovered by XFS for Windows, on Linux.
#
# For every <image>.img in the directory:
#   1. the independent probe must classify the journal as *clean* (Windows
#      recovery finished with an unmount record; Linux must not have to replay
#      anything),
#   2. xfs_repair -n must be silent (no metadata/log complaints),
#   3. the filesystem must mount and the namespace/content must match the
#      manifest produced before the crash capture (when present),
#   4. unmount and require a clean log again.
#
# usage: verify_recovery_linux.sh <dir> [<dir>...]
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
MOUNT=$(mktemp -d)
cleanup() {
    set +e
    mountpoint -q "$MOUNT" && umount -l "$MOUNT"
    rmdir "$MOUNT" 2>/dev/null
}
trap cleanup EXIT

FAILED=0
CHECKED=0
for dir in "$@"; do
    for image in "$dir"/*.img; do
        [ -e "$image" ] || continue
        name=$(basename "$image" .img)
        echo "== $name =="
        CHECKED=$((CHECKED + 1))

        state=$(python3 "$REPO/tests/tools/recovery_probe.py" "$image" | head -1 || true)
        if [ "$state" != "state: clean" ]; then
            echo "  FAIL: journal is not clean after recovery ($state)"
            FAILED=$((FAILED + 1))
            continue
        fi

        if ! xfs_repair -n "$image" > "$dir/$name.repair.txt" 2>&1; then
            echo "  FAIL: xfs_repair -n reported errors:"
            sed 's/^/    /' "$dir/$name.repair.txt" | head -40
            FAILED=$((FAILED + 1))
            continue
        fi
        # xfs_repair -n prints nothing for a clean filesystem; anything that
        # looks like corruption is a failure even if the exit status is 0.
        if grep -Eiq "corrupt|bad |error|warn" "$dir/$name.repair.txt"; then
            echo "  FAIL: xfs_repair -n was not silent:"
            sed 's/^/    /' "$dir/$name.repair.txt" | head -40
            FAILED=$((FAILED + 1))
            continue
        fi

        if ! mount -o loop,ro,nouuid "$image" "$MOUNT"; then
            echo "  FAIL: mount failed (Linux wanted to replay?)"
            FAILED=$((FAILED + 1))
            continue
        fi
        manifest="$dir/$name.manifest.json"
        if [ -e "$manifest" ]; then
            if ! python3 "$REPO/tests/tools/dirty_ops.py" verify "$MOUNT" "$manifest"; then
                echo "  FAIL: namespace/content does not match the manifest"
                FAILED=$((FAILED + 1))
                umount "$MOUNT"
                continue
            fi
        else
            # No manifest (Windows-generated crash images): at least prove the
            # root directory is readable.
            if ! ls "$MOUNT" >/dev/null; then
                echo "  FAIL: root directory is not readable"
                FAILED=$((FAILED + 1))
                umount "$MOUNT"
                continue
            fi
        fi
        umount "$MOUNT"

        state=$(python3 "$REPO/tests/tools/recovery_probe.py" "$image" | head -1 || true)
        if [ "$state" != "state: clean" ]; then
            echo "  FAIL: log is dirty after the read-only mount ($state)"
            FAILED=$((FAILED + 1))
            continue
        fi
        echo "  ok (clean journal, xfs_repair -n silent, manifest matches)"
    done
done

echo "$CHECKED image(s) checked, $FAILED failure(s)"
[ "$FAILED" -eq 0 ]
