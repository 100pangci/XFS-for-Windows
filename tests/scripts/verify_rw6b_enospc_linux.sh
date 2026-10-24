#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B near-ENOSPC qualification, Linux side.
#
# The Windows pass fills a Linux-created rmapbt image until a 1 MiB fallocate
# is refused with a genuine free-space error.  This script verifies that:
#
#   * the image still passes xfs_repair -n and the forward-derived oracle;
#   * every successful allocation is exactly 1 MiB of allocated blocks;
#   * the refused allocation left a zero-length, zero-extent file;
#   * free-space accounting matches the Windows manifest;
#   * a writable copy can free space and then accept normal Linux operations.
#
#   sudo bash tests/scripts/verify_rw6b_enospc_linux.sh <image> <manifest.json>
set -euo pipefail

img="${1:?usage: verify_rw6b_enospc_linux.sh <image> <enospc.json>}"
manifest="${2:?usage: verify_rw6b_enospc_linux.sh <image> <enospc.json>}"
img="$(readlink -f "$img")"
manifest="$(readlink -f "$manifest")"
repo="$(cd "$(dirname "$0")/../.." && pwd)"
python3 - "$manifest" <<'PY'
import json
import sys

d = json.load(open(sys.argv[1]))
for key in ("block_size", "fill_bytes", "free_blocks_before",
            "free_blocks_after", "successful_allocations", "failed_index",
            "failed_path", "failed_exit", "failure"):
    if key not in d:
        raise SystemExit("manifest missing %s" % key)
if int(d["successful_allocations"]) < 1:
    raise SystemExit("no successful allocations recorded")
if int(d["failed_index"]) != int(d["successful_allocations"]):
    raise SystemExit("failed index does not follow the last success")
if not any(word in d["failure"].lower()
           for word in ("no space", "enospc", "free space", "nospace")):
    raise SystemExit("refusal was not a free-space error: %r" % d["failure"])
PY

for tool in xfs_db xfs_repair xfs_bmap mount umount stat find dd sync awk; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool" >&2
        exit 1
    }
done

# Sum allocated data coverage in bytes, including unwritten preallocation.
# xfs_bmap offsets are always 512-byte basic blocks, NOT filesystem blocks.
# stat %b also counts bmbt blocks; holes/delalloc are not allocated coverage.
allocated_bytes() {
    xfs_bmap -p "$1" | awk -F'[][]' '
        /\[/ && !/hole|delalloc/ {
            if (split($2, range, /[.][.]/) == 2)
                total += range[2] - range[1] + 1;
        }
        END { printf "%.0f\n", total * 512 }'
}

echo "== xfs_db: superblock =="
db_out="$(xfs_db -r -c 'sb 0' -c 'p' "$img" 2>&1)"
echo "$db_out" | sed -n '1,45p'
if echo "$db_out" | grep -qiE 'sanity check failed|corruption|crc error'; then
    echo "FAILED: xfs_db reported superblock problems" >&2
    exit 1
fi

echo "== xfs_repair -n =="
if ! xfs_repair -n "$img" > /tmp/rw6b-enospc-repair.log 2>&1; then
    cat /tmp/rw6b-enospc-repair.log
    echo "FAILED: xfs_repair -n returned an error" >&2
    exit 1
fi
cat /tmp/rw6b-enospc-repair.log
if grep -qiE 'corruption|fatal|would fix' /tmp/rw6b-enospc-repair.log; then
    echo "FAILED: xfs_repair -n found problems" >&2
    exit 1
fi

echo "== independent ownership oracle =="
python3 "$repo/tests/tools/rw6b_rmap_probe.py" "$img" --quiet
echo "oracle: clean"

block_size="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["block_size"])' "$manifest")"
fill_bytes="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["fill_bytes"])' "$manifest")"
free_before="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["free_blocks_before"])' "$manifest")"
free_after="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["free_blocks_after"])' "$manifest")"
succeeded="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["successful_allocations"])' "$manifest")"
failed_index="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["failed_index"])' "$manifest")"
failed_path="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["failed_path"])' "$manifest")"

# Compare like-for-like disk counters before mounting. Linux statfs excludes
# the allocator set-aside and non-root bnobt/cntbt blocks; Windows info reports
# sb_fdblocks. These are different quantities, not a permissible error margin.
ag_count="$(xfs_db -r -c 'sb 0' -c 'p agcount' "$img" | awk '$1 == "agcount" {print $3}')"
disk_free="$(xfs_db -r -c 'sb 0' -c 'p fdblocks' "$img" | awk '$1 == "fdblocks" {print $3}')"
ag_free=0
allocbt_blocks=0
for ((ag = 0; ag < ag_count; ag++)); do
    read -r contribution allocbt < <(
        xfs_db -r -c "agf $ag" \
            -c 'p freeblks flcount btreeblks rmapblocks' "$img" |
        awk '/ = / {v[$1] = $3}
             END {print v["freeblks"] + v["flcount"] + v["btreeblks"],
                        v["btreeblks"] - (v["rmapblocks"] - 1)}')
    if [ "$allocbt" -lt 0 ]; then
        echo "FAILED: AG$ag free-space btree block count is negative" >&2
        exit 1
    fi
    ag_free=$((ag_free + contribution))
    allocbt_blocks=$((allocbt_blocks + allocbt))
done
if [ "$disk_free" -ne "$free_after" ] || [ "$ag_free" -ne "$free_after" ]; then
    echo "FAILED: disk free blocks SB=$disk_free AGs=$ag_free != manifest $free_after" >&2
    exit 1
fi
# xfs_alloc_set_aside = agcount * (XFS_ALLOCBT_AGFL_RESERVE + 4), reserve=4.
# xfs_fs_statfs subtracts this plus m_allocbt_blks on a read-only mount.
statfs_free=$((free_after - 8 * ag_count - allocbt_blocks))
if [ "$statfs_free" -lt 0 ]; then statfs_free=0; fi

echo "== read-only mount: accepted tree =="
mnt="$(mktemp -d)"
cleanup() {
    if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
}
trap cleanup EXIT
mount -o loop,ro "$img" "$mnt"

fill="$(printf '%s' "$failed_path" | sed 's|/a[0-9]*\.bin$||')"
if [ ! -f "$mnt$fill/fill.bin" ]; then
    echo "FAILED: fill.bin is missing under $fill" >&2
    exit 1
fi
fill_size="$(stat -c %s "$mnt$fill/fill.bin")"
fill_allocated="$(allocated_bytes "$mnt$fill/fill.bin")"
if [ "$fill_size" -ne "$fill_bytes" ]; then
    echo "FAILED: fill.bin size $fill_size != $fill_bytes" >&2
    exit 1
fi
if [ "$fill_allocated" -ne "$fill_bytes" ]; then
    echo "FAILED: fill.bin covers $fill_allocated bytes, expected $fill_bytes" >&2
    exit 1
fi

count="$(find "$mnt$fill" -maxdepth 1 -name 'a[0-9][0-9][0-9][0-9].bin' | wc -l)"
if [ "$count" -ne $((succeeded + 1)) ]; then
    echo "FAILED: $count allocation files, expected $((succeeded + 1))" >&2
    exit 1
fi
for ((i = 0; i < succeeded; i++)); do
    path="$(printf '%s/a%04d.bin' "$fill" "$i")"
    size="$(stat -c %s "$mnt$path")"
    allocated="$(allocated_bytes "$mnt$path")"
    if [ "$size" -ne 1048576 ] || [ "$allocated" -ne 1048576 ]; then
        echo "FAILED: $path is not a fully allocated 1 MiB file " \
             "(size $size, $allocated allocated bytes)" >&2
        exit 1
    fi
done
failed_file="$(printf '%s/a%04d.bin' "$fill" "$failed_index")"
if [ "$(stat -c %s "$mnt$failed_file")" -ne 0 ] ||
   [ "$(allocated_bytes "$mnt$failed_file")" -ne 0 ]; then
    echo "FAILED: refused allocation left data in $failed_file" >&2
    exit 1
fi
free_now="$(stat -f -c %f "$mnt")"
fs_bsize="$(stat -f -c %S "$mnt")"
if [ "$fs_bsize" -ne "$block_size" ]; then
    echo "FAILED: mounted block size $fs_bsize != $block_size" >&2
    exit 1
fi
if [ "$free_now" -ne "$statfs_free" ]; then
    echo "FAILED: mounted free blocks $free_now != expected $statfs_free " \
         "(disk $free_after, AG set-aside $((8 * ag_count)), allocbt $allocbt_blocks)" >&2
    exit 1
fi
if [ "$free_after" -ge "$free_before" ]; then
    echo "FAILED: filling did not reduce free space" >&2
    exit 1
fi
echo "enospc tree: $succeeded x 1 MiB, refused $failed_file empty, free=$free_now blocks"
umount "$mnt"
trap - EXIT

echo "== writable copy: free space with Linux and write again =="
copy="$(mktemp /tmp/rw6b-enospc-copy-XXXXXX.img)"
cleanup_copy() {
    if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
    rm -f "$copy"
}
trap cleanup_copy EXIT
cp --sparse=always "$img" "$copy"
rmdir "$mnt"
mnt="$(mktemp -d)"
mount -o loop "$copy" "$mnt"
for ((i = 0; i < 32; i++)); do
    rm -f "$(printf '%s/a%04d.bin' "$mnt$fill" "$i")"
done
sync
dd if=/dev/urandom of="$mnt$fill/linux-write.bin" bs=1M count=16 status=none
mkdir -p "$mnt$fill/linux-dir/sub"
ln -s /enospc "$mnt$fill/linux-link"
ln "$mnt$fill/linux-write.bin" "$mnt$fill/linux-hard"
mv "$mnt$fill/linux-hard" "$mnt$fill/linux-dir/moved.bin"
sync
umount "$mnt"
trap - EXIT

echo "== xfs_repair -n after the Linux writable pass =="
if ! xfs_repair -n "$copy" > /tmp/rw6b-enospc-repair-after.log 2>&1; then
    cat /tmp/rw6b-enospc-repair-after.log
    echo "FAILED: xfs_repair -n rejected the Linux-modified copy" >&2
    exit 1
fi
cat /tmp/rw6b-enospc-repair-after.log
if grep -qiE 'corruption|fatal|would fix' /tmp/rw6b-enospc-repair-after.log; then
    echo "FAILED: the Linux writable pass left repairable damage" >&2
    exit 1
fi
python3 "$repo/tests/tools/rw6b_rmap_probe.py" "$copy" --quiet
echo "oracle after Linux writes: clean"
rm -f "$copy"

echo "RW-6B near-ENOSPC Linux verification OK"
