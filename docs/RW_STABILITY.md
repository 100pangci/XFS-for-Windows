# Supported read-write contract (RW-STABLE-1)

This document is the contract for the documented XFS read-write profile. It is
the authority for what "supported" means in this repository; the README, CLI
help and automount text summarize it and must not widen it.

The writer is not a complete Linux XFS replacement. It is a fail-closed writer:
a filesystem or device is either inside the documented profile and covered by
the evidence below, or a read-write mount is refused before the first write.

## 1. Supported scope

### 1.1 Filesystem writer

Supported for the allow-listed profile in section 2:

* direct image files, through the `xfswin` mutation commands and the WinFsp
  read-write mount;
* the resident automount service, after an explicit read-write enrollment that
  records the live device identity pins;
* journal replay and the recovery-only mount used by `xfswin recover`, with the
  same profile gate applied before any recovery write.

### 1.2 Device qualification (separate from the filesystem claim)

The filesystem writer can be correct while a particular transport or device is
not qualified. These are deliberately different statements:

| Target | Writer status | Hardware / device qualification |
|---|---|---|
| image file | supported | qualified by the CI image tests |
| attached VHD/VHDX | supported | DEV-1 qualified; virtual-disk identity pins |
| Samsung Flash Drive USB (serial `AA00000000000489`, USB BOT, 512/512-byte sectors) | supported | DEV-1R passed on 2026-11-01, **one device only** |
| other removable USB disks | available under the explicit opt-in | **not hardware-qualified**; no universal claim |
| fixed physical `\\.\PhysicalDriveN` | refused by default | **NOT READY**; explicit qualification-only opt-in |
| 4Kn devices | outside the profile | **NOT QUALIFIED** |
| UASP transports | outside the qualification evidence | **NOT QUALIFIED** |
| media power-loss durability | not claimed | device caches are not modelled as a power-loss guarantee |

A successful hardware smoke test does not widen the filesystem profile, and a
supported filesystem profile does not qualify a new device.

## 2. The allow-listed profile

A writable mount requires every one of these:

```text
XFS v5, CRC verified, ftype=1, attr2
one internal log, one data device, no realtime device
512-byte filesystem sectors and 512-byte log sectors
log stripe unit 0
dirblklog == 0 (one filesystem block per directory block)
at most 64 inodes per filesystem block
allocation groups smaller than 2^31 blocks when reflink is enabled
no quota, no external log, no metadir, no zoned/zone-gaps, no needs-repair
no unknown incompatible, read-only-compatible, compatible or log-incompatible bits
no asciici directories
```

These features are maintained by the writer when present and are otherwise
absent without changing the claim:

```text
finobt, inobtcount, sparse inodes, bigtime, NREXT64
rmapbt, reflink (refcountbt + CoW)
parent pointers (PARENT)
exchange-range as a feature bit (the XMI/XMD operation is not implemented)
XFS_SB_FEAT_INCOMPAT_LOG_XATTRS
meta-uuid
```

Refused before the first write, with the blocking feature named: metadir,
zoned, zone gaps, needs-repair, quotas, realtime devices, external logs,
asciici, multi-block directory blocks, 4Kn/alternate sector geometry, log
stripe units, unsupported inode geometry, unknown feature bits, and
exchange-range operations in the log.

Read-only access is more permissive and is not changed by this contract.

## 3. The ten promises

Each promise names the invariant owner and the evidence that exercises it.
A promise is not satisfied by a code comment; it is satisfied by a test or an
external oracle.

### P1. No illegal on-disk state

Every mutation is a transaction against structures this build maintains; the
allow-list in section 2 is the precondition. The writer never "best-effort"
updates a structure it does not own.

* Invariant owners: `CheckWritableProfile` (admission), transaction writer
  (`src/write`), journal/replay (`src/recovery`).
* Evidence: full unit suite; stateful model and fuzz jobs; every Windows
  writer artifact is judged by Linux `xfs_repair -n`, a kernel mount and the
  independent Python probes; `linux-verify-windows`, `linux-verify-rw*`,
  `linux-verify-rw6*`, `linux-stateful-fuzz`, `linux-verify-stateful-windows`.

### P2. Unsupported features fail closed before the first write

No writable mount or mutation may write a byte when the profile is not
allow-listed. The profile check runs before clean-log recovery writes, before
dirty-log replay and before the mount becomes writable.

* Invariant owners: `XfsFilesystem::Mount`, `CheckWritableProfile`,
  `RecoverJournal`.
* Evidence: `rw_stable_clean_log_recover_checks_profile_before_first_write`,
  `rw_stable_recover_journal_checks_profile_before_replay`,
  `writable_profile_allow_list`, and
  `tests/tools/rw_stable_refusal_matrix.py`, which patches 17 independent
  superblock fields, repairs the CRC, runs a mutation with `--rw`, and proves
  both a non-zero exit and a byte-identical image digest. The same tool proves
  the unpatched control mutation is accepted.

### P3. WAL and durability ordering

The log record is written and flushed before the metadata it describes.
Metadata write-back is deferred only while the log tail pins the oldest
transaction; a durability boundary (Flush, clean unmount, log wrap) cannot
publish metadata ahead of its WAL record. A commit failure past log durability
disables further writes instead of reporting success.

* Invariant owners: `TransactionWriter`, log tail pinning, clean-unmount
  record, `Flush`.
* Evidence: `transaction_writes_log_before_metadata`,
  `transaction_larger_than_one_record_is_split_and_recovers`,
  `conc_flush_waits_for_in_flight_write`,
  `perf_deferred_timestamps_are_visible_and_flushed`,
  `rw_stable_commit_failure_after_log_flush_isolates_mount`, and the crash
  injection jobs (`windows-rw6-crash`, `windows-rw6b-crash`,
  `windows-rw6c-crash`, `windows-rw6d-crash`, `windows-verify-rw2-crash`,
  `windows-verify-rw3-crash`, `windows-verify-rw4-crash`).

### P4. Crash and recovery legality, accepted by Linux

A crash may leave old or new committed state, never a third state. Records are
replayed only when committed; torn tails are trimmed; unsupported or
unprovable intents are refused rather than half-applied. Linux/xfsprogs judges
the result.

* Invariant owners: `ScanLog`, `ReplayLog`, intent completion, AGI unlinked
  processing, CoW staging recovery.
* Evidence: `linux-verify-crash`, `linux-verify-rw2-crash`,
  `linux-verify-rw3-crash`, `linux-verify-rw4-crash`,
  `linux-verify-rw6-crash`, `linux-verify-rw6b-crash`,
  `linux-verify-rw6c-crash`, `linux-verify-rw6d-crash`,
  `linux-verify-recovery-crash`, `windows-recovery-crash-matrix`,
  `windows-recover-windows-crash`, and the buffer-cancellation jobs
  (`windows-recover-linux-buffer-cancel`,
  `linux-recover-windows-buffer-cancel`).

### P5. Windows writer to Linux oracle

A filesystem written by Windows must mount, read, repair-check and scrub on
Linux for the documented profile.

* Evidence: `linux-verify-windows`, `linux-verify-rw`,
  `linux-verify-rw2`, `linux-verify-rw3`, `linux-verify-rw4`,
  `linux-verify-rw6*`, `linux-verify-rw6b-*`, `linux-verify-rw6c-*`,
  `linux-verify-rw6d-*`, `linux-verify-rw6e-*`, plus the independent Python
  probes in `tests/tools`.
* The alias-equivalence images are also judged by Linux:
  `tests/scripts/verify_rw_alias_linux.sh` runs `xfs_repair -n`, mounts each
  image read-only and compares the Windows-recorded content hashes.

### P6. Linux writer to Windows interoperability

Linux-created and Linux-mutated filesystems must remain readable and
writable by the supported profile, and Windows changes must remain legal to
Linux.

* Evidence: `windows-read-linux-image`, `windows-read-rw6b-rmap`,
  `windows-rw6b-mutation`, `windows-rw6b-reverse`,
  `windows-rw6b-enospc`, `windows-rw6c-roundtrip`,
  `windows-verify-rw6c-reverse`, `windows-rw6d-native`,
  `windows-rw6d-roundtrip`, `windows-verify-rw6d-reverse`,
  `windows-rw6e-linux-roundtrip`, `windows-verify-rw6-reverse`,
  `windows-verify-rw2-reverse`, `windows-verify-rw3-reverse`,
  `windows-verify-rw4-reverse`, and the corresponding
  `linux-verify-*-final` jobs.

### P7. ENOSPC cannot silently corrupt

Allocation failure leaves the old state intact or returns a clear error; it
never publishes a partially written transaction, a dangling extent, or a
success result.

* Invariant owners: allocator reservations, transaction commit order,
  `ENOSPC` error propagation.
* Evidence: `rw6b_allocator_enospc_reservation_and_release`,
  `rw6d_enospc_preserves_existing_attributes`, `conc_enospc_free_and_reallocate`,
  stateful ENOSPC sequences in `windows-build-test`, and
  `windows-rw6b-enospc`, `windows-rw6c-enospc`, `windows-rw6d-enospc`,
  `linux-verify-rw6b-enospc`, `linux-verify-rw6c-enospc`.

### P8. Concurrency never exposes neither-old-nor-new

Readers see the old value or the new value. Namespace operations serialize
the directory entry and its reverse parent record in one transaction. Cache
generations prevent an old parse from being republished after inode reuse.

* Invariant owners: per-inode locks, path/directory/info cache generations,
  transaction scheduler.
* Evidence: the `conc_*` suite (namespace races, same-inode writes, directory
  enumeration, rename/unlink, reflink CoW, stale caches), deterministic
  scheduler replay, `linux-concurrency`, `linux-concurrency-sanitizer`,
  `linux-concurrency-tsan`, `linux-verify-conc-windows`,
  `linux-verify-conc-winfsp`, `windows-concurrency-core`, and the WinFsp
  runspace test in `windows-mount`.

### P9. Device failure never reports false clean or durable success

A failed write, flush or device open must not leave the mount claiming a clean
log or a durable boundary. A read-write mount requires a writable, exclusive
device; the log is flushed before metadata; flush errors propagate.

* Invariant owners: block-device error classification, `Flush`, clean-unmount
  record, device policy.
* Evidence: `device_error_classification`, `device_write_policy_matrix`,
  `device_expected_identity_pin`, `device_expected_class_pin`,
  `write_rejected_on_read_only_mount`,
  `mount_refuses_read_write_on_read_only_device`, and the raw-device/VHD jobs.

### P10. No sudden read-write eligibility loss on upgrade

An ordinary filesystem written by this stable profile must not lose ordinary
read-write access solely because the user upgraded to a later stable build.
Narrowing the profile requires a correctness reason, documentation, a
fail-closed refusal and no destructive migration.

Current compatibility mechanisms:

* the allow-list is evaluated from the on-disk superblock, not a version tag;
* `--rw-experimental` remains a deprecated alias for the same `read_write`
  flag, and all qualification/device checks still run;
* automount accepts `experimental-rw`, `experimental`, `rw` and `read-write`,
  and rewrites a legacy entry as `mode = read-write` on the next save while
  preserving every identity pin;
* `rw-stable-refusal` and `rw-stable-alias` run on every push/PR, and the
  alias images are judged by Linux.

## 4. Dirty-log semantics

* A read-only mount refuses a dirty or unknown log unless the caller asks for
  the unsafe pre-recovery view (`--force-unsafe-ro`).
* A read-write mount replays a recoverable dirty log before it exposes the
  filesystem. The writable profile is checked before replay.
* A clean log can still carry AGI unlinked inodes or CoW staging extents; both
  are completed on every writable mount, matching the Linux kernel.
* A standalone `xfswin recover` is recovery-only: it finishes intents and
  writes a clean unmount record, then refuses to remain a general writable
  mount.
* Unknown, malformed, unsupported or unprovable log content is refused before
  the first write. Recovery is not repair.

## 5. Fail-closed evidence matrix

`tests/tools/rw_stable_refusal_matrix.py` is the before/after digest proof for
the admission gate. It patches the primary superblock, recomputes the
superblock CRC, runs `create ... --rw` on the copy, and requires:

1. non-zero exit;
2. the whole image digest unchanged;
3. the expected feature named in the refusal (where the message is stable);
4. an unpatched control copy accepts the same mutation.

Covered branches (17):

```text
metadir, zoned, zone-gaps, needs-repair
unknown incompatible bit, unknown read-only-compatible bit,
unknown log-incompatible bit
quota flags, quota inode
realtime device
asciici version bit
ftype=0
4K filesystem sector
log stripe unit
dirblklog != 0
external log
inconsistent inodes-per-block
```

The unit suite covers the same admission logic at the API boundary
(`writable_profile_allow_list`) and the ordering guarantees
(`rw_stable_clean_log_recover_checks_profile_before_first_write`,
`rw_stable_recover_journal_checks_profile_before_replay`).

## 6. Concurrency scope and nonclaims

CONC-1 provides deterministic schedules, not a proof over every possible
interleaving. The evidence covers the tested namespace, data, xattr, reflink,
cache and flush interleavings, including seeded stress runs. The contract does
not claim that every theoretical schedule has been enumerated, and the
sanitizer/TSan jobs are detection tools, not formal verification.

## 7. CLI and configuration compatibility

### 7.1 CLI

* `--rw` is the canonical read-write switch for every command.
* `--rw-experimental` is a deprecated alias: it sets the same internal
  `read_write` flag and prints one warning. It does not bypass the profile,
  device policy, class opt-in, identity pins, system/pagefile refusal or
  volume ownership.
* The alias retains the legacy automount-enrollment qualification requirement
  so old scripts behave as before; canonical `--rw` uses the ordinary
  identity-pin confirmation.
* Destructive operations keep their own confirmations: `format` and `recover`
  still require `--yes-i-really-mean-it`.

### 7.2 Automount configuration

* canonical serialization is `mode = read-write`;
* the loader accepts `read-write`, `rw`, `experimental-rw` and
  `experimental`;
* the next save normalizes the spelling and preserves UUID, serial, stable
  id, size, device class and qualification flags;
* an enrolled UUID that does not match the discovered filesystem is refused
  for read-write, and the live device class is used when an enrolled class is
  unknown.

### 7.3 Alias equivalence

`tests/scripts/verify_rw_alias_windows.ps1` starts from two byte-identical
images, runs the same semantic workload through `--rw` and
`--rw-experimental`, and compares names, modes, link counts, sizes, extent
counts, symlink targets, xattr values and content hashes. The journals are
not required to be byte-identical. The images are then judged by
`tests/scripts/verify_rw_alias_linux.sh` (repair, mount, content hashes).

## 8. Version compatibility

The profile is a property of the on-disk superblock, not of the build number.
A future stable build:

* must keep accepting a filesystem that satisfies section 2;
* must not silently lower a mount to read-only when the user asked for
  read-write;
* if it narrows the profile, must document the correctness reason, refuse
  before the first write, and provide a non-destructive way to inspect or
  migrate the filesystem. A destructive in-place migration is not an
  acceptable upgrade path.

## 9. Known limits and nonclaims

* No quotas, realtime devices, external logs, metadir or zoned filesystems for
  writing.
* Exchange-range operations (XMI/XMD) are not implemented; the feature bit is
  accepted.
* `asciici` directories, multi-block directory blocks, 4Kn/alternate sector
  geometry and log stripe units are refused for writing.
* No ACL/security-descriptor translation; ACLs and security xattrs are
  preserved as opaque bytes.
* No NTFS alternate data streams.
* No online repair; `check` is read-only and recovery is journal replay.
* Fixed physical-disk read-write is NOT READY. The DEV-1R removable result is
  one device only and does not generalize to other removable media, UASP, 4Kn
  or power-loss media durability.
* No formal verification and no proof over all concurrency schedules.
* The writer does not log ATTRI/ATTRD for ordinary user xattrs; it uses one
  atomic transaction. Linux ATTRI/ATTRD and PPTR records are replayed.

## 10. Audit findings fixed on this branch

The RW-STABLE-1 audit fixed these correctness findings, one commit each:

| Finding | Fix |
|---|---|
| A commit failure after log durability could leave a mount reporting further writes as successful | disable writes after the failure |
| Clean-log recovery wrote AGI unlinked work before checking the writable profile | profile check before recovery writes |
| `RecoverJournal` could replay before the profile gate | gate recovery first |
| The writer accepted ftype=0, alternate sector geometry and log stripe units | enforce the documented geometry |
| `dblocks * block_size` could overflow and let an undersized image through | reject overflowing geometry |
| Read-write could proceed without a verified superblock CRC | require CRC verification |
| An unreadable AGI silently fell back to a mount-time LSN | fail the recovery |
| A pending ATTRI intent was not treated as supported | handle ATTRI as a supported intent |
| A deferred timestamp could be hidden by an info-cache fill | keep it visible across the fill |
| In-place overwrite of a reflink-shared extent would modify both files | refuse with `kWriteNeedsCow` before data writes |
| Automount could bind an enrolled UUID/class to a different live target | pin UUID and live device class |
| `check` reported OK for an unreadable primary superblock | report it as corruption |
| A malformed logged btree root was silently ignored | reject it in `BmbtToBmdr` |
| The WinFsp path cache could reuse an inode generation | key the cache by inode generation |
| A read-write mount refused a recoverable dirty log discovered at mount time | let read-write mounts replay it |

## 11. Evidence ledger

The `rw-stable-qualification` CI job requires the following jobs to pass; it
does not run filesystem code itself.

| Area | CI jobs |
|---|---|
| build, unit, sanitizers | `linux-build-test`, `linux-sanitizer`, `windows-build-test` |
| concurrency | `linux-concurrency`, `linux-concurrency-sanitizer`, `linux-concurrency-tsan`, `windows-concurrency-core`, `linux-verify-conc-windows`, `linux-verify-conc-winfsp` |
| stateful/model/fuzz | `linux-stateful-fuzz`, `linux-stateful-fuzz-sanitizer`, `linux-verify-stateful-windows` |
| Windows writer → Linux oracle | `linux-verify-windows`, `linux-verify-rw`, `linux-verify-rw6-modern`, `linux-verify-rw6c`, `linux-verify-rw6d` |
| Linux writer → Windows | `windows-read-linux-image` |
| WinFsp read-write | `windows-mount` |
| ENOSPC | `windows-rw6b-enospc`, `windows-rw6c-enospc`, `windows-rw6d-enospc` |
| crash/recovery | `windows-rw6b-crash`, `windows-rw6c-crash`, `windows-rw6d-crash`, `windows-recover-linux-logs` |
| parent pointers | `linux-verify-rw6e-mutation` |
| DEV-1 / VHD | `windows-rawdevice-vhd`, `windows-vhd-device`, `windows-vhd-winfsp`, `linux-verify-windows-vhd` |
| automount and UX takeover | `windows-automount-events`, `windows-automount-takeover`, `windows-automount-vhd`, `linux-verify-ux-rw-image` |

Additional jobs outside the aggregate (buffer cancellation, cross-recovery,
reverse mutation, scrub, perf) remain part of the full CI signal and are
listed in `docs/TESTING.md`; the aggregate is the minimum gate, not a claim
that the other jobs are optional.

## 12. Final decision

**RW STABLE: READY** for the documented profile in section 2, on the
qualified targets in section 1, with the nonclaims in section 9. The decision
is `NOT READY` if any of the following is true: a supported-profile mutation
produces on-disk state that Linux rejects; a profile refusal writes before it
fails; recovery leaves an illegal state; the alias or configuration migration
loses identity pins; or the aggregate qualification job is red.

The final audit found no blocking RW-stability finding beyond the fixes listed
in section 10.
