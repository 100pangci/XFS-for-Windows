#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - prepare the read-write interoperability image.
#
#   sudo tests/scripts/make_rw_image.sh <source.img> <outdir>
#
# Takes an image written by `xfswin format` (read-write safe feature profile)
# and populates it with the deterministic files and the overlay definition that
# tests/tools/rw_content.py describes. The Windows job then mounts the result
# read-write and overwrites bytes; Linux verifies the outcome with
# tests/scripts/verify_rw_linux.sh.
#
# Produces in <outdir>:
#   rw.img              the populated image
#   rw-overlay.bin      the bytes the Windows side must write
#   rw-recipe.json      paths/offsets of the overlays
#   rw-pristine.sha256  checksum of the populated image, before Windows writes
#
# Exit code 0 means the image is valid, populated and Linux-verified.

set -euo pipefail

source_img="${1:?usage: make_rw_image.sh <source.img> <outdir>}"
outdir="${2:?usage: make_rw_image.sh <source.img> <outdir>}"
source_img="$(readlink -f "$source_img")"
mkdir -p "$outdir"
outdir="$(readlink -f "$outdir")"

for tool in xfs_repair xfs_db mount umount python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool (install xfsprogs/util-linux)" >&2
        exit 1
    }
done
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools_dir="$(cd "$script_dir/../tools" && pwd)"

img="$outdir/rw.img"
cp --sparse=always "$source_img" "$img"

# Mount writable, populate, unmount. Root is required for the loop mount.
mnt="$(mktemp -d)"
cleanup() {
    if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
    rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT
mount -o loop "$img" "$mnt"
python3 "$tools_dir/rw_content.py" populate "$mnt"
sync
umount "$mnt"
trap - EXIT
rmdir "$mnt"

echo "== xfs_repair -n (populated) =="
if ! xfs_repair -n "$img" > "$outdir/rw-repair.log" 2>&1; then
    cat "$outdir/rw-repair.log"
    echo "FAILED: xfs_repair -n returned an error" >&2
    exit 1
fi
cat "$outdir/rw-repair.log"
if grep -qiE 'corruption|fatal|dirty|would fix|ALERT|valuable metadata' "$outdir/rw-repair.log"; then
    echo "FAILED: xfs_repair -n found problems" >&2
    exit 1
fi

# Read-only mount: the documented content must be there.
mnt="$(mktemp -d)"
trap cleanup EXIT
mount -o loop,ro "$img" "$mnt"
python3 "$tools_dir/rw_content.py" verify "$mnt"
ls -la "$mnt"
umount "$mnt"
trap - EXIT
rmdir "$mnt"

python3 "$tools_dir/rw_content.py" make-overlay "$outdir/rw-overlay.bin"
python3 "$tools_dir/rw_content.py" recipe "$outdir/rw-recipe.json"
sha256sum "$img" | awk '{print $1}' > "$outdir/rw-pristine.sha256"

echo "== prepared $img =="
cat "$outdir/rw-recipe.json"
echo "pristine sha256: $(cat "$outdir/rw-pristine.sha256")"
