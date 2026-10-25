#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Refuse to run the RW-6E qualification unless this host can actually exercise
# parent pointers.  This is intentionally strict: a repair-only run is not
# evidence for the writable gate.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${1:-$(mktemp -d)}"
mkdir -p "$WORK"

for tool in mkfs.xfs xfs_db xfs_info xfs_repair mount umount truncate python3; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "rw6e: missing required tool: $tool" >&2
    exit 1
  }
done

echo "== host =="
uname -a
echo "== xfsprogs =="
mkfs.xfs -V

PROBE="$WORK/parent-probe.img"
truncate -s 64M "$PROBE"
if ! mkfs.xfs -f -q -n parent=1 "$PROBE" >"$WORK/probe.mkfs.log" 2>&1; then
  echo "rw6e: mkfs.xfs does not support -n parent=1" >&2
  cat "$WORK/probe.mkfs.log" >&2
  exit 1
fi
rm -f "$PROBE"

if [ "$(id -u)" -ne 0 ]; then
  echo "rw6e: mount qualification requires root (or a user namespace with CAP_SYS_ADMIN)" >&2
  exit 1
fi

MNT="$WORK/mnt"
mkdir -p "$MNT"
truncate -s 64M "$WORK/kernel-probe.img"
mkfs.xfs -f -q -n parent=1 "$WORK/kernel-probe.img"
if ! mount -o loop "$WORK/kernel-probe.img" "$MNT"; then
  echo "rw6e: this kernel cannot mount a parent=1 XFS image; refusing to downgrade to xfs_repair-only evidence" >&2
  exit 1
fi
if ! touch "$MNT/kernel-ok"; then
  echo "rw6e: parent filesystem mounted read-only or is not writable" >&2
  umount "$MNT" || true
  exit 1
fi
sync
umount "$MNT"
echo "rw6e: host parent-pointer mount/format probe passed"
