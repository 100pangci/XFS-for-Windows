# Concurrent FUZZ-2 crash integration

The hooks-enabled `stateful_fuzz` target can execute an operation on one
`Controller` worker while a second worker performs a stable root lookup or
`Flush`. The sequential FUZZ-2 model remains the source of truth: it is updated
on the driver thread after both workers finish. Crash runs execute the
operation prefix sequentially, issue an explicit flush boundary, then schedule
the crash target with the observer. Namespace operations remain serialized by
the filesystem namespace barrier.

## Build and CTest

Configure and build the dedicated hooks-enabled tree:

```bash
cmake -S . -B build-concurrent-crash \
  -DXFSWIN_BUILD_TESTS=ON -DXFSWIN_BUILD_WINFSP=OFF \
  -DXFSWIN_CONCURRENCY_HOOKS=ON -DXFSWIN_TEST_HOOKS=ON \
  -DXFSWIN_WARNINGS_AS_ERRORS=ON
cmake --build build-concurrent-crash --target stateful_fuzz xfs_tests -j
ctest --test-dir build-concurrent-crash -R 'stateful_(schedule_reducer|range_oracle|concurrent_crash_smoke)' --output-on-failure
```

The bounded crash test records and replays schedules for four fixed crash
cases, runs project recovery, validates recovered images with the independent
raw stateful oracle, and checks them with `xfs_repair -n`. It also mutates
expected unrelated file data, hard-link count, and xattr values and requires the
raw oracle to reject each mutation. A successful run removes its large images;
failed runs retain images and logs. The test requires Python 3 and `xfs_repair`,
and it does not mount an image or require root. It only needs
`XFSWIN_CONCURRENCY_HOOKS`; the dedicated local build below also enables
`XFSWIN_TEST_HOOKS` as requested.

Run a longer record/replay/recovery pass manually with:

```bash
python3 -I tests/scripts/concurrent_stateful_crash.py \
  --exe build-concurrent-crash/tests/stateful_fuzz \
  --work-dir out/management-validation/concurrent-crash-soak --mode soak
```

Artifacts include the fixed operation list, flushed scheduler choices,
record/replay/recovery logs, and the recovered manifest. Successful runs remove
their large images; failures retain the image and logs. Each choice is persisted
before `Controller::Step`, so the schedule file retains the attempted
interleaving if crash injection exits the process. Replay rejects invalid
choices, choices that cannot currently run, early exhaustion, and unused
trailing choices.

For root-only Linux qualification, `--kernel-replay` passes a byte-for-byte
copy of each dirty crash image to `verify_stateful_linux.sh --kernel-replay`
before project recovery. `--kernel-recovered` runs the same verifier on the
project-recovered image. Both mount loop images and belong only in a prepared
Linux CI environment; the bounded CTest invokes neither option.

The regular concurrency stress schedule matrix remains available separately:

```bash
python3 -I tests/scripts/concurrency_stateful_matrix.py \
  --exe build-concurrent-crash/tests/xfs_tests \
  --work-dir out/management-validation/concurrency-smoke --mode smoke
python3 -I tests/scripts/concurrency_stateful_matrix.py \
  --exe build-concurrent-crash/tests/xfs_tests \
  --work-dir out/management-validation/concurrency-soak --mode soak
```

The existing `linux-concurrency` Actions job configures
`XFSWIN_CONCURRENCY_HOOKS=ON`, builds all test targets, runs the full CTest suite
(which includes all three stateful tests above), and runs the regular seeded
schedule matrix. A scheduled soak can run the `--mode soak` commands above and
archive the work directory on both success and failure. The FUZZ-2 feature does
not require `XFSWIN_TEST_HOOKS`; the dedicated local configuration enables both
hook sets. Windows driver concurrency remains covered by
`verify_mount_concurrency_windows.ps1` in the Windows Actions job.
