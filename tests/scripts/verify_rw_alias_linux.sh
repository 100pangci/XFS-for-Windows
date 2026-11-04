#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-STABLE-1 alias equivalence: Linux oracle.
#
#   sudo tests/scripts/verify_rw_alias_linux.sh canonical.img alias.img manifest.json
#
# Both images must pass xfs_repair -n and mount read-only through the kernel.
# The content hashes recorded by the Windows equivalence test must match on
# both images, so the two spellings are not only accepted but semantically
# identical to the Linux reader.

set -euo pipefail

canonical="${1:?usage: verify_rw_alias_linux.sh <canonical.img> <alias.img> <manifest.json>}"
alias_image="${2:?missing alias image}"
manifest="${3:?missing manifest}"

for tool in xfs_repair mount umount sha256sum readlink python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool" >&2
        exit 1
    }
done

expected="$(mktemp)"
trap 'rm -f "$expected"' EXIT
python3 - "$manifest" > "$expected" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
for path, digest in data["sha256"].items():
    # Get-FileHash prints uppercase hex; sha256sum prints lowercase.
    print(path, digest.lower())
PY
if [ ! -s "$expected" ]; then
    echo "FAILED: the manifest contains no content hashes" >&2
    exit 1
fi

verify_image() {
    local label="$1"
    local image="$2"
    echo "== $label: xfs_repair -n =="
    xfs_repair -n "$image" > "/tmp/rw-alias-$label-repair.log" 2>&1 || {
        cat "/tmp/rw-alias-$label-repair.log"
        echo "FAILED: xfs_repair -n rejected $label" >&2
        exit 1
    }
    cat "/tmp/rw-alias-$label-repair.log"
    if grep -qiE 'corruption|fatal|would fix' \
            "/tmp/rw-alias-$label-repair.log"; then
        echo "FAILED: xfs_repair -n found problems in $label" >&2
        exit 1
    fi

    echo "== $label: read-only kernel mount =="
    local mnt
    mnt="$(mktemp -d)"
    mount -o loop,ro "$image" "$mnt"
    local failed=0
    while read -r path digest; do
        if [ -L "$mnt$path" ]; then
            # Do not hash through the link: the workload's relative target
            # climbs above the mountpoint when the image is mounted at a
            # subdirectory, which is valid only when the image is the root.
            # The stored target is checked explicitly below, and the Windows
            # equivalence test already compared the followed content.
            continue
        fi
        actual="$(sha256sum "$mnt$path" | awk '{print tolower($1)}')"
        if [ "$actual" != "$digest" ]; then
            echo "FAILED: $label $path is $actual, expected $digest" >&2
            failed=1
        fi
    done < "$expected"
    link_target="$(readlink "$mnt/link")"
    if [ "$link_target" != "../dir/src" ]; then
        echo "FAILED: $label /link points at '$link_target'" >&2
        failed=1
    fi
    umount "$mnt"
    rmdir "$mnt"
    if [ "$failed" -ne 0 ]; then
        exit 1
    fi

    echo "== $label: xfs_repair -n after the read-only pass =="
    xfs_repair -n "$image" > "/tmp/rw-alias-$label-repair-after.log" 2>&1 || {
        cat "/tmp/rw-alias-$label-repair-after.log"
        echo "FAILED: the read-only pass left $label rejected" >&2
        exit 1
    }
    if grep -qiE 'corruption|fatal|would fix' \
            "/tmp/rw-alias-$label-repair-after.log"; then
        echo "FAILED: the read-only pass left $label damaged" >&2
        exit 1
    fi
}

verify_image canonical "$canonical"
verify_image alias "$alias_image"

echo "OK: both spellings are Linux-accepted and semantically identical"
