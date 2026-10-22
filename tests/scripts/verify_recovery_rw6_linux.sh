#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6A: Linux verification of modern-inode images recovered by Windows.
# A read-only mount only succeeds when the log is clean; xfs_repair -n is the
# structural oracle; the dirty-log manifest is the semantic oracle.
#
# usage: verify_recovery_rw6_linux.sh <dirty-dir> <recovered-dir>
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
DIRTY=${1:?usage: verify_recovery_rw6_linux.sh <dirty-dir> <recovered-dir>}
RECOVERED=${2:?usage: verify_recovery_rw6_linux.sh <dirty-dir> <recovered-dir>}
MOUNT=$(mktemp -d)
cleanup() {
    set +e
    mountpoint -q "$MOUNT" && umount -l "$MOUNT"
    rmdir "$MOUNT" 2>/dev/null
}
trap cleanup EXIT

FAILED=0
COUNT=0
for image in "$RECOVERED"/*.img; do
    [ -e "$image" ] || continue
    COUNT=$((COUNT + 1))
    name=$(basename "$image" .img)
    echo "== $name =="
    if ! xfs_repair -n "$image" > "$RECOVERED/$name.repair.txt" 2>&1; then
        echo "  FAIL: xfs_repair -n failed"
        sed 's/^/    /' "$RECOVERED/$name.repair.txt" | head -40
        FAILED=$((FAILED + 1))
        continue
    fi
    if grep -Eiq "corrupt|bad |error|warn" "$RECOVERED/$name.repair.txt"; then
        echo "  FAIL: xfs_repair -n was not silent"
        sed 's/^/    /' "$RECOVERED/$name.repair.txt" | head -40
        FAILED=$((FAILED + 1))
        continue
    fi
    if ! mount -o loop,ro,nouuid "$image" "$MOUNT"; then
        echo "  FAIL: read-only mount failed (log not clean?)"
        FAILED=$((FAILED + 1))
        continue
    fi
    manifest="$DIRTY/$name.manifest.json"
    if [ -e "$manifest" ]; then
        if ! python3 "$REPO/tests/tools/dirty_ops.py" verify "$MOUNT" "$manifest"; then
            echo "  FAIL: recovered namespace/content does not match the manifest"
            FAILED=$((FAILED + 1))
            umount "$MOUNT"
            continue
        fi
    else
        ls "$MOUNT" >/dev/null
    fi
    umount "$MOUNT"
    echo "  ok"
done
if [ "$COUNT" -eq 0 ]; then
    echo "no recovered images" >&2
    exit 1
fi
if [ "$FAILED" -ne 0 ]; then
    echo "RW-6A recovered-image verification FAILED ($FAILED/$COUNT)"
    exit 1
fi
echo "RW-6A recovered-image verification OK ($COUNT images)"
