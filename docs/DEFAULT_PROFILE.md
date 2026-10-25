# The Linux `mkfs.xfs` default profile and the writable subset

Status: **reference note**, recorded from the XFS source trees in `reference/`
and re-measured by CI (`linux-record-mkfs-defaults`). The authoritative
measurement for a particular distribution is the CI artifact, not this file.

## Why this note exists

`mkfs.xfs` enables a profile that keeps growing: modern xfsprogs turns on
rmapbt, reflink, parent pointers, exchange-range, bigtime and NREXT64 by
default. The Windows formatter and the writable profile intentionally support
a smaller, explicitly qualified set. A "default `mkfs.xfs`" image therefore is
**not** guaranteed to be writable by this project, and saying "Linux default"
without the exact options is not evidence.

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
`/etc/mkfs.xfs.conf` can change them at runtime. The CI job records
`mkfs.xfs -V`, the formatted image's `xfs_info` and `xfs_db` superblock so the
actual distro profile is known.

With parent pointers enabled, `mkfs.xfs` also enables exchange-range
(`xfs_mkfs.c` explicitly couples them) and writes `ATTRBIT`; every new inode
carries parent-pointer attributes. The ordinary writable profile refuses this
geometry, so a genuine default image is **read-only** in this build.

## Supported writable geometry

`CheckWritableProfile` accepts the following v5 geometry:

* 512-byte sectors, 1 KiB–64 KiB filesystem blocks;
* CRC on, ftype on, attr2;
* finobt, inobtcount, sparse inodes, bigtime and NREXT64 (individually or as
  the `--modern-inode-profile` set);
* rmapbt on or off;
* reflink on or off, in either combination with rmapbt;
* one internal log, one data device, no realtime device;
* `dirblklog == blocklog` (one filesystem block per directory block);
* `XFS_SB_FEAT_INCOMPAT_LOG_XATTRS` (logged extended attributes), now that
  ATTRI/ATTRD are parsed;
* `meta_uuid` and exchange-range bits without parent pointers (the formatter
  and Linux legal combinations are checked by the existing formatter jobs).

Refused before the first write:

* parent pointers (`XFS_SB_FEAT_INCOMPAT_PARENT`);
* metadir, zoned and needs-repair;
* quotas, realtime, external logs, unsupported log-incompat bits;
* allocation groups of 2^31 blocks or more (reflink's CoW flag uses bit 31);
* `dirblklog != blocklog` and `asciici`.

Read-only mounts remain permissive for the reader; this list is about the
writable path.

## How RW qualification fixtures are made

Linux fixtures must disable parent pointers explicitly and can use either
reflink profile:

```bash
truncate -s 512M rw6d.img
mkfs.xfs -f -q -b size=4096 -d agcount=2 \
  -m crc=1,finobt=1,inobtcount=1,bigtime=1,reflink=1,rmapbt=1 \
  -i sparse=1,nrext64=1 -n parent=0 rw6d.img
```

`tests/scripts/make_rw6d_linux.sh` detects whether the local `mkfs.xfs`
understands `-n parent=0`; older xfsprogs without parent pointers simply need
no option. `SIZE_MB` overrides the default size.

The Windows formatter's older minimal profile is deliberately not the Linux
default: `xfswin format` leaves the modern features off unless requested. The
RW-6D 1 KiB fixture enables them explicitly (`--make-rw6d-fixture`), so it
exercises a profile close to modern Linux while keeping one block per
directory block.

## Why 4 KiB Linux fixtures and 1 KiB Windows fixtures

`mkfs.xfs` cannot create a 1 KiB directory block on a v5 filesystem with CRC
enabled (`XFS_MIN_CRC_BLOCKSIZE` is 1024 but the directory block is forced to
`XFS_MIN_REC_DIRSIZE`, 4096, when the filesystem block is smaller). That would
set `dirblklog != blocklog`, which the writable policy refuses.

The Linux fixtures therefore use 4 KiB blocks. A 4 KiB attribute leaf holds 15
entries with 240-byte names, and a 4 KiB DA node holds at most 504 child
pointers, so the generator uses 9000 long-name attributes and interleaved
filler writes to force a level-2 DA node and an attribute BMBT. The Windows
formatter can create a 1 KiB filesystem with one block per directory block; its
fixture reaches level 2 with 140 entries of 600 bytes and exercises the same
writer paths. Both fixtures are verified by Linux.

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

The expected default for the reference 7.2 tree is on; the CI artifact is the
record of what the runner's xfsprogs actually enabled. Qualification fixtures
never rely on the default when parent pointers would make the image read-only.

## Consequences for compatibility claims

* A default Linux image with parent pointers is read-only; this is a documented
  gap, not a silent downgrade. `xfswin check` still verifies it.
* An image formatted with `-n parent=0` and the modern features is within the
  supported profile (rmapbt/reflink both on or off) and is what the RW-6D
  round-trip jobs use.
* The Windows formatter's minimal profile is still valid; the writer supports
  it, and older RW-0..RW-6C jobs continue to use it unchanged.
* "Linux-created" evidence must name the exact mkfs options. The CI logs do.
