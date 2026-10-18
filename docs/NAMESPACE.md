# Namespace mutations: rename, hard links and symlinks (RW-4)

This note documents the third group of writable namespace operations. It
follows the Linux implementation (`fs/xfs/xfs_inode.c:xfs_rename`,
`libxfs/xfs_dir2.c:xfs_dir_rename_children`, `xfs_symlink.c`,
`xfs_symlink_remote.c`) and the v5 on-disk format, and records exactly which
metadata each operation owns.

Scope of this milestone:

```
rename      same directory, cross directory, file/empty-directory replacement,
            directory moves across parents (with the ".." rewrite)
link        hard links to regular files and symlinks, link-count lifecycle
symlink     creation of real S_IFLNK inodes, inline and remote targets
```

Explicitly **not** in scope: xattr/ACL mutation, reflink/clone, quotas, parent
pointers, realtime, metadir, Windows-side dirty-log replay, and Windows
reparse-point (symlink) semantics. See the end of this document.

## Public API

```cpp
struct RenameOutcome {
  uint64_t moved_ino;          // the inode that changed name/parent
  uint64_t replaced_ino;       // 0 when the target name was free
  uint32_t replaced_new_nlink;
  bool replaced_zero_link;     // target is on the AGI unlinked list
};
Result<RenameOutcome> Rename(src_parent, src_name, dst_parent, dst_name,
                             replace_existing, defer_replaced_free);
Status Link(ino, dst_parent, dst_name);
Result<uint64_t> CreateSymlink(parent, name, target, mode, uid, gid);
```

`Rename` is the only entry point for the operation: the WinFsp callback and
the CLI resolve names and call it. Neither composes `DirRemove` + `DirInsert`
itself, because that cannot be atomic.

All three operations run inside one `XfsTransaction` and therefore one
committed log record. Everything below describes what that record contains.

## Rename: one transaction for the whole namespace flip

`xfs_dir_rename_children` is the reference. In transaction order:

```
1. target side
   - target name absent: xfs_dir_createname(target_dp, target_name -> src_ip)
   - target name present: xfs_dir_replace(target_dp, target_name -> src_ip)
     (the name, hash, leaf slot, bestfree table and btree keys do not change;
      only the data entry's inode number and ftype are rewritten)
   - target parent mtime + ctime
   - cross-parent directory move into a free name:
       target_dp di_nlink += 1   (the moved directory's ".." now counts here)
2. replaced target (only when the name existed)
   - di_nlink -= 1
   - when the source is a directory: di_nlink -= 1 again ("." link of the
     destroyed target directory)
   - ctime
   - if the new link count is 0: di_next_unlinked = AGI bucket head and the
     bucket head = the target agino (xfs_iunlink, in this same transaction)
3. moved inode
   - ctime
   - cross-parent directory move: rewrite ".." to target_dp
     (shortform header field or the ".." dirent in the data blocks)
4. source side
   - xfs_dir_removename(src_dp, src_name)
   - source parent mtime + ctime
   - di_nlink -= 1 when a directory left the parent, i.e.
       (source is a directory) and (cross-parent or a target was replaced)
       because the destroyed target's ".." reference replaced the moved one
       inside the same parent
```

The direction of each parent's link count is worth spelling out:

| operation | source parent | target parent |
|---|---|---|
| file rename, same or cross parent | unchanged | unchanged |
| directory rename, same parent, no target | unchanged | unchanged |
| directory rename, same parent, target replaced | -1 (destroyed target's "..") | same directory |
| directory move, free target name | -1 (source) | +1 (".." moved here) |
| directory move replacing an empty directory | -1 (source) | unchanged (destroyed target's ".." is replaced by the moved one) |

A directory's `di_nlink` is always `2 + number of subdirectories`; the
independent oracle recomputes it from the namespace on every image.

### Same-directory rename

`/a` → `/b` in one parent updates one directory (insert/replace then remove)
and the moved inode's ctime. The inode number, data, size, blocks and
`di_nlink` of the file do not change; only the directory entry moves. This is
never implemented as create + copy + unlink.

### Cross-directory rename

`/A/file` → `/B/file` touches two directory inodes, two parent link counts
(for directories), the moved inode (ctime and, for a directory, "..") and the
replaced target. Both parents may live in different AGs: the block and inode
allocators accumulate per-AG buffers and the transaction logs them all in the
same record.

### Directory moves and ".."

`xfs_dir_replace(moved_dir, "..", new_parent)` is applied through the moved
directory's own `DirectoryEditor`, never through a parent's editor. For the
shortform format this rewrites the header parent field (and adjusts the
8-byte-inode count when the new parent does not fit 32 bits); for block, leaf
and node formats it rewrites the `..` data entry in the block that holds it.
The moved directory's own listing cache is invalidated because its `..` entry
changed.

Moving a directory into its own subtree (including onto itself) is rejected
with `kWouldCycle` (Linux `EINVAL`) by walking `..` from the destination
parent before any editor is touched.

### Directories with attribute forks

Every inode the Linux kernel creates on this filesystem carries a reserved
attribute fork (`xfs_icreate_want_attrfork`: `di_forkoff =
xfs_default_attroffset >> 3` = 24 for 512-byte inodes, `di_aformat = EXTENTS`,
no attributes yet), and real xattrs can follow later. The directory writer
therefore confines every rewrite to the data fork:

- the data fork owns the literal area up to `di_forkoff * 8`
  (`XfsInode::data_fork`, already sliced by the core parser); the shortform
  size checks and the image rebuild inside `FinalizeInode` use that capacity,
  so a directory whose shortform data no longer fits converts to the block
  format exactly like the kernel does;
- the attribute fork bytes are part of the inode image the writer starts from
  and are never touched: the inode core keeps `di_forkoff`/`di_aformat`, and
  the logged region is only the data fork's share of the literal area;
- attributes are still neither interpreted nor mutated (xattr/ACL support is
  out of scope), but a rename, link, unlink or dirent edit inside such a
  directory preserves them byte for byte.

### Replace semantics

| source | target | result |
|---|---|---|
| file or symlink | regular file or symlink | target name points at the source inode; target `di_nlink -= 1`; if it reached 0 it goes on the AGI unlinked list, otherwise it keeps its other hard links |
| directory | empty directory (`nlink == 2`, only `.`/`..`) | target name points at the source directory; target inode loses both links and is unlinked |
| file | directory | refused `kNotRegularFile` (EISDIR) |
| directory | file | refused `kNotDirectory` (ENOTDIR) |
| anything | non-empty directory | refused `kDirectoryNotEmpty` before anything is modified |
| two names of the same inode | — | success, no metadata changed (POSIX/VFS `source == target`) |

The replaced inode is never freed inside the rename transaction. With
`defer_replaced_free == true` the caller keeps it and calls
`FreeUnlinkedInode` after the last handle closes (the open-but-unlinked
state); with `false` the same call does the rolling truncate + `xfs_ifree`
right away. A target that still has hard links is only decremented and is
*not* put on the unlinked list.

The WinFsp adapter tracks nodes by inode: a replaced target with an open
handle is marked delete-pending (its `Cleanup` must not unlink anything any
more; the last `Close` frees the zero-link inode), and a replaced target
without an open handle is freed immediately.

## Hard links (xfs_link)

```
dirent insert (xfs_dir_createname)
parent mtime + ctime
source di_nlink += 1, ctime
```

All in one transaction, no inode allocation, no data change. Directories are
refused (`kPermissionDenied`, XFS `EPERM`). The link count ceiling is
`XFS_MAXLINK = 2^31 - 1`: at the limit `Link` fails with `kTooManyLinks`
(`EMLINK`) and never wraps. Unlinking one of several names only decrements
`di_nlink`; the inode goes on the AGI unlinked list only when the last name is
removed, which is exactly the RW-3 `Unlink`/`FreeUnlinkedInode` path.

`di_nlink` counts names, not paths: the core and the adapter never assume that
an inode has one path. Two names of one inode share content, size, extents
and metadata; opening either name returns the same inode number.

## Symlink creation (xfs_symlink)

A symlink is a real inode with `di_mode & S_IFMT == S_IFLNK`, `di_nlink == 1`
and an empty attribute fork:

### Inline target (`XFS_DINODE_FMT_LOCAL`)

Used when `target.size() <= inode literal area` (`inode_size - 176`; 336 bytes
for a 512-byte inode):

```
di_format = 1 (LOCAL)
di_size   = target length (1..336)
di_nblocks = 0, di_nextents = 0
data fork = the target bytes, no terminator stored
```

The inode item logs `XFS_ILOG_CORE | XFS_ILOG_DDATA` with the target bytes;
`xfs_dinode_verify` accepts LOCAL whenever the size fits the fork.

### Remote target (`XFS_DINODE_FMT_EXTENTS`)

Used when the target does not fit the literal area. Up to
`XFS_SYMLINK_MAXLEN - 1 = 1023` bytes are allowed; the number of blocks is
`ceil(size / (block_size - 56))` (XFS_SYMLINK_BUF_SPACE). The writer allocates
one contiguous run in the parent's AG, inserts it at file offset 0 and writes
one v5 `xfs_dsymlink_hdr` per contiguous extent:

| offset | field | value |
|---|---|---|
| 0 | `sl_magic` | `XFS_SYMLINK_MAGIC` (`XSLM`, 0x58534C4D) |
| 4 | `sl_offset` | offset of this chunk inside the target string |
| 8 | `sl_bytes` | bytes of the target in this chunk |
| 12 | `sl_crc` | CRC32c of the whole buffer (this field zeroed) |
| 16 | `sl_uuid` | `sb_meta_uuid` |
| 32 | `sl_owner` | the symlink inode number |
| 40 | `sl_blkno` | buffer daddr in 512-byte units |
| 48 | `sl_lsn` | the transaction LSN |
| 56 | data | the target chunk |

The buffer is logged with `XFS_BLFT_SYMLINK_BUF`; the inode item carries the
`XFS_ILOG_DEXT` extent records. The kernel's `xfs_symlink_remote_read`,
`xfs_repair`'s `process_symlink_remote` and the independent oracle all check
the same fields, including the per-extent offset/length and the CRC. The
target itself is never parsed, resolved, escaped or required to exist; NUL
bytes are refused because they cannot exist in a kernel-created symlink.

On deletion a remote symlink is handled like `xfs_inactive_symlink_rmt`: the
inode is turned into a regular file and `di_size` becomes 0 in the same
transaction that frees the remote blocks, so a zero-length symlink is never
visible on disk.

## Open handles, cache invalidation and WinFsp

* `handle → inode` is stable across renames. `XfsFilesystem::Rename` only
  changes namespace metadata; the WinFsp `FileNode` keeps its inode and
  extent list, and `Rename` updates the node's cached parent/name when the
  handle belongs to the moved inode.
* Directory renames invalidate the **whole subtree's** path memo, not just
  `/A` and `/B`: the adapter drops both parent listings and clears the whole
  path→inode cache (`DropNamespaceCaches`), so `/A/x/y` cannot resolve to a
  stale inode.
* The core invalidates both parent directories and the moved/replaced inodes
  through `InvalidateCaches`; a directory move also invalidates the moved
  directory's listing (its `..` changed).
* Hard links mean an inode has several names. Nothing in the core or the
  adapter caches "the path of an inode"; the CLI and WinFsp resolve both names
  independently and both return the same inode number and data.
* Windows sharing rules (delete/rename sharing, sharing violations) stay in
  the WinFsp/Windows layer. The core only guarantees that an operation it
  accepts is written correctly.

## Errors

| case | result |
|---|---|
| source missing | `kNotFound` |
| target exists, `replace_existing == false` | `kAlreadyExists` |
| target non-empty directory | `kDirectoryNotEmpty` |
| file ↔ directory mismatch | `kNotRegularFile` / `kNotDirectory` |
| directory into its own subtree | `kWouldCycle` (EINVAL) |
| name too long / `.` / `..` / NUL | `kNameTooLong` / `kInvalidArgument` |
| source or parent immutable | `kPermissionDenied` |
| hard link to a directory | `kPermissionDenied` |
| `di_nlink` at `XFS_MAXLINK` | `kTooManyLinks` |
| symlink target empty / ≥ 1024 / NUL | `kNameTooLong` |
| no space / no inode / no record budget | `kNoSpace` / `kUnsupportedFeature` |

Every failure happens before a metadata write: the transaction is built in
memory and either refused during build or rejected by `BuildLogRecord` before
the log record lands. A rejected operation leaves the image byte-identical.

## Log-record budget

A cross-directory rename can touch two directories, several inodes, the AGI
and allocator metadata. The writer logs each buffer as 128-byte dirty chunks
(the difference to the current on-disk image) and deduplicates buffers
re-modified inside one transaction, so the record holds exactly the final
state of every touched buffer. If the framed record would still exceed the
32 KiB single-header limit, the commit fails with `kUnsupportedFeature`
before writing anything. The operation is never split into two transactions:
atomicity wins over accepting an impossible rename.

## Current limitations

* No xattr/ACL mutation, reflink/clone, quotas, parent pointers, realtime or
  metadir; those features are refused for read-write mounting.
* Windows symlinks stay the documented read-only view (the target is exposed
  as file content); no reparse-point tags are produced or consumed.
* Windows-side journal replay is still out of scope: the Windows writer always
  leaves a clean log when it finishes, and a dirty log is replayed by Linux
  (the CI oracle).
* Renames that cannot fit one log record are refused rather than split.
* The core serializes mutations with one coarse mutex; fine-grained inode
  locking is a later performance concern, not a correctness one.
