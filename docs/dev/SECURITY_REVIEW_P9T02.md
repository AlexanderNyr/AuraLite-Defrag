# Security review — handles, paths, TOCTOU (P9-T02)

MUST (R-084, R-081). Owner: Security reviewer. Inputs: P1-T07, P3-T05/T07, S-15/S-16.
Severity follows the scale in [THREAT_MODEL.md](THREAT_MODEL.md).

The review asks three questions and this document answers them with evidence:

1. every volume and file handle is opened with the least access it needs, and closed on
   every path;
2. every resolved path stays inside the volume being operated on;
3. nothing between analysis and move can be tricked into acting on a different file.

## Summary

| Severity | Found | Closed | Open |
|---|---|---|---|
| Critical | 0 | 0 | 0 |
| High | 3 | **3** | 0 |
| Medium | 2 | **2** | 0 |
| Low | 5 | 2 | 3 |
| Informational | 1 | 0 | 1 |

Accept criterion 1 (0 Critical/High open) is met. The three High findings were all the
same class of defect and were found by running the product on a real volume, not by
reading it — which is itself the main lesson of this review.

## Findings

### F-01 · An extent read that failed was counted as "not fragmented" — **High · Closed**

`Enumerate` called `ReadRetrievalPointers` and, on failure, fell through an `if` with no
`else`: `fragmentCount` stayed 0, which the aggregation reads as "one extent, contiguous".
A volume on which no file's layout could be read reported `Fragmented: 0`, `Volume Frag:
0.00%` — the tool said there was nothing to do while being blind to every file.
**Fix:** failures are counted (`extentReadFailures_`), the file is marked locked rather
than contiguous, and the CLI prints an extent census so the state is visible.

### F-02 · `FSCTL_*` issued on handles without `FILE_READ_DATA` — **High · Closed**

`FSCTL_GET_NTFS_VOLUME_DATA` and `FSCTL_GET_VOLUME_BITMAP` ran on a volume handle opened
with `FILE_READ_ATTRIBUTES` only. The ioctl failed with `ERROR_ACCESS_DENIED`, which the
backend then misreported as `E_PREFLIGHT_UNSUPPORTED_FS` and `E_BITMAP_IO` on healthy
NTFS volumes. **Fix:** `OpenVolumeForFsctl()` opens with `FILE_READ_DATA` and falls back
to attributes only so the failure can be reported as "not elevated" instead of "bad
filesystem"; every FSCTL now goes through it.

### F-03 · `OpenFileById` called with a truncated file reference — **High · Closed**

USN records carry a 64-bit reference (48-bit MFT index + 16-bit sequence). Enumeration
stored only the index, and `OpenFileById` matches on the pair, so **every** open failed;
the failure path set `isLocked = true` with no error code recorded. On a 160,129-file
volume the census read `locked=144836` — all of them — and the tool still printed a
complete-looking report. **Fix:** the full reference is cached and tried first, the bare
index second; every failed attempt records its `GetLastError()` and the trace is printed
whenever the census is blind.

### F-04 · `--target` matched by raw string prefix — **Medium · Closed**

`file`/`folder` selection compared `options.targetFolderPrefix` with `FileEntry::path`
using a case-insensitive `IStartsWith`. Real paths come from `GetFinalPathNameByHandleW`
in extended-length form (`\\?\D:\Data\a.bin`) while `--target` arrives as `D:\Data`, so
the comparison could never succeed on a real volume — **while passing on the emulator,
whose paths are plain**. A second defect sat behind the first: a prefix match without a
separator boundary made `D:\Data` select files in `D:\Database`.

**Fix:** `ExclusionEngine::IsWithinScope` canonicalises both sides (strips `\\?\`,
lowercases, folds separators, collapses runs) and requires the boundary to be a
separator. The planner uses it, the CLI refuses a `--target` that is not on the volume
being operated on (exit 2), and it warns when a target matched no file.
**Tests:** SEC-02/SEC-03 in `tests/unit/test_main.cpp`; the folder case fails on the old
comparison (verified by reverting it and observing `FAIL … sawA`).

### F-05 · Cluster protection applied to read-only analysis handles — **Medium · Closed**

`OpenFileByReference` issued `FSCTL_MARK_HANDLE` with `MARK_HANDLE_PROTECT_CLUSTERS` for
every handle it returned, including the ones used only to read retrieval pointers during
`analyze`. That withholds those clusters from Volume Shadow Copy and from any other
defragmenter for the lifetime of the handle — a privileged side effect nobody asked for,
applied 144k times per analysis. **Fix:** protection is a parameter, requested only by
`MoveExtent`.

### F-06 · The bitmap is a point-in-time snapshot — **Low · Open, accepted**

`FSCTL_GET_VOLUME_BITMAP` and `FSCTL_MOVE_FILE` are documented to race on a volume with
write activity: a move can target a cluster that has been allocated since the bitmap was
read. The engine maps the resulting failure to a retryable code, retries up to three
times, then records a `Skip` with `StaleBitmapConflict` in the journal. Residual risk:
no pre-move re-read of the bitmap. Accepted — the failure is detected and never silent.

### F-07 · `Flags()` returns empty flags for an unknown reference — **Low · Open**

On a cache miss `Flags()` returns a default-constructed `FileFlags` instead of an error,
so an unknown file reads as "plain, movable". Callers currently use the enumeration
cache and the planner uses `FileEntry::flags`, so nothing takes that path today; the API
shape is still wrong. Recorded for the next API revision rather than changed under
release pressure.

### F-08 · Shell-extension argument quoting — **Low · Open (latent)**

`context_menu.cpp` builds `--target "<path>"` by concatenation. A directory path ends
with a separator, giving `--target "D:\Dir\"`, where `\"` is an escaped quote for
`CommandLineToArgvW` and the argument list breaks. `cliPath_` is also passed unquoted.
The extension is not registered in v1 (no `GetModuleFileNameW` in the tree), so this is
latent, not live. **Status:** quoting fixed for trailing separators; registration
remains out of scope until S-08.

### F-09 · Journal file name is independent of the volume — **Low · Closed**

The default journal was `%TEMP%\auralite-cli.journal` for every volume. A stale journal
from another volume is correctly refused by the serial and plan-hash checks, so no
resume ever crossed volumes, but the file was silently reused. **Fix:** the volume id is
part of the name.

### F-10 · The CLI has no console control handler — **Informational**

`Ctrl+C` terminates the process (no `SetConsoleCtrlHandler` anywhere in `src/`), so the
`CancelToken` created at `cli_app.cpp` is never cancelled and the graceful path
(`journal.Commit()` / `Seal()`) does not run. Data safety does not depend on it: each
step is a transactional `FSCTL_MOVE_FILE`, and the write-ahead journal records
`MOVE_INTENT` with a flush **before** the move, then reconciles by verifying the extent
on the next run. Recorded as a usability gap, not a data-integrity one.

## What was checked and found sound

| Area | Evidence |
|---|---|
| Handle lifetime | `UniqueHandle` is move-only RAII; closes `INVALID_HANDLE_VALUE`-safe, no copy, no double close. Every `CreateFileW` in the tree is wrapped in one. |
| Least privilege, media probes | `\\.\Volume` and `\\.\PhysicalDriveN` opened with `FILE_READ_ATTRIBUTES`; only `MoveExtent` asks for write access, and only after pre-flight. |
| FRN → path resolution | Paths come from `GetFinalPathNameByHandleW` on the file's own handle, so a name cannot redirect resolution; USN names cannot contain a separator, so a crafted name cannot traverse. |
| CSV injection | Formula-leading cells neutralised with `'`, RFC 4180 quoting verified by SEC-04 (`TestCsvInjectionSafetyAndStatistics`). |
| Untrusted input | Scenario/config parsers bounded (1 MiB, 8 levels, allowlisted keys) and fuzzed to 6,000,000 executions with 0 findings (`FUZZING.md`). |
| Elevation | `IsCurrentProcessElevated` reads `TokenElevation`; a non-elevated run fails pre-flight with `E_PREFLIGHT_NOT_ELEVATED` before any write handle is opened. |

## A-08 confirmation (privilege split)

**Confirmed for v1:** one elevated process. Reasons, in order of weight:

1. the UI must open volume handles to draw the map, which is the same privilege that
   moves clusters — the split would duplicate the capability boundary without removing it;
2. the attack surface that matters is *parsing*, and the parsers already run with strict
   limits and fuzzing behind them;
3. no finding in this review is attributable to UI elevation.

The split becomes a post-v1 MUST if a future review finds at least one High finding that
is attributable to the UI running elevated, per the assumption's own escape clause.

## Residual risk

The NTFS backend is now exercised on one real HDD (D:, 160k files) and nowhere else.
Every High finding above was invisible on the emulator; the emulator cannot be the only
gate for this backend. P9-T05 (VHDX CI) is the mitigation and is not yet running.
