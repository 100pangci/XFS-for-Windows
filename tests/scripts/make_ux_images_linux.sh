#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# UX-1: build the VHD/VHDX fixtures the Windows discovery, interactive-mount,
# Explorer-volume-info and automount jobs consume.  Runs on Linux as root.
#
# Every fixture is a real filesystem image made by the Linux tools, not a
# hand-assembled approximation:
#
#   single/no-xfs          VHDX, GPT, one XFS / one ext4 partition
#   multi-partition        VHD, GPT, two XFS partitions and one ext4 partition
#   whole-disk             VHD, XFS formatted directly on the disk
#   corrupt                VHD, XFS magic with a broken superblock CRC
#   unsupported            VHD, valid superblock with the METADIR feature
#   dirty                  VHD, a real dirty journal captured after fsync
#   same-a / same-b        VHD, two different UUIDs sharing the label UX1-SAME
#
# The fixture manifest records the XFS UUID, label, geometry, journal state and
# SHA-256 of every file.  The Windows jobs hash the files again before and
# after discovery to prove that discovery did not write.
#
# usage: make_ux_images_linux.sh <outdir>
set -euo pipefail

OUT=${1:?usage: make_ux_images_linux.sh <outdir>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
TOOLS="$REPO/tests/tools"
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

if [ "$(id -u)" != "0" ]; then
    echo "must run as root (loop devices, mount)" >&2
    exit 1
fi
for tool in mkfs.xfs mkfs.ext4 sgdisk qemu-img losetup mount umount \
            xfs_repair xfs_db truncate sha256sum python3; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

mkdir -p "$OUT" "$WORK/raw" "$MNT"

# Newer xfsprogs enable parent pointers by default; that is outside the
# writable profile and irrelevant for a read-only discovery fixture.
PARENT_OPT=""
if mkfs.xfs 2>&1 | grep -q 'parent='; then
    PARENT_OPT="-n parent=0"
fi

xfs_format() {
    local out=$1 size_mb=$2 label=$3 uuid=$4
    rm -f "$out"
    truncate -s "${size_mb}M" "$out"
    LOOP=$(losetup --find --show "$out")
    mkfs.xfs -f -q -L "$label" \
        -m crc=1,finobt=0,rmapbt=0,reflink=0,inobtcount=0,bigtime=0,uuid="$uuid" \
        -i sparse=0 $PARENT_OPT "$LOOP"
    mount "$LOOP" "$MNT"
    printf 'hello from xfswin ux1\n' > "$MNT/hello.txt"
    printf 'XFS UX-1 fixture payload\n' > "$MNT/payload.bin"
    truncate -s 262144 "$MNT/payload.bin"
    sha256sum "$MNT/payload.bin" | awk '{print $1}' > "$MNT/payload.sha256"
    sync
    umount "$MNT"
    losetup -d "$LOOP"
    LOOP=""
}

ext4_format() {
    local out=$1 size_mb=$2 label=$3
    rm -f "$out"
    truncate -s "${size_mb}M" "$out"
    LOOP=$(losetup --find --show "$out")
    mkfs.ext4 -q -L "$label" -F "$LOOP"
    losetup -d "$LOOP"
    LOOP=""
}

gpt_disk() {
    local out=$1 size_mb=$2
    rm -f "$out"
    truncate -s "${size_mb}M" "$out"
    sgdisk --clear "$out" >/dev/null
}

write_at_mib() {
    local payload=$1 disk=$2 offset_mib=$3
    dd if="$payload" of="$disk" bs=1M seek="$offset_mib" conv=notrunc status=none
}

echo "== XFS partition payloads =="
xfs_format "$WORK/raw/single.img" 384 UX1-SINGLE \
    11111111-1111-4111-8111-111111111111
xfs_format "$WORK/raw/part-a.img" 384 UX1-PART-A \
    22222222-2222-4222-8222-222222222222
xfs_format "$WORK/raw/part-b.img" 384 UX1-PART-B \
    33333333-3333-4333-8333-333333333333
xfs_format "$WORK/raw/whole.img" 384 UX1-WHOLE \
    44444444-4444-4444-8444-444444444444
xfs_format "$WORK/raw/same-a.img" 384 UX1-SAME \
    66666666-6666-4666-8666-666666666666
xfs_format "$WORK/raw/same-b.img" 384 UX1-SAME \
    77777777-7777-4777-8777-777777777777

echo "== non-XFS payload =="
ext4_format "$WORK/raw/ext4.img" 320 UX1-NOT-XFS
# The middle partition of the multi-partition disk is 32 MiB, so it gets its
# own complete ext4 filesystem instead of the truncated large payload.
ext4_format "$WORK/raw/ext4-small.img" 32 UX1-MIDDLE

echo "== assembled disks =="
# single: one 384 MiB XFS partition at the conventional 1 MiB offset.
gpt_disk "$WORK/raw/single.disk" 400
sgdisk -n 1:2048:+786432 -t 1:8300 -c 1:UX1-SINGLE \
    "$WORK/raw/single.disk" >/dev/null
write_at_mib "$WORK/raw/single.img" "$WORK/raw/single.disk" 1

# no-xfs: one 320 MiB ext4 partition.  No XFS magic anywhere.
gpt_disk "$WORK/raw/no-xfs.disk" 352
sgdisk -n 1:2048:+655360 -t 1:8300 -c 1:UX1-NOT-XFS \
    "$WORK/raw/no-xfs.disk" >/dev/null
write_at_mib "$WORK/raw/ext4.img" "$WORK/raw/no-xfs.disk" 1

# multi-partition: XFS, ext4, XFS.  The ext4 partition must not be reported.
gpt_disk "$WORK/raw/multi.disk" 832
sgdisk -n 1:2048:+786432 -t 1:8300 -c 1:UX1-PART-A \
       -n 2:788480:+65536 -t 2:8300 -c 2:UX1-MIDDLE \
       -n 3:854016:+786432 -t 3:8300 -c 3:UX1-PART-B \
       "$WORK/raw/multi.disk" >/dev/null
write_at_mib "$WORK/raw/part-a.img" "$WORK/raw/multi.disk" 1
write_at_mib "$WORK/raw/ext4-small.img" "$WORK/raw/multi.disk" 385
write_at_mib "$WORK/raw/part-b.img" "$WORK/raw/multi.disk" 417

echo "== corrupt and unsupported superblocks =="
python3 "$TOOLS/ux_fixture_tools.py" corrupt-superblock \
    "$WORK/raw/single.img" "$WORK/raw/corrupt.img"
python3 "$TOOLS/ux_fixture_tools.py" unsupported-feature \
    "$WORK/raw/single.img" "$WORK/raw/unsupported.img"

echo "== dirty journal (real Linux capture) =="
SCENARIOS=create bash "$REPO/tests/scripts/make_dirty_logs_linux.sh" \
    "$WORK/dirty" >/dev/null
cp --sparse=always "$WORK/dirty/create.img" "$WORK/raw/dirty.img"

echo "== wrap as VHD/VHDX =="
python3 "$TOOLS/ux_fixture_tools.py" vhd-footer \
    "$WORK/raw/single.disk" "$OUT/ux-single-raw.vhd"
qemu-img convert -f raw -O vhdx -o subformat=fixed,block_size=1M \
    "$WORK/raw/single.disk" "$OUT/ux-single.vhdx" 2>/dev/null || {
    rm -f "$OUT/ux-single.vhdx"
    qemu-img convert -f raw -O vhdx \
        "$WORK/raw/single.disk" "$OUT/ux-single.vhdx"
}
qemu-img convert -f raw -O vhdx -o subformat=fixed,block_size=1M \
    "$WORK/raw/no-xfs.disk" "$OUT/ux-no-xfs.vhdx" 2>/dev/null || {
    rm -f "$OUT/ux-no-xfs.vhdx"
    qemu-img convert -f raw -O vhdx \
        "$WORK/raw/no-xfs.disk" "$OUT/ux-no-xfs.vhdx"
}
rm -f "$OUT/ux-single-raw.vhd"
for pair in \
    "multi.disk:ux-multi-partition.vhd" \
    "whole.img:ux-whole-disk.vhd" \
    "corrupt.img:ux-corrupt.vhd" \
    "unsupported.img:ux-unsupported.vhd" \
    "dirty.img:ux-dirty.vhd" \
    "same-a.img:ux-same-a.vhd" \
    "same-b.img:ux-same-b.vhd"; do
    raw=${pair%%:*}
    name=${pair##*:}
    python3 "$TOOLS/ux_fixture_tools.py" vhd-footer \
        "$WORK/raw/$raw" "$OUT/$name"
done

echo "== independent format checks =="
for file in "$OUT"/*.vhd "$OUT"/*.vhdx; do
    qemu-img info --output=json "$file" | python3 -c \
        'import json,sys; d=json.load(sys.stdin); assert d["format"] in ("vpc","vhdx"), d'
    qemu-img check "$file" >/dev/null
done

echo "== Linux XFS oracle on the pristine payloads =="
for img in single.img part-a.img part-b.img whole.img same-a.img same-b.img; do
    xfs_repair -n "$WORK/raw/$img" >/dev/null
done
python3 "$TOOLS/recovery_probe.py" "$WORK/raw/dirty.img" --json >/dev/null
STATE=$(python3 "$TOOLS/recovery_probe.py" "$WORK/raw/dirty.img" --json |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])')
if [ "$STATE" != "dirty" ]; then
    echo "ERROR: dirty fixture classified as '$STATE'" >&2
    exit 1
fi

echo "== manifest =="
python3 - "$OUT" "$WORK/raw" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from ux_fixture_tools import summarize  # noqa: E402

out = Path(sys.argv[1])
raw = Path(sys.argv[2])


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def partition(payload, label, offset, length, whole_disk=False, journal="clean",
              number=0):
    facts = summarize(payload)
    return {
        "label": label,
        "uuid": facts["uuid"],
        "offset": offset,
        "length": length,
        "whole_disk": whole_disk,
        "partition_number": 0 if whole_disk else number,
        "filesystem_bytes": facts["filesystem_bytes"],
        "free_bytes": facts["free_bytes"],
        "block_size": facts["block_size"],
        "journal": journal,
    }


mib = 1024 * 1024
fixtures = [
    {
        "name": "single",
        "file": "ux-single.vhdx",
        "format": "vhdx",
        "expect": "valid",
        "partitions": [partition(raw / "single.img", "UX1-SINGLE", 1 * mib,
                                 384 * mib, number=1)],
    },
    {
        "name": "no-xfs",
        "file": "ux-no-xfs.vhdx",
        "format": "vhdx",
        "expect": "none",
        "partitions": [],
    },
    {
        "name": "multi-partition",
        "file": "ux-multi-partition.vhd",
        "format": "vhd",
        "expect": "valid",
        "partitions": [
            partition(raw / "part-a.img", "UX1-PART-A", 1 * mib, 384 * mib,
                      number=1),
            partition(raw / "part-b.img", "UX1-PART-B", 417 * mib, 384 * mib,
                      number=3),
        ],
    },
    {
        "name": "whole-disk",
        "file": "ux-whole-disk.vhd",
        "format": "vhd",
        "expect": "valid",
        "partitions": [partition(raw / "whole.img", "UX1-WHOLE", 0,
                                 384 * mib, whole_disk=True)],
    },
    {
        "name": "corrupt",
        "file": "ux-corrupt.vhd",
        "format": "vhd",
        "expect": "corrupt",
        "partitions": [],
    },
    {
        "name": "unsupported",
        "file": "ux-unsupported.vhd",
        "format": "vhd",
        "expect": "unsupported",
        "partitions": [],
    },
    {
        "name": "dirty",
        "file": "ux-dirty.vhd",
        "format": "vhd",
        "expect": "valid",
        "partitions": [partition(raw / "dirty.img", "", 0,
                                 (raw / "dirty.img").stat().st_size,
                                 whole_disk=True, journal="dirty")],
    },
    {
        "name": "same-a",
        "file": "ux-same-a.vhd",
        "format": "vhd",
        "expect": "valid",
        "partitions": [partition(raw / "same-a.img", "UX1-SAME", 0,
                                 384 * mib, whole_disk=True)],
    },
    {
        "name": "same-b",
        "file": "ux-same-b.vhd",
        "format": "vhd",
        "expect": "valid",
        "partitions": [partition(raw / "same-b.img", "UX1-SAME", 0,
                                 384 * mib, whole_disk=True)],
    },
]

for fixture in fixtures:
    path = out / fixture["file"]
    fixture["disk_size"] = path.stat().st_size
    fixture["sha256"] = sha256(path)

manifest = {"version": 1, "fixtures": fixtures}
(out / "ux-fixtures.json").write_text(
    json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(f"wrote {out / 'ux-fixtures.json'}")
PY

echo "fixtures written to $OUT"