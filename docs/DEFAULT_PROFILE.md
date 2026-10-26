# The Linux `mkfs.xfs` default profile and the writable subset

Status: **RW-6E qualification note.**  The modern parent-pointer profile is
inside the experimental writable allow-list when it stays within the documented
single-device/internal-log geometry.  The exact xfsprogs and kernel versions
are recorded by CI; a plain `mkfs.xfs -f` image without `parent=0` is the
acceptance fixture, not a hand-edited feature-bit hack.

## Why this note exists

`mkfs.xfs` enables a profile that keeps growing.  The reference 7.2 tree turns
on crc, finobt, sparse inodes, inobtcount, rmapbt, reflink, bigtime, NREXT64,
parent pointers and exchange-range by default.  The Windows formatter supports
this set (minus metadir/realtime/quota) and the writable policy validates the
actual superblock, so a future default that adds an unsupported feature is
refused instead of silently corrupted.

## Build-time defaults (xfsprogs 7.2.0)

From `reference/xfsprogs-7.2.0/mkfs/xfs_mkfs.c`, the `dft` structure:

| Feature | Default |
|---|---|
| CRC / v5 | on |
| directory v2 + ftype | on |
| attr2 | on |
| lazy superblock counters | on |
| finobt | on |
| sparse inodes | on |
| inobtcount | on |
| rmapbt | on |
| reflink (refcountbt) | on |
| bigtime | on |
| NREXT64 | on |
| parent pointers | on |
| exchange-range | on |
| metadir | off (7.2 default) |
| realtime | off |
| external log | off |
| quotas | off |
| zoned | off |

Distribution packages may patch these defaults; `mkfs.xfs -c` and
`/etc/mkfs.xfs.conf` can change them at runtime.  CI records `mkfs.xfs -V`, the
formatted image's `xfs_info` and the `xfs_db` superblock so the actual profile
is known.  Test logic measures and classifies the resulting features; it does
not hardcode a version string.

With parent pointers enabled, `mkfs.xfs` also enables exchange-range
(`xfs_mkfs.c` explicitly couples them).  The exchange-range **bit** does not
require the exchange operation for ordinary namespace, attribute, reflink or
write paths; the XMI/XMD operation remains outside RW-6E and its log items are
refused before the first write.

## Supported writable geometry

`CheckWritableProfile` accepts the following v5 geometry:

* 512-byte sectors, 1 KiB–64 KiB filesystem blocks;
* CRC on, ftype on, attr2, and `XFS_SB_VERSION_ATTRBIT` (the formatter sets
  it because it supports user xattrs and parent pointers; `xfs_scrub`
  reports every attr fork as corrupt when the bit is clear);
* finobt, inobtcount, sparse inodes, bigtime and NREXT64 (individually or as
  the `--modern-inode-profile` set);
* rmapbt on or off;
* reflink on or off, in either combination with rmapbt;
* parent pointers (`XFS_SB_FEAT_INCOMPAT_PARENT`) with the corresponding
  forward namespace and reverse attr-fork maintenance;
* exchange-range as a feature bit (operation not implemented);
* `XFS_SB_FEAT_INCOMPAT_LOG_XATTRS` (logged extended attributes), independent
  of parent pointers;
* one internal log, one data device, no realtime device;
* `dirblklog == blocklog` (one filesystem block per directory block).

Refused before the first write:

* metadir, zoned and needs-repair;
* quotas, realtime devices, external logs, unsupported log-incompat bits;
* allocation groups of 2^31 blocks or more (reflink's CoW flag uses bit 31);
* `dirblklog != blocklog` and `asciici`;
* exchange-range *operations*: a log containing XMI/XMD is refused rather than
  replayed halfway.

Read-only mounts remain permissive for the reader; this list is about the
writable path.

## The Modern Default Profile Gate

The primary RW-6E fixture is:

```bash
truncate -s 512M default.img
mkfs.xfs -f default.img          # no -n parent=0
xfs_info default.img
xfs_db -r -c 'sb 0' -c 'p' default.img
```

The image must:

1. contain the PARENT incompat bit (and exchange-range, as mkfs couples them);
2. stay within the supported geometry above;
3. mount read-write through this build with ordinary `--rw-experimental`;
4. pass `xfswin check`, the independent Python parent oracle, a Linux kernel
   mount, `xfs_repair -n` and `xfs_scrub` (when available).

If a future xfsprogs default adds metadir, zoned, realtime or another
unsupported feature, the measured feature classification refuses that image
and the README's claim remains tied to the tested version.  There is no claim
that *all future* defaults are supported.

## How qualification fixtures are made

Linux fixtures no longer disable parent pointers.  `make_rw6e_linux.sh` runs a
plain `mkfs.xfs -f`, records the tool version and measured features, mounts the
image, and creates the parent workload (directories, 1000 hard links to one
inode, same-name links under different parents, cross-directory renames,
symlinks, Unicode/long names, xattrs, ACL, reflink + CoW).  See:

```bash
sudo bash tests/scripts/make_rw6e_linux.sh rw6e-linux
sudo bash tests/scripts/verify_rw6e_linux.sh rw6e-linux/rw6e.img \
    rw6e-linux/rw6e.manifest.json
```

`qualify_rw6e_host.sh` refuses to run if `mkfs.xfs` cannot create `parent=1` or
the host kernel cannot mount it.  A repair-only result is never accepted as
substitute evidence.

The Windows formatter exposes the same profile:

```text
xfswin format image.img --modern-default-profile
xfswin format image.img --parent        # parent + exchange-range
```

It refuses profiles that are not legal in the reference xfsprogs (for example,
inobtcount without finobt).

## Measured defaults (CI)

The `linux-record-mkfs-defaults` job uploads:

```text
version.txt                mkfs.xfs -V
default.mkfs.txt           mkfs output for a plain mkfs.xfs -f
default.xfs_info.txt       xfs_info of the default image
default.sb.txt             xfs_db -r -c 'sb 0' -c 'p'
default.version.txt        xfs_db -r -c 'version'
parentless.*               the same for mkfs.xfs -n parent=0
```

The first measured runner (Ubuntu 24.04, xfsprogs **6.6.0**) produced:

```text
mkfs.xfs version 6.6.0
meta-data  isize=512 agcount=4 agsize=32768 blks
           attr=2, projid32bit=1, crc=1, finobt=1, sparse=1,
           rmapbt=1, reflink=1, bigtime=1, inobtcount=1, nrext64=0
data       bsize=4096 blocks=131072
naming     version=2 bsize=4096 ascii-ci=0 ftype=1
log        internal bsize=4096 blocks=16384 version=2 lazy-count=1
realtime   none
sb_dirblklog = 0
sb_features_ro_compat = 0xf
sb_features_incompat = 0xb
sb_features_log_incompat = 0
```

`mkfs.xfs` 6.6 has no `parent=` option, so that older observation could not
exercise parent pointers. The RW-6E jobs now run on GitHub-hosted
`ubuntu-24.04` (kernel 6.17.0-1022-azure), install xfsprogs 7.2.0 from
kernel.org with `tests/scripts/install_xfsprogs_7_2_0.sh`, and run plain
`mkfs.xfs -f` as the acceptance fixture. The 7.2 source defaults above are the
target profile; the job artifacts record the actual runner, tool version,
`xfs_info` and superblock bits. `qualify_rw6e_host.sh` remains the hard gate:
a runner that cannot format or mount `parent=1` fails instead of producing
repair-only evidence.

[Run 40221845841](https://github.com/100pangci/XFS-for-Windows/actions/runs/40221845841)
verified this profile on Linux **6.17.0-1022-azure** with xfsprogs **7.2.0**:
plain `mkfs.xfs -f` enabled `parent=1`, `exchrange=1` and incompat bits
`0xeb`. The image passed the Linux → Windows → Linux namespace/content
round trip, the independent parent oracle (**1023/1023** edges/pointers), and
`xfs_repair -n`. Online scrub was explicitly skipped because the Azure kernel
did not provide its metadata scrub facility. See
[PARENT_POINTERS.md](PARENT_POINTERS.md#github-actions-linux-evidence-2026-10-26)
for the measured coverage and remaining qualification gates.

## Consequences for compatibility claims

* A plain modern `mkfs.xfs -f` image is the primary RW-6E fixture.  It is
  accepted by ordinary `--rw-experimental` after the feature measurement and
  the parent audit pass.
* An image formatted with `-n parent=0` remains a valid subset.
* The exact qualified xfsprogs/kernel versions, and the full feature list, must
  accompany any release claim.  This note intentionally does not promise
  support for future defaults that add metadir, realtime, quotas or zoned
  geometry.
