# Native extended attributes (RW-6D)

Status: **implementation complete, Linux qualification pending.** The writable
profile accepts attribute creation, replacement, removal and preservation on
ordinary `--rw-experimental` mounts. The gate is not opened merely by this
document: the qualification ledger at the end must be filled from a CI run,
exactly like RW-6B and RW-6C.

This milestone implements:

* a typed, fully verified reader for shortform, single-leaf, node and
  multi-level node attribute forks, including attribute BMBT extent maps;
* remote attribute values (`XARM` blocks);
* a writer that grows/shrinks shortform → leaf → node trees incrementally,
  including node split, merge, borrow and root collapse, and rebuilds a
  representation atomically when the target format changes;
* `forkoff` changes, data-fork extents → btree conversion when the attribute
  fork takes literal space, and the reverse layout;
* attribute-fork block ownership in free space and (when enabled) rmap;
* inode deletion with attribute extents, BMBT blocks and remote values;
* `ATTRI`/`ATTRD` log intent parsing and recovery completion;
* `user.*`, `trusted.*`, `security.*` and the raw `system.posix_acl_*` storage
  forms, exposed through `xfswin xattr`;
* `xfswin check` verification of every attribute block, `di_nblocks`, and
  attribute rmap ownership;
* an independent Python oracle and Linux/Windows round-trip, crash and ENOSPC
  scripts.

Out of scope, deliberately:

* translating Windows security descriptors or POSIX ACL semantics; the ACL
  attribute payload is stored and returned as opaque bytes;
* NTFS alternate data streams;
* parent-pointer mutation. Parent-pointer filesystems remain read-only for
  ordinary RW; parent entries are read and preserved, never created or edited;
* quotas, realtime, external logs, metadir, zoned filesystems and online
  repair.

## Source map

| Area | Files |
|---|---|
| Typed reader / verifier | `include/xfs/xattr.h`, `src/core/xattr.cpp` |
| Writer, tree editor, fork transitions | `src/write/xattr_write.cpp` |
| Data-fork forkoff conversion | `include/xfs/bmap.h`, `src/write/bmap.cpp` |
| Inode log item (data + attr regions) | `src/write/inode_write.cpp` |
| Inode deletion / attribute free | `src/write/namespace.cpp` |
| `check` integration / rmap ownership | `src/core/filesystem.cpp` |
| ATTRI/ATTRD format | `include/xfs/xattr_intent.h`, `src/core/xattr_intent.cpp` |
| Intent scan and lifetime | `src/recovery/log_scan.cpp` |
| Intent completion | `src/write/recovery_host.cpp` |
| Log metadata classification | `src/recovery/replay.cpp` |
| CLI | `src/cli/commands.cpp`, `src/cli/cli.h` |
| Unit tests | `tests/unit/rw6d_tests.cpp` |
| Independent oracle | `tests/tools/rw6d_xattr_probe.py` |
| Fixture / mutation / crash / ENOSPC tools | `tests/test_main.cpp`, `tests/tools/rw6d_*.py` |
| Linux generators / verifiers | `tests/scripts/*rw6d*` |

The authority is `reference/linux/fs/xfs/libxfs/xfs_attr_leaf.c`,
`xfs_attr_remote.c`, `xfs_da_btree.c`, `xfs_da_format.h`, `xfs_bmap.c` and
`xfs_attr_item.c`, plus `reference/xfsprogs-7.2.0/`. No GPL code is copied.

## On-disk model

### Inode fork fields

| Field | Meaning |
|---|---|
| `di_forkoff` | offset of the attribute fork from the end of the v3 core, in 8-byte units; 0 means no attribute fork |
| `di_aformat` | 1 = LOCAL (shortform), 2 = EXTENTS, 3 = BTREE |
| `di_anextents` | attribute-fork extent count; with NREXT64 this is `di_big_anextents` and `di_nrext64_pad` must be 0 |
| `di_nblocks` | data extents + data BMBT blocks + attribute extents/BMBT blocks |

The literal area is `inode_size - 176`, so a 512-byte inode has 336 bytes. A
shortform attribute fork shares that area with a shortform directory/symlink
data fork; the writer chooses a fork offset that preserves both, or refuses
before writing.

### Shortform

```text
xfs_attr_sf_hdr:  totsize (be16), count (u8), i8count (u8)
entry:            namelen (u8), valuelen (u8), flags (u8), name, value
```

There is **no padding** between shortform entries: the entry size is exactly
`3 + namelen + valuelen`. `totsize` includes the 4-byte header and every entry.
Shortform entries carry only namespace bits (and never `XFS_ATTR_LOCAL` or
`XFS_ATTR_INCOMPLETE`); names are at most 255 bytes and values at most 254
bytes.

### Leaf blocks

A v5 attribute leaf starts with the 80-byte `xfs_attr3_leaf_hdr` (magic
`0x3bee` at offset 8, CRC at 12, blkno at 16, UUID at 32, owner inode at 48,
LSN at 56, count/usedbytes/firstused, three bestfree maps) followed by one
8-byte `xfs_attr_leaf_entry` per attribute:

| Offset | Field |
|---|---|
| 0 | `hashval` (be32) |
| 4 | `nameidx` (be16) |
| 6 | `flags` (u8) |
| 7 | padding |

`nameidx` points into the name/value area:

* local: `valuelen` (be16), `namelen` (u8), name, value, padded to 4 bytes;
* remote: `valueblk` (be32, logical attr block), `valuelen` (be32, full value
  length), `namelen` (u8), pad, name, padded to 4 bytes.

Entries are sorted by `xfs_da_hashname`; the leaf `usedbytes` is the sum of the
padded entry sizes. `firstused` is the first byte of the name/value area.

### DA node tree

A node block has the 56-byte v5 DA blkinfo (magic `0x3ebe` at 8, CRC at 12,
blkno at 16, LSN at 24, UUID at 32, owner at 48) plus count and level at
offsets 56/58, then 8-byte entries:

| Offset | Field |
|---|---|
| 0 | `hashval` (be32), the last hash in the child subtree |
| 4 | child logical attr block (be32) |

The root is always logical block 0. Leaves and nodes carry `forw`/`back`
sibling links at offsets 4/0. A root with leaf children has level 1; one with
node children has level 2 and so on (maximum 5). The writer builds levels
until the remaining children fit one root, and the incremental editor
splits/merges/borrows/collapses the same way.

### Remote values

A remote value is stored in one or more `xfs_attr3_rmt_hdr` blocks:

| Offset | Field |
|---|---|
| 0 | magic `XARM` (`0x5841524d`, be32) |
| 4 | offset of this block's bytes in the value (be32) |
| 8 | number of bytes in this block (be32) |
| 12 | CRC32c of the whole block |
| 16 | metadata UUID |
| 32 | owner inode |
| 40 | block number (`daddr`, 512-byte units) |
| 48 | LSN; `NULLCOMMITLSN` (`~0`) for unjournaled value data |
| 56 | value bytes |

The payload capacity is `block_size - 56`. Values are at most 65536 bytes.
Remote blocks are written and flushed **before** the metadata transaction
commits; they are not journaled, so a crash can leave unreferenced value bytes
in free space but never expose a half-written value through an inode.

### Attribute BMBT

When the attribute extent list does not fit the literal area, `di_aformat`
becomes 3 and the fork starts with the `bmdr` root. The BMBT blocks use
`BMAP`/`BMA3` (`0x424d4133`) with owner inode and the attr-fork flag in the
rmap owner. Leaf records are the ordinary 16-byte `xfs_bmbt_rec`. The inode's
`di_anextents` counts leaf records, not BMBT blocks.

### Namespaces and hashes

| Bits | Namespace | VFS prefix |
|---|---|---|
| 0 | user | `user.` |
| 1 (`XFS_ATTR_ROOT`) | root/trusted | `trusted.` |
| 2 (`XFS_ATTR_SECURE`) | security | `security.` |
| 3 (`XFS_ATTR_PARENT`) | parent pointer (private) | `parent.` |

`XFS_ATTR_LOCAL` (bit 0) is a leaf value-placement flag, not a namespace;
`XFS_ATTR_INCOMPLETE` (bit 7) is the transient kernel state. The raw on-disk
names `SGI_ACL_FILE` and `SGI_ACL_DEFAULT` in the root namespace are displayed
as `system.posix_acl_access` and `system.posix_acl_default`.

The hash is `xfs_da_hashname`: rotate left by 7 bits before each character
group, XOR the bytes, as implemented by `AttrHashName` and the oracle. The
shortform list keeps insertion order; leaf/node entries are hash order.

## Writable algorithms

### Representation choice

`XattrWriter::DecideTarget` mirrors the kernel:

* an empty set removes the fork, unless the data fork is a btree (then an
  empty shortform fork is retained, matching `xfs_bmap_forkoff_reset`);
* shortform when `xfs_attr_shortform_bytesfit` fits and every value is local;
* one leaf when all entries fit one leaf block;
* a new full tree for a representation change (shortform/leaf → node or
  node → leaf/shortform), built from the complete desired entry set;
* an incremental tree edit when the fork is already a node and the new entry
  set still needs a node. The editor splits the target leaf, inserts/removes,
  splits/merges/borrows at each level and collapses the root when possible.

A target representation is assembled in memory and published only through
journaled metadata buffers; a failed build writes nothing.

### Remote values and free space

`AllocateChangedRemote` allocates the value's logical attr-fork blocks before
the tree/leaf is published, writes every `XARM` block, and flushes the device.
The replaced entry's old remote range is released in the same transaction as
the inode and tree buffers. If a new value would not fit, the operation fails
before any allocation is committed and the old value stays intact.

### forkoff and the data fork

If a new attribute fork needs more literal space, the writer updates
`di_forkoff` and re-lays the data fork in the smaller area. When a data-fork
extent list no longer fits, it is converted to btree format in the same
transaction (and the bmdr root is logged with `XLOG_ABROOT`); if the data fork
is already a btree and its in-fork root no longer fits, the root is moved into
a new bmbt block and becomes a level+1 node over it (the kernel's
`xfs_btree_new_iroot` path). Shrinking can convert extents back, but an
existing btree stays a btree while an attribute fork is removed. A local data
fork (shortform directory/symlink) is preserved or the operation is refused
with `kNoSpace` rather than truncating it.

The inode log item carries both the data-fork and attr-fork regions and their
field masks (`XFS_ILOG_*`), so replay updates them atomically.

### Inode deletion

`Unlink`/`RemoveEmptyDirectory` free attribute extents, BMBT blocks and remote
value blocks in rolling transactions before the inode is freed. Every block
release removes the rmap owner and updates free space; the attribute fork is
never left dangling because an inode was freed.

### Rmap ownership

Attribute allocations use rmap owner `OWN_ATTR`; attribute BMBT blocks use the
inode owner with the attr-fork flag (`OWN_BMBT` + attr). Remote value blocks
also use `OWN_ATTR`. `xfswin check` cross-checks every attribute extent,
BMBT block and remote block against the rmap tree when `rmapbt=1`, and
recomputes `di_nblocks` from both forks.

## Recovery

`ATTRI` is the host-order attribute intent; `ATTRD` is its done item. The
parser records the inode, the full name, the namespace, the operation
(`set`, `replace`, `remove`), optional value bytes, and validates every offset
against the item length before any replay write. The scan accepts unresolved
intents and the completion pass resolves them in log order:

* set/replace inserts or replaces the verified entry, allocating attr blocks
  and remote values as needed;
* remove deletes the entry and releases its blocks;
* the matching `ATTRD` is logged after the set/remove commits; a crash before
  the done item leaves the intent pending and a restart re-runs the idempotent
  operation (upsert/remove), so the result converges.

Duplicate `namespace + name` is accepted only while at least one entry is
`INCOMPLETE`, which is exactly the kernel's logged-replace transient. Windows
writes attributes in one atomic transaction (inode + free-space/tree buffers)
and therefore does not emit ATTRI/ATTRD itself; it can replay Linux ones.

`XFS_SB_FEAT_INCOMPAT_LOG_XATTRS` is accepted by the writable policy because
the log now has a dedicated parser. Parent-pointer filesystems remain refused
for ordinary RW even though their attribute entries are readable.

## Policy and CLI

Attribute writes require `--rw-experimental`, like every other write path.
Reads (`list`, `get`) work on any readable v5 filesystem.

```text
xfswin xattr list <image> <path> [--json]
xfswin xattr get  <image> <path> <name> [--hex | --output=<file>]
xfswin xattr set  <image> <path> <name> <value> --rw-experimental
                  [--value-file=<file> | --value-hex=<hex>]
                  [--create | --replace]
xfswin xattr remove <image> <path> <name> --rw-experimental
```

Names are VFS names. `--create` is `XATTR_CREATE`, `--replace` is
`XATTR_REPLACE`; the default is upsert. Values are opaque bytes, up to 65536
bytes. `system.posix_acl_*` is stored in the raw `SGI_ACL_*` form; no ACL
translation is performed.

## Testing

Local:

```text
build\tests\RelWithDebInfo\xfs_tests.exe --filter=rw6d
build\tests\RelWithDebInfo\xfs_tests.exe
```

`tests/unit/rw6d_tests.cpp` covers:

* shortform/leaf/remote sets across reflink/rmapbt profile combinations and
  all namespaces;
* create/replace/remove semantics, the 64 KiB limit and missing keys;
* growth to a level-2 DA node with 140 entries, remote replacement and shrink
  back to shortform/none;
* data-fork extents → btree conversion and the retained empty attr fork;
* data-fork btree root promotion when an attribute fork shrinks the literal
  area (`xfs_btree_new_iroot` semantics), with 700 data extents intact;
* attribute BMBT creation and inode deletion with data, tree and remote blocks,
  including a deletion that rolls the attribute free across transactions;
* hash collisions, ordering and duplicate replacement;
* ATTRI/ATTRD parsing: set/remove/replace namespaces, region rounding, parent
  operations refused, bad type/count/inode refused;
* reverse transitions (node → leaf → shortform → none) and a second growth;
* ENOSPC: failed remote sets/replacements leave every attribute unchanged, a
  shortform set still works, and the same set succeeds after freeing space;
* attributes following mkdir/rename/rmdir, hard links, symlink unlink and
  rename replacement;
* data writes, truncate/grow and reflink on an inode with a remote attribute.

Every test ends with a clean-unmount record and `CheckConsistency`, so the
attribute rmap and `di_nblocks` checks run on every scenario.

The independent oracle:

```text
python tests/tools/rw6d_xattr_probe.py <image> --inode N [--inode N ...] \
    [--manifest <json>] [--json out]
```

It reimplements the fork/leaf/node/remote/BMBT layout with its own CRC32c and
hash, checks every reachable block and remote block, and compares values by
length and SHA-256. `tests/tools/rw6d_format_assert.py` requires the full set
of representations in an oracle dump.

Windows fixture and stress tools:

```text
xfs_tests.exe --make-rw6d-fixture out/rw6d-windows.img
python tests/tools/rw6d_windows_mutate.py --exe xfswin.exe \
    --image in.img --manifest in.manifest.json --work-dir out --json out.json
python tests/tools/rw6d_crash_windows.py --exe xfswin.exe \
    --image base.img --manifest base.manifest.json --work-dir crash --zip
python tests/tools/rw6d_enospc_windows.py --exe xfswin.exe \
    --image fill.img --manifest base.manifest.json --work-dir out
python tests/tools/rw6d_windows_reverse.py --exe xfswin.exe \
    --image reverse.img --manifest linux-reverse.manifest.json
```

Linux scripts:

```bash
sudo bash tests/scripts/make_rw6d_linux.sh rw6d-linux
sudo bash tests/scripts/verify_rw6d_linux.sh rw6d-linux/rw6d.img \
    --manifest rw6d-linux/rw6d.manifest.json --require-formats
sudo bash tests/scripts/verify_rw6d_linux_reverse.sh reverse.img \
    windows-mutated.manifest.json linux-reverse.manifest.json
```

The Linux fixtures use 4 KiB blocks because `mkfs.xfs` cannot make a 1 KiB
directory block, and the writable policy requires `dirblklog == blocklog`.
The 1 KiB multi-level node and attribute-BMBT cases are produced by the
Windows formatter and verified by Linux; the 4 KiB Linux fixture reaches a
level-2 node with 240-byte attribute names.

### Local evidence (not qualification)

Observed on the development machine on 2026-10-25:

* full suite: **181 tests passed**, including twelve RW-6D tests and the
  ATTRI/ATTRD log scan test;
* `--make-rw6d-fixture` produced a 1 KiB, rmapbt+reflink image containing
  shortform, leaf, level-2 node, remote-value and attribute-BMBT forks,
  directory and symlink attributes; `xfswin check` reported 40 OK checks and
  the independent oracle accepted every inode and value;
* `rw6d_windows_mutate.py` completed the full Windows mutation, including
  data writes, rename/hardlink/unlink, a reflink clone and a level-2 tree
  replacement; `xfswin check` and the oracle accepted the result;
* `rw6d_crash_windows.py` recovered **66/66** injection points (set/replace/
  remote/local, node split/merge, BMBT growth, directory attrs, unlink and
  rename) with a clean log, a passing `check`, and an old-or-new manifest;
* `rw6d_enospc_windows.py` exhausted **335,118,336 bytes**, refused four
  remote operations with ENOSPC, kept every existing value and the whole
  structure unchanged, accepted a shortform set at zero free blocks, and
  accepted the same remote set after freeing the filler;
* the Linux-generated 4 KiB fixtures and their kernel/xfs_repair results are
  produced only in CI; no local result is claimed as Linux evidence.

## Qualification ledger

A row may be marked **verified** only when the linked CI run exists and passed.
Local Windows results are support evidence, not Linux evidence.

| # | Gate | Local status | Linux/independent evidence |
|---|---|---|---|
| 1 | Windows 1 KiB formatter fixture: all representations accepted by `xfs_repair -n` and mount | fixture, check, oracle pass | pending |
| 2 | Linux-created 4 KiB fixtures, both reflink profiles: Windows reads every value, `check` and oracle pass | tools implemented | pending |
| 3 | Windows mutation: shortform/leaf/node/level-2/remote/BMBT, namespaces, dir/symlink, data, rename/link/unlink, reflink | local fixture pass | pending |
| 4 | DA node split/merge/borrow/root collapse and node → leaf → shortform reverse transitions | unit matrix pass | pending |
| 5 | Remote value ordering, replace/free and crash consistency | unit + 66 crash images pass locally | pending |
| 6 | ATTRI/ATTRD parsing and completion, including Linux-logged replace transients | parser unit tests pass | pending: Linux emits these only in a `CONFIG_XFS_DEBUG` kernel with `larp=1`; the distro runner is non-debug, so this needs a dedicated debug-kernel run |
| 7 | Crash matrix: every injection point accepted by Linux with old-or-new state | 66/66 recovered locally | pending |
| 8 | ENOSPC: failed writes leave the image equivalent; shortform still works | 335 MB fill, unchanged oracle | pending |
| 9 | Linux → Windows → Linux churn and a final audit | local simulation pass | pending |
| 10 | `check` agrees with `xfs_repair -n` on attr extents, BMBT, remote blocks, `di_nblocks` and rmap ownership | check tests pass | pending |

## Known limitations

* ACL/security-descriptor translation and NTFS ADS are out of scope; bytes are
  preserved but not interpreted.
* Parent-pointer entries are readable/preserved, but parent-pointer
  filesystems are refused by the ordinary writable profile.
* The Windows writer does not log ATTRI/ATTRD; it uses one atomic metadata
  transaction with pre-flushed remote values. Linux's intents are replayed.
  Non-debug Linux kernels do not emit attribute intents either:
  `xfs_attr_want_log_assist()` returns false unless `CONFIG_XFS_DEBUG` is set,
  and even then the sysfs `larp` knob must be enabled. The Ubuntu CI runner is
  non-debug, so the parser has unit coverage and synthetic format tests but no
  genuine Linux ATTRI log yet; that gate stays pending rather than being
  claimed from Windows-generated data.
* The Linux 4 KiB fixture needs 9000 long-name attributes to force a level-2
  DA node; the Windows 1 KiB fixture reaches it with 140 entries.
* Attribute values larger than 64 KiB and realtime/external-log/zoned
  geometries are refused before the first write.
* Online repair, quotas, metadir and exchange-range operations remain outside
  the writable profile.
