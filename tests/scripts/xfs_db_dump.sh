#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Dump the metadata our Windows reader must agree with (xfs_db oracle).
#
#   tests/scripts/xfs_db_dump.sh <image> [path-prefix]
#
# Prints superblock/geometry, root inode, directory contents and inode details
# for every top-level entry. Use this output as the expected values when
# writing differential tests.

set -euo pipefail

img="${1:?usage: xfs_db_dump.sh <image>}"
shift || true

command -v xfs_db >/dev/null 2>&1 || { echo "missing xfs_db" >&2; exit 1; }

echo "=== superblock ==="
xfs_db -r -c 'sb 0' -c 'p' "$img"

rootino="$(xfs_db -r -c 'sb 0' -c 'p rootino' "$img" | awk -F'= ' '{print $2}' | awk '{print $1}')"
echo
echo "=== root inode = $rootino ==="
xfs_db -r -c "inode $rootino" -c 'p core' "$img"
xfs_db -r -c "inode $rootino" -c 'p u' "$img"

echo
echo "=== top-level entries ==="
# xfs_db directory listing prints inode/name pairs; duplicate lines are fine.
xfs_db -r -c "inode $rootino" -c 'p u' "$img" | sed -n '1,200p'
