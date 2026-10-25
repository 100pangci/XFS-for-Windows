#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Build a genuinely modern Linux fixture: plain `mkfs.xfs -f`, no parent=0,
# no hand-edited feature bits.  Refuses if the installed xfsprogs/kernel do
# not support parent pointers.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-rw6e-linux}"
SIZE_MB="${SIZE_MB:-512}"
mkdir -p "$OUT"

IMG="$OUT/rw6e.img"
MNT="$OUT/mnt"
MANIFEST="$OUT/rw6e.manifest.json"

bash "$ROOT/tests/scripts/qualify_rw6e_host.sh" "$OUT/host" || exit 1

truncate -s "${SIZE_MB}M" "$IMG"
# The whole point of RW-6E: no option disabling parent pointers.
mkfs.xfs -f "$IMG" | tee "$OUT/mkfs.txt"
mkfs.xfs -V | tee "$OUT/version.txt"
xfs_info "$IMG" | tee "$OUT/xfs_info.txt"
xfs_db -r -c 'sb 0' -c 'p' "$IMG" >"$OUT/sb.txt"

python3 - "$IMG" <<'PY' | tee "$OUT/features.txt"
import struct, sys
data = open(sys.argv[1], "rb").read(512)
incompat = struct.unpack_from(">I", data, 216)[0]
print("features_incompat=0x%08x" % incompat)
if not (incompat & (1 << 7)):
    raise SystemExit("plain mkfs.xfs did not enable parent pointers")
print("parent=1")
print("exchrange=%d" % (1 if incompat & (1 << 6) else 0))
PY

mkdir -p "$MNT"
mount -o loop "$IMG" "$MNT"
trap 'umount "$MNT" 2>/dev/null || true' EXIT
python3 "$ROOT/tests/tools/rw6e_linux_content.py" --make "$MNT" \
    --manifest "$MANIFEST"
sync
if command -v xfs_scrub >/dev/null 2>&1; then
  if ! xfs_scrub -n "$MNT" 2>&1 | tee "$OUT/xfs_scrub.txt"; then
    if grep -q 'Kernel metadata scrubbing facility is not available' \
        "$OUT/xfs_scrub.txt"; then
      echo "rw6e: kernel has no online scrub ioctl; recording a skip" \
        | tee -a "$OUT/xfs_scrub.txt"
    else
      echo "rw6e: xfs_scrub rejected the Linux fixture" >&2
      exit 1
    fi
  fi
fi
umount "$MNT"
trap - EXIT
xfs_repair -n "$IMG" | tee "$OUT/xfs_repair.txt"
echo "rw6e: Linux fixture ready at $IMG"
