#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Verify an image after Windows mutation through the real Linux kernel, the
# independent Python parent oracle, xfs_scrub (when available) and xfs_repair.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMG="${1:?usage: verify_rw6e_linux.sh <image> [manifest] [workdir]}"
MANIFEST="${2:-}"
WORK="${3:-$(mktemp -d)}"
mkdir -p "$WORK"
MNT="$WORK/mnt"
mkdir -p "$MNT"

xfs_repair -n "$IMG" | tee "$WORK/xfs_repair.txt"
python3 "$ROOT/tests/tools/rw6e_parent_probe.py" "$IMG" --require-parent \
    --json "$WORK/parent-oracle.json"

mount -o loop "$IMG" "$MNT"
trap 'umount "$MNT" 2>/dev/null || true' EXIT
find "$MNT" -maxdepth 6 -printf '%y %n %s %p -> %l\n' | sort \
    >"$WORK/find.txt"
if [ -n "$MANIFEST" ]; then
  python3 "$ROOT/tests/tools/rw6e_linux_content.py" --verify "$MNT" \
      --manifest "$MANIFEST"
fi
if command -v xfs_scrub >/dev/null 2>&1; then
  if ! xfs_scrub -n "$MNT" 2>&1 | tee "$WORK/xfs_scrub.txt"; then
    echo "rw6e: xfs_scrub rejected the image" >&2
    exit 1
  fi
fi
umount "$MNT"
trap - EXIT
# Repair again after the kernel has updated atime/ctime, if the image was
# mounted writable by any prior step.
xfs_repair -n "$IMG" | tee "$WORK/xfs_repair-final.txt"
echo "rw6e: Linux verification passed"
