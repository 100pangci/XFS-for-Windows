#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Verify the exact namespace and content that the Windows RW-6E round trip
# adds to a Linux-created parent-pointer image.  The original Linux workload
# manifest is checked separately by verify_rw6e_linux.sh; this script checks
# the Windows-created entries through the real Linux kernel.
set -euo pipefail

IMG="${1:?usage: verify_rw6e_windows_linux.sh <image> [workdir]}"
WORK="${2:-$(mktemp -d)}"
MNT="$WORK/mnt"
mkdir -p "$MNT"

umount_retry() {
  local mp="$1"
  local attempt
  for attempt in 1 2 3 4 5; do
    if umount "$mp" 2>/dev/null; then
      return 0
    fi
    sleep 1
  done
  echo "rw6e windows verification: cannot unmount $mp" >&2
  findmnt -T "$mp" || true
  if command -v fuser >/dev/null 2>&1; then
    fuser -vm "$mp" || true
  fi
  umount -l "$mp"
}

mount -o loop "$IMG" "$MNT"
trap 'umount_retry "$MNT" 2>/dev/null || true' EXIT

fail() {
  echo "rw6e windows verification: $*" >&2
  exit 1
}

[ -f "$MNT/windows.bin" ] || fail "missing /windows.bin"
[ "$(cat "$MNT/windows.bin")" = "WINDOWS-RW6E" ] || \
  fail "/windows.bin has the wrong content"

[ -d "$MNT/win-move/dir" ] || fail "missing /win-move/dir"
[ ! -e "$MNT/win-dir" ] || fail "/win-dir still exists after the directory move"
[ -f "$MNT/win-move/dir/child" ] || fail "missing /win-move/dir/child"
[ "$(cat "$MNT/win-move/dir/child")" = "CHILD" ] || \
  fail "/win-move/dir/child has the wrong content"
[ ! -e "$MNT/win-move/dir/after" ] || \
  fail "the unlinked /win-move/dir/after is still present"
[ ! -e "$MNT/win-delete" ] || fail "the unlinked /win-delete is still present"
[ ! -e "$MNT/win-empty" ] || fail "the removed /win-empty is still present"

[ -f "$MNT/win-victim" ] || fail "missing the replacement target /win-victim"
[ "$(cat "$MNT/win-victim")" = "REPLACED" ] || \
  fail "/win-victim does not contain the replacement source"
[ ! -e "$MNT/win-source" ] || fail "/win-source survived the replace rename"

child_ino=$(stat -c '%i' "$MNT/win-move/dir/child")
link_ino=$(stat -c '%i' "$MNT/win-move/dir/renamed")
[ "$child_ino" = "$link_ino" ] || fail "the hard link points at a different inode"
[ "$(stat -c '%h' "$MNT/win-move/dir/child")" = "2" ] || \
  fail "the hard-linked child does not have nlink=2"
[ "$(stat -c '%h' "$MNT/win-move/dir/renamed")" = "2" ] || \
  fail "the renamed hard link does not have nlink=2"

[ -L "$MNT/win-move/link" ] || fail "missing the Windows-created symlink"
[ "$(readlink "$MNT/win-move/link")" = "target" ] || \
  fail "the Windows-created symlink target changed"

python3 - "$MNT/win-move/dir/child" <<'PY'
import os
import sys

value = os.getxattr(sys.argv[1], "user.ci")
if value != b"hello":
    raise SystemExit("user.ci is %r, expected b'hello'" % (value,))
PY

sync
umount_retry "$MNT"
trap - EXIT
echo "rw6e: Windows-added namespace and content verified on Linux"
