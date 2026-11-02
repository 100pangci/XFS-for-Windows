#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# UX-1: the independent Linux XFS oracle for the read-write automount test.
#
# The Windows job leaves the VHDX copy it wrote through the WinFsp mount.  This
# script converts it back to a raw disk, runs xfs_repair -n, mounts it read-only
# and verifies the semantic result: the original fixture payload is unchanged
# and the files created/renamed/truncated/deleted through Windows have exactly
# the expected shape.  xfs_scrub -n runs when the distribution ships it.
#
# usage: verify_ux_rw_image_linux.sh <rw-ux-single.vhdx> <artifact-dir>

set -euo pipefail

IMAGE=${1:?usage: verify_ux_rw_image_linux.sh <image.vhdx> [artifact-dir]}
OUT=${2:-$(dirname "$IMAGE")}
WORK=$(mktemp -d)
MNT="$WORK/mnt"
LOOP=""

cleanup() {
    set +e
    mountpoint -q "$MNT" && umount -l "$MNT"
    if [ -n "$LOOP" ]; then losetup -d "$LOOP" >/dev/null 2>&1; fi
    rm -rf "$WORK"
}
trap cleanup EXIT

for tool in qemu-img losetup xfs_repair mount umount sha256sum; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

mkdir -p "$OUT" "$MNT"

echo "== validate the VHDX container"
qemu-img check -f vhdx "$IMAGE"

echo "== convert the VHDX to the raw disk"
qemu-img convert -f vhdx -O raw "$IMAGE" "$WORK/disk.raw"

echo "== map the GPT partitions"
LOOP=$(losetup --find --show --partscan "$WORK/disk.raw")
for _ in $(seq 1 40); do
    [ -b "${LOOP}p1" ] && break
    sleep 0.5
done
[ -b "${LOOP}p1" ] || { echo "partition 1 did not appear on $LOOP" >&2; exit 1; }

echo "== xfs_repair -n"
xfs_repair -n "${LOOP}p1" | tee "$OUT/xfs-repair.log"

echo "== mount read-only and verify the semantic state"
mount -o ro "${LOOP}p1" "$MNT"

pass=0
fail=0
check() {
    local label=$1
    shift
    if "$@"; then
        echo "PASS: $label"
        pass=$((pass + 1))
    else
        echo "FAIL: $label" >&2
        fail=$((fail + 1))
    fi
}

check "hello.txt content" \
    bash -c "test \"\$(tr -d '\r\n' < '$MNT/hello.txt')\" = 'hello from xfswin ux1'"
check "payload.bin unchanged" \
    bash -c "cd '$MNT' && sha256sum -c payload.sha256 >/dev/null"
check "ux1-rw directory exists" test -d "$MNT/ux1-rw"
check "alpha.bin was renamed away" test ! -e "$MNT/ux1-rw/alpha.bin"
check "beta.bin is 1000 bytes" \
    bash -c "test \"\$(stat -c %s '$MNT/ux1-rw/beta.bin')\" = 1000"
check "notes.txt content" \
    bash -c "test \"\$(tr -d '\r\n' < '$MNT/ux1-rw/notes.txt')\" = 'ux1 persistent read-write'"
check "delete-me.txt is gone" test ! -e "$MNT/ux1-rw/delete-me.txt"

if command -v xfs_scrub >/dev/null 2>&1; then
    echo "== xfs_scrub -n"
    if xfs_scrub -n "$MNT" >"$OUT/xfs-scrub.log" 2>&1; then
        echo "PASS: xfs_scrub -n"
        pass=$((pass + 1))
    else
        code=$?
        # 4 means the scrub found something it could not fix; report it, but
        # keep the log so the failure is diagnosable.
        echo "FAIL: xfs_scrub -n exited $code (see xfs-scrub.log)" >&2
        fail=$((fail + 1))
    fi
else
    echo "warning: xfs_scrub is not installed; skipping the scrubbing pass" \
        | tee "$OUT/xfs-scrub-skipped.txt"
fi

umount "$MNT"

cat >"$OUT/ux-rw-oracle.json" <<EOF
{
  "kind": "ux1-linux-rw-oracle",
  "image": "$(basename "$IMAGE")",
  "passed": $pass,
  "failed": $fail
}
EOF

if [ "$fail" -ne 0 ]; then
    echo "the Linux XFS oracle reported $fail failure(s)" >&2
    exit 1
fi
if [ "$pass" -eq 0 ]; then
    echo "the Linux XFS oracle ran no checks" >&2
    exit 1
fi
echo "Linux XFS oracle passed ($pass checks)"
