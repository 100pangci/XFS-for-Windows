#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - Linux verification of the RW-4 round trip.
#
#   sudo tests/scripts/verify_rw4_linux.sh <dir>
#
# <dir> holds the images produced by tests/scripts/verify_rw4_windows.ps1
# (rename, replace, symlink, nodedir) and manifest.json.
#
# Every image must pass:
#   1. the independent structural oracle (rw4_probe.py: namespace walk, link
#      counts, symlink headers/CRCs, inobt/AGI and directory formats),
#   2. xfs_repair -n (no corruption),
#   3. a kernel mount (replays whatever the writer left) and the mounted state
#      must match the manifest exactly (names, types, sizes, contents, symlink
#      targets, hard-link identity),
#   4. after the kernel's clean unmount, a clean log according to our parser.
#
# Then the kernel modifies rename.img the other way: Linux renames files and
# directories, creates a hard link and a symlink inside Windows-created
# directories, and the result is checked again and saved as reverse.img for
# the Windows reverse job.
set -euo pipefail

dir="${1:?usage: verify_rw4_linux.sh <dir>}"
dir="$(readlink -f "$dir")"
for tool in mount umount xfs_repair python3 readlink stat cmp; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing required tool: $tool (install xfsprogs/util-linux)" >&2
        exit 1
    }
done
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools_dir="$(cd "$script_dir/../tools" && pwd)"
manifest="$dir/manifest.json"
images="$(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1]))["images"].keys()))' "$manifest")"

failures=0
work="$(mktemp -d)"
mnt="$(mktemp -d)"
cleanup() {
    if mountpoint -q "$mnt"; then umount "$mnt" || true; fi
    rm -rf "$work" "$mnt"
}
trap cleanup EXIT

check_image() {
    local img="$1"
    echo "== $(basename "$img") =="
    if ! python3 "$tools_dir/rw4_probe.py" "$img" --quiet; then
        echo "FAILED: oracle rejected $(basename "$img")" >&2
        failures=1
        return
    fi
    local repair_log
    repair_log="$(mktemp)"
    if ! xfs_repair -n "$img" > "$repair_log" 2>&1; then
        cat "$repair_log"
        echo "FAILED: xfs_repair -n returned an error for $(basename "$img")" >&2
        failures=1
    elif grep -qiE 'corruption|fatal|would fix|ALERT|valuable metadata' \
            "$repair_log"; then
        cat "$repair_log"
        echo "FAILED: xfs_repair -n found problems in $(basename "$img")" >&2
        failures=1
    else
        tail -1 "$repair_log"
    fi
    rm -f "$repair_log"
    if ! mount -o loop,nouuid "$img" "$mnt"; then
        echo "FAILED: the kernel could not mount $(basename "$img")" >&2
        dmesg | tail -40 || true
        failures=1
        return
    fi
    if ! python3 "$tools_dir/rw4_content.py" check-mount "$mnt" "$manifest" \
            "$(basename "$img")"; then
        echo "FAILED: mounted state differs for $(basename "$img")" >&2
        failures=1
    fi
    umount "$mnt"
    if ! python3 "$tools_dir/log_probe.py" "$img" --expect clean \
            >/dev/null; then
        echo "FAILED: no clean log after the kernel unmounted $(basename "$img")" \
            >&2
        failures=1
    fi
}

for image in $images; do
    cp "$dir/$image" "$work/$image"
    check_image "$work/$image"
done

# ---------------------------------------------------------------------------
# Reverse interoperability: the kernel renames, links and symlinks inside the
# Windows-created namespace of rename.img, then reads it all back.
# ---------------------------------------------------------------------------
echo "== reverse interoperability =="
cp "$dir/rename.img" "$work/reverse.img"
if ! mount -o loop,nouuid "$work/reverse.img" "$mnt"; then
    echo "FAILED: cannot mount rename.img for the reverse test" >&2
    failures=1
else
    ok=1
    # 1. Everything Windows created must be visible to Linux.
    [ "$(readlink "$mnt/link2")" = "../renamed.bin" ] || { ok=0; echo "FAILED: readlink link2"; }
    ino_renamed="$(stat -c %i "$mnt/renamed.bin")"
    ino_hard="$(stat -c %i "$mnt/hard.bin")"
    [ "$ino_renamed" = "$ino_hard" ] || { ok=0; echo "FAILED: hard link inodes differ ($ino_renamed != $ino_hard)"; }
    [ "$(stat -c %h "$mnt/renamed.bin")" = "2" ] || { ok=0; echo "FAILED: nlink != 2"; }
    # 2. Modifying one hard link is visible through the other.
    printf 'LINUX-AFTER-WINDOWS' > "$mnt/hard.bin"
    cmp -s "$mnt/renamed.bin" "$mnt/hard.bin" || { ok=0; echo "FAILED: hard link content differs"; }
    # 3. Linux-side namespace operations on the Windows-created tree.
    mv "$mnt/B/file.bin" "$mnt/B/linux-renamed.bin" || { ok=0; echo "FAILED: linux rename in /B"; }
    mkdir "$mnt/linuxdir"
    mv "$mnt/B/A" "$mnt/linuxdir/A2" || { ok=0; echo "FAILED: linux cross-directory directory move"; }
    mv "$mnt/B" "$mnt/B2" || { ok=0; echo "FAILED: linux directory rename"; }
    ln "$mnt/hard.bin" "$mnt/linux-hardlink.bin" || { ok=0; echo "FAILED: linux hard link"; }
    ln -s 'target/linux' "$mnt/linux-symlink" || { ok=0; echo "FAILED: linux symlink"; }
    ln -s "$(python3 -c 'print("L" * 500)')" "$mnt/linux-long-symlink" || { ok=0; echo "FAILED: linux remote symlink"; }
    sync
    umount "$mnt"
    [ "$ok" = "1" ] || failures=1

    if ! python3 "$tools_dir/rw4_probe.py" "$work/reverse.img" --quiet; then
        echo "FAILED: oracle rejected the reverse image" >&2
        failures=1
    fi
    if ! xfs_repair -n "$work/reverse.img" >/dev/null 2>&1; then
        echo "FAILED: xfs_repair after the Linux modifications" >&2
        failures=1
    fi
    # Read the new state back through another mount.
    if mount -o loop,nouuid "$work/reverse.img" "$mnt"; then
        ok=1
        [ "$(readlink "$mnt/linux-symlink")" = "target/linux" ] || { ok=0; echo "FAILED: linux symlink target"; }
        [ "$(readlink "$mnt/linux-long-symlink")" = "$(python3 -c 'print("L" * 500)')" ] || { ok=0; echo "FAILED: linux remote symlink target"; }
        [ -f "$mnt/B2/linux-renamed.bin" ] || { ok=0; echo "FAILED: renamed file"; }
        [ -d "$mnt/linuxdir/A2" ] || { ok=0; echo "FAILED: moved directory"; }
        [ -f "$mnt/linuxdir/A2/child2/inner.bin" ] || { ok=0; echo "FAILED: moved directory contents"; }
        cmp -s "$mnt/hard.bin" "$mnt/linux-hardlink.bin" || { ok=0; echo "FAILED: linux hard link content"; }
        [ "$(stat -c %h "$mnt/hard.bin")" = "3" ] || { ok=0; echo "FAILED: nlink after linux link != 3"; }
        [ "$(stat -c %i "$mnt/hard.bin")" = "$(stat -c %i "$mnt/linux-hardlink.bin")" ] || { ok=0; echo "FAILED: linux hard link inode"; }
        [ ! -e "$mnt/B" ] || { ok=0; echo "FAILED: old /B still exists"; }
        umount "$mnt"
        [ "$ok" = "1" ] || failures=1
    else
        echo "FAILED: cannot remount the reverse image" >&2
        failures=1
    fi
    cp "$work/reverse.img" "$dir/reverse.img"
fi

if [ "$failures" != "0" ]; then
    echo "RW-4 LINUX VERIFICATION FAILED" >&2
    exit 1
fi
echo "OK: every RW-4 image mounted, replayed and verified by Linux"
