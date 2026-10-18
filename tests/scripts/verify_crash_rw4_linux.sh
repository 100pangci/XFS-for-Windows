#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - Linux replay of the RW-4 crash matrix.
#
#   sudo tests/scripts/verify_crash_rw4_linux.sh <crash-dir>
#
# <crash-dir> is the output of tests/scripts/verify_crash_rw4_windows.ps1.
# Every image is a Windows process that died at some crash point during a
# rename, hard link or symlink operation; the log is dirty and only a real
# kernel replay can decide which transactions happened.
#
# For every image:
#   1. mount it (the kernel replays the journal natively),
#   2. the mounted namespace must be exactly the old or the new state
#      (tests/tools/rw4_crash_replay_check.py),
#   3. unmount, then the structural oracle (rw4_probe.py) and xfs_repair -n
#      must be silent and the log must be clean.
set -euo pipefail

dir="${1:?usage: verify_crash_rw4_linux.sh <crash-dir>}"
dir="$(readlink -f "$dir")"
for tool in mount umount xfs_repair python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool (install xfsprogs/util-linux)" >&2
        exit 1
    }
done
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools_dir="$(cd "$script_dir/../tools" && pwd)"
manifest="$dir/manifest.json"

failures=0
work="$(mktemp -d)"
mnt="$(mktemp -d)"
cleanup() {
    if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
    rm -rf "$work" "$mnt"
}
trap cleanup EXIT

python3 - "$manifest" <<'PY' > "$work/cases.txt"
import json, sys
with open(sys.argv[1], encoding="utf-8-sig") as f:
    manifest = json.load(f)
for case in manifest["cases"]:
    print(case["file"])
PY
count=0
while IFS= read -r file; do
    [ -n "$file" ] || continue
    count=$((count + 1))
    img="$work/$file"
    cp "$dir/$file" "$img"

    # 1. Replay: the kernel applies every committed transaction.
    if ! mount -o loop,nouuid "$img" "$mnt"; then
        echo "FAILED: cannot mount (replay) $file" >&2
        dmesg | tail -30 || true
        failures=1
        continue
    fi
    # 2. The visible namespace is one of the two legal states.
    if ! python3 "$tools_dir/rw4_crash_replay_check.py" "$mnt" "$manifest" \
            "$file"; then
        echo "FAILED: illegal post-replay state in $file" >&2
        failures=1
    fi
    umount "$mnt"

    # 3. The replayed image must be structurally perfect.
    if ! python3 "$tools_dir/rw4_probe.py" "$img" --quiet; then
        echo "FAILED: oracle rejected the replayed $file" >&2
        failures=1
    fi
    if ! xfs_repair -n "$img" >/dev/null 2>&1; then
        echo "FAILED: xfs_repair -n rejected the replayed $file" >&2
        failures=1
    fi
    if ! python3 "$tools_dir/log_probe.py" "$img" --expect clean \
            >/dev/null; then
        echo "FAILED: the log is not clean after replay of $file" >&2
        failures=1
    fi
done < "$work/cases.txt"

if [ "$failures" != "0" ]; then
    echo "RW-4 CRASH REPLAY VERIFICATION FAILED" >&2
    exit 1
fi
echo "OK: every RW-4 crash image replayed by Linux into a legal namespace"
