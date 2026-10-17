#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - verify the test fixture image on Linux.
#
#   sudo tests/scripts/verify_fixture_linux.sh <fixture.img>
#
# The fixture (written by `xfs_tests --make-fixture`) is the image the Windows
# read-write unit tests use: a filesystem with one file whose blocks a *test*
# builder allocated. Those tests are only meaningful if the image is a real
# XFS filesystem, so the same image is handed to the Linux tools here:
#
#   1. xfs_repair -n must be clean,
#   2. xfs_db must agree on the inode (size, blocks, extent),
#   3. a read-only mount must expose /hello.bin with the documented content,
#   4. the log must still be clean.
#
# Exit code 0 means the fixture is a valid filesystem that Linux accepts.

set -euo pipefail

img="${1:?usage: verify_fixture_linux.sh <fixture.img>}"
img="$(readlink -f "$img")"

for tool in xfs_repair xfs_db mount umount python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool (install xfsprogs/util-linux)" >&2
        exit 1
    }
done
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools_dir="$(cd "$script_dir/../tools" && pwd)"

echo "== structural probe (independent implementation) =="
python3 "$tools_dir/fixture_probe.py" "$img"

echo "== xfs_repair -n =="
repair_log="$(mktemp)"
if ! xfs_repair -n "$img" > "$repair_log" 2>&1; then
    cat "$repair_log"
    echo "FAILED: xfs_repair -n returned an error" >&2
    rm -f "$repair_log"
    exit 1
fi
cat "$repair_log"
if grep -qiE 'corruption|fatal|dirty|would fix|ALERT|valuable metadata' "$repair_log"; then
    echo "FAILED: xfs_repair -n found problems" >&2
    rm -f "$repair_log"
    exit 1
fi
rm -f "$repair_log"

echo "== xfs_db: file inode =="
xfs_db -r -c 'inode 67' -c 'p core.magic core.mode core.version core.format core.size core.nblocks core.nextents' "$img"
xfs_db -r -c 'inode 67' -c 'bmap' "$img"

echo "== read-only mount and content check =="
mnt="$(mktemp -d)"
cleanup() {
    if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
    rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT
mount -o loop,ro,nouuid "$img" "$mnt"
ls -la "$mnt"
python3 - "$mnt" <<'PY'
import sys
path = sys.argv[1] + "/hello.bin"
data = open(path, "rb").read()
assert len(data) == 32768, f"size {len(data)}"
expected = bytes(((i * 167) ^ (i >> 7) ^ 0x5A) & 0xFF for i in range(len(data)))
assert data == expected, "content does not match the documented pattern"
print(f"content OK: {len(data)} bytes")
PY
umount "$mnt"
trap - EXIT
rmdir "$mnt"

echo "== log record =="
xfs_db -r -c 'logformat' "$img" 2>/dev/null | head -20 || true

echo "OK: the fixture image is a valid XFS filesystem Linux accepts"
