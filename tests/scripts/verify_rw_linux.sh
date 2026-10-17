#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - verify a read-write modified image against Linux oracles.
#
#   sudo tests/scripts/verify_rw_linux.sh <modified.img> <pristine.img> \
#        <recipe.json> <overlay.bin>
#
# The Windows job overwrote bytes inside two existing files (once through the
# WinFsp Write callback, once through `xfswin pwrite`). This script proves that
# the result is exactly that and nothing more:
#
#   1. xfs_repair -n must stay clean (metadata, CRCs, log, free space),
#   2. the modified files must match the documented content with the overlays
#      applied (regenerated here, not taken from Windows),
#   3. every other file must be byte-identical to the pristine image,
#   4. the log must still be clean (no replay needed).
#
# Exit code 0 means the Windows write path produced a Linux-consistent result.

set -euo pipefail

modified="${1:?usage: verify_rw_linux.sh <modified.img> <pristine.img> <recipe.json> <overlay.bin>}"
pristine="${2:?usage: verify_rw_linux.sh <modified.img> <pristine.img> <recipe.json> <overlay.bin>}"
recipe="${3:?usage: verify_rw_linux.sh <modified.img> <pristine.img> <recipe.json> <overlay.bin>}"
overlay="${4:?usage: verify_rw_linux.sh <modified.img> <pristine.img> <recipe.json> <overlay.bin>}"
modified="$(readlink -f "$modified")"
pristine="$(readlink -f "$pristine")"
recipe="$(readlink -f "$recipe")"
overlay="$(readlink -f "$overlay")"

for tool in xfs_repair xfs_db mount umount python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool (install xfsprogs/util-linux)" >&2
        exit 1
    }
done
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools_dir="$(cd "$script_dir/../tools" && pwd)"

# The overlay the Windows side used must be the one we expect.
expected_overlay="$(mktemp)"
python3 "$tools_dir/rw_content.py" make-overlay "$expected_overlay" >/dev/null
if ! cmp -s "$expected_overlay" "$overlay"; then
    echo "FAILED: overlay bytes differ from tests/tools/rw_content.py" >&2
    cmp -l "$expected_overlay" "$overlay" | head -5 >&2 || true
    rm -f "$expected_overlay"
    exit 1
fi
rm -f "$expected_overlay"

echo "== image sizes =="
echo "pristine: $(stat -c %s "$pristine") bytes"
echo "modified: $(stat -c %s "$modified") bytes"
if [ "$(stat -c %s "$pristine")" != "$(stat -c %s "$modified")" ]; then
    echo "FAILED: the modified image changed size" >&2
    exit 1
fi

echo "== xfs_db: superblock and root inode =="
xfs_db -r -c 'sb 0' -c 'p rootino versionnum features_ro_compat features_incompat' "$modified"
rootino="$(xfs_db -r -c 'sb 0' -c 'p rootino' "$modified" | awk -F'= ' '{print $2}' | awk '{print $1}')"
xfs_db -r -c "inode $rootino" -c 'p core' "$modified"

echo "== xfs_repair -n (modified) =="
repair_log="$(mktemp)"
if ! xfs_repair -n "$modified" > "$repair_log" 2>&1; then
    cat "$repair_log"
    echo "FAILED: xfs_repair -n returned an error" >&2
    rm -f "$repair_log"
    exit 1
fi
cat "$repair_log"
if grep -qiE 'corruption|fatal|would fix|dirty log' "$repair_log"; then
    echo "FAILED: xfs_repair -n found problems" >&2
    rm -f "$repair_log"
    exit 1
fi
rm -f "$repair_log"

# Mount both images read-only and compare every file.
pristine_mnt="$(mktemp -d)"
modified_mnt="$(mktemp -d)"
cleanup() {
    if mountpoint -q "$modified_mnt"; then umount "$modified_mnt" || true; fi
    if mountpoint -q "$pristine_mnt"; then umount "$pristine_mnt" || true; fi
    rmdir "$pristine_mnt" "$modified_mnt" 2>/dev/null || true
}
trap cleanup EXIT
mount -o loop,ro "$pristine" "$pristine_mnt"
mount -o loop,ro "$modified" "$modified_mnt"

echo "== documented content with the overlays applied =="
python3 "$tools_dir/rw_content.py" verify "$modified_mnt" --with-overlays

echo "== every unmodified file must be byte-identical =="
overlay_paths="$(python3 -c '
import json,sys
recipe=json.load(open(sys.argv[1]))
print(" ".join(o["path"] for o in recipe["overlays"]))
' "$recipe")"
failed=0
while IFS= read -r -d '' file; do
    rel="${file#"$modified_mnt"/}"
    expect="$pristine_mnt/$rel"
    skip=0
    for overlay_path in $overlay_paths; do
        if [ "$rel" = "$overlay_path" ]; then skip=1; fi
    done
    if [ "$skip" = "1" ]; then continue; fi
    if [ ! -f "$expect" ]; then
        echo "FAILED: $rel exists only in the modified image" >&2
        failed=1
    elif ! cmp -s "$file" "$expect"; then
        echo "FAILED: $rel differs from the pristine image" >&2
        failed=1
    fi
done < <(find "$modified_mnt" -type f -print0)
if [ "$failed" != "0" ]; then
    exit 1
fi
echo "all other files are byte-identical"

echo "== directory listings =="
if ! diff <(cd "$pristine_mnt" && find . | sort) \
          <(cd "$modified_mnt" && find . | sort); then
    echo "FAILED: the directory trees differ" >&2
    exit 1
fi
echo "directory trees are identical"

umount "$modified_mnt"
umount "$pristine_mnt"
trap - EXIT
rmdir "$pristine_mnt" "$modified_mnt"

echo "OK: read-write modification is exactly the intended in-place overwrite"
