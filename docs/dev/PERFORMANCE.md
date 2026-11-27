# Performance: budgets, measurements, and what they actually prove (P9-T04)

The plan states numeric budgets for RH-1 — an i5-10400 with 16 GB and a 7200 rpm
disk. **None of those numbers were measured here.** This document says what was
measured, on what, and what a passing result means.

## Where the budget claims stand

| Budget (plan §1.3) | Target | Measured here | Host | Verdict |
|---|---|---|---|---|
| 4M-cluster aggregation | ≤ 50 ms | **1.8 ms** | 2 shared vCPU | Passes with 27× headroom |
| 64M-cluster aggregation | ≤ 400 ms | **11.8 ms** | 2 shared vCPU | Passes with 34× headroom |
| 1,000 incremental extents | ≤ 2 ms | **0.24 ms** | 2 shared vCPU | Passes |
| 1000 hit tests @ 250k files | ≤ 100 ms | **0.02 ms** | 2 shared vCPU | Passes (was 303 ms — see below) |
| 244M-cluster analysis + stats | ≤ 90 s | **146 ms** | 2 shared vCPU | Passes |
| Histogram @ 250k files | ≤ 50 ms | **2.8 ms** | 2 shared vCPU | Passes |
| Top-N 25 @ 250k files | ≤ 20 ms | **4.4 ms** | 2 shared vCPU | Passes |
| CSV export 250k rows | no ms budget | **85 ms** | 2 shared vCPU | Round-trip verified separately |
| Chart push (UI thread block) | ≤ 16 ms | **0.03 ms** | 2 shared vCPU | Passes |
| Map repaint (pixel buffer) @4K | ≤ 16 ms | **0.21 ms** | 2 shared vCPU | Passes (was 90,000 draw calls) |
| Map redraw end-to-end @4M | ≤ 16 ms | **not measured** | needs D2D | **Open** — GPU upload needs Windows |
| Peak memory, 1M files | ≤ 512 MB | **303 MB** | 2 shared vCPU | Passes |
| Move throughput | not specified | **0.14 ms/run** | emulator | **Not a throughput number** |

**Read the last two rows carefully.** Map redraw needs Direct2D, which does not
exist on this host; move throughput against an in-memory backend measures the
engine's bookkeeping, not a disk. Both remain open, and `P9-T05` (VHDX on
Windows) is what would close them.

## The two real findings

### 1. Hit testing was linear in the file count

`ClusterAggregator::HitTest` looked up the dominant file's name by scanning
`snapshot.files`. With 250,000 files that is a 250,000-element scan **per mouse
move**, measured at **0.30 ms** each — 303 ms for 1,000 hovers, against a
100 ms input-latency budget.

The fix records what a hover needs while the grid is being painted, where the
file record is already in hand. Cost: 4 ns per hit test, and a bounded amount of
work during aggregation. The grid **owns** the copied strings, because an
earlier version stored pointers into the caller's snapshot and dangled as soon
as that snapshot was replaced — a real lifetime bug that the unit test now
pins down.

Getting there took two wrong turns, both worth recording because both looked
correct:

| Approach | 4M aggregation | Why |
|---|---|---|
| Scan the file set per cell | 87 ms (was 1 ms) | Quadratic: 20,000 cells × 250,000 files |
| Index every file first | 18 ms build | 250k hash inserts for 11k needed entries |
| Fill during paint, dedupe by id | 1.8 ms total | Work proportional to cells, not files |

### 2. The benchmark was reusing a journal

`BinaryJournal` is bound to `(planHash, volumeSerial)` on purpose, and a second
defragmentation of an already-defragmented volume produces a different plan, so
the second repetition was refused with `E_JOURNAL_PLAN_HASH_MISMATCH` (6004) in
0.11 ms — and the harness reported 0.11 ms as a passing throughput number. The
guard is correct; the benchmark was wrong. Each repetition now gets a fresh
journal, as a real invocation does.

### 3. The map issued one draw call per cell

Measuring the 4K case exposed something the earlier budget checks could not see.
At 4K the grid is 400×225 — the maximum `mapCols`/`mapRows` the config allows —
and `RenderClusterMap` called `FillRectangle` once per cell: **90,000 draw calls
per frame**, every frame, on the UI thread. That is not a budget that tuning the
aggregation can meet; it is a budget that the architecture cannot meet.

The fix moves the pixel work to L5 where it can be measured without Direct2D:
`ClusterAggregator::BuildPixelBuffer` packs the grid into one row-major ARGB
array, and the renderer uploads it as a single bitmap (`CreateBitmap` +
`DrawBitmap`, nearest-neighbour so cells stay crisp). The per-cell path is kept
as a fallback if bitmap creation fails, so a failure degrades rather than
blanking the map.

| Grid | Cells | `BuildPixelBuffer` |
|---|---|---|
| 1080p (200×100) | 20,000 | 0.044 ms |
| 1080p wide (240×135) | 32,400 | 0.077 ms |
| 4K (400×225) | 80,000 | 0.216 ms |

**What is still not proven:** the Direct2D upload itself. The 0.21 ms is the CPU
half; whether `DrawBitmap` of an 80,000-pixel texture lands inside 16 ms depends
on the GPU and needs Windows. The 90,000-call version could not have met it on
any hardware, which is the part that was actually wrong.

### 4. The build did not build

Worth recording because it invalidates the "just compile it on Windows"
assumption. Checking the CMake path before writing instructions for it turned up
three defects that `make` never touched:

* `tests/crash/crash_child.cpp` contains `#error` under `_WIN32`, but the target
  was declared unconditionally — **the Windows build could not compile at all**.
* `auralite-crash-campaign` had no include directories, so it failed on
  `#include "backend/usn/usn_parser.hpp"` even on Linux.
* Every `add_test` ran from the build directory while the suites load
  `tests/scenarios/*.json` relative to the working directory: ctest reported
  4 of 8 failing on code that passed when run by hand.

`tools/bench_perf.cpp` also gains a Windows path for PT-MEM
(`GetProcessMemoryInfo`/`PeakWorkingSetSize` instead of `/proc/self/status`), and
its temp paths use `std::filesystem::temp_directory_path()`.

## The regression gate

`make perf-gate` compares a run against `perf/baseline.txt` and fails if any
measured path regresses beyond the threshold. `make perf-baseline` rewrites the
baseline deliberately.

Two honesty mechanisms are built in, because both problems were observed here:

* **Medians, not single samples.** `--reps 5` runs the suite five times and
  compares medians. On this host PT-02a swung 1.85→2.70 ms (+46%) between
  identical runs.
* **A noise floor.** Sub-millisecond results are reported but not gated. A 0.02 ms
  measurement moving to 0.03 ms is the scheduler, not the code.

The plan's threshold is **±20%** (P9-T04 criterion 3). The honest statement
about it in this environment: **this sandbox cannot support a 20% gate, and
pretending otherwise would produce a gate that fails on nothing.**

The evidence is direct. Three consecutive `--save-baseline` runs on unchanged
code gave PT-06b = 2.97, 4.46 and 5.06 ms — a 70% spread between identical
measurements. PT-06a moved 2.49 → 3.13 ms. With that distribution a 20% gate
reports the host scheduler, not the code; re-baselining then "fixes" it, and the
gate becomes a rubber stamp.

So the two claims are kept apart, deliberately:

| Command | Claim | Mode |
|---|---|---|
| `make perf` | Absolute budgets ("4M aggregation ≤50 ms") | **Blocking**, in CI |
| `make perf-gate` | Relative regression vs baseline | **Informational** here |

Absolute budgets survive this host because the measured values sit 20–30× under
their limits — noise cannot move 2.1 ms across a 50 ms line. The regression gate
is the one that needs a host that measures the same code the same way twice.

`make perf-gate` therefore defaults to `THRESHOLD=20 ENFORCE=0`: it reports
`+35.2%` and prints *"INFORMATIONAL MODE: 2 finding(s) reported, not failing the
build"*, exiting 0. On a dedicated runner (RH-2) it is the plan's gate:
`ENFORCE=1 make perf-gate`. Verified both ways — `--enforce 1` exits 1 on the
same numbers, and an artificial 2× slowdown in the paint loop produced +2104%
(PT-02a) and +322% (PT-04), so the mechanism itself is not decorative.

**This is an open item, not a closed one:** criterion 3 ("0 PT benchmarks
regress >20%") is *measured and reported* but not *enforced* until it runs on
RH-2. Recorded as such rather than claimed as met.

### 6. The remaining cost is one random read per file, so stop opening files

After the path fix and the worker pool the scan ran at 172 files/s. That is
5.8 ms per file with eight requests in flight, i.e. ~46 ms per request: the disk,
not the CPU, and not the API. Two syscalls per file are left
(`OpenFileById`, `FSCTL_GET_RETRIEVAL_POINTERS`) and each one seeks.

Everything those calls return is already written in the file's own MFT record:
the size, the allocation, the run list. So the fast path reads `$MFT` once -
314 MB on the reference volume, contiguous, its own run list being one fragment -
and parses the records in memory (`src/backend/ntfs/mft/`). One sequential read
instead of 160,000 random ones.

What the parser has to get right, because the whole report depends on it:

| Concern | Handling |
|---|---|
| Torn or corrupt records | the update sequence array is verified; a record that fails is counted, never trusted |
| Attributes split across records | `$ATTRIBUTE_LIST` is detected and the file is re-resolved through `OpenFileById` |
| Extension records | skipped; their data belongs to the base record |
| Sparse holes | omitted, exactly as `FSCTL_GET_RETRIEVAL_POINTERS` omits them, so the fragmentation count cannot drift |
| Resident data | flagged `resident`, not `empty` - the misreport that produced "0% fragmented" |
| Hostile field lengths | every attribute and run-length field is bounds-checked; `make fuzz` mutates a valid record 1,000,000 times under ASan/UBSan |
| The whole table unreadable | `Enumerate` falls back to the handle path; the fast path never fails an analysis |

The two engines must agree. `AURALITE_SCAN_ENGINE` exists so that claim is
checked rather than asserted:

```bat
set AURALITE_SCAN_ENGINE=handles
auralite-cli analyze --volume D:
set AURALITE_SCAN_ENGINE=mft
auralite-cli analyze --volume D:
```

The `Files:` and `Extents:` lines must match to the unit. `auto` (the default)
is `mft` with a fallback to `handles`.

### 7. `$MFT` cannot be opened as "file 0"

The first run of the fast path on the reference volume failed like this:

```
OpenFileById failed id=0 ref=0 tries=1 access=129 err=87
Access was denied to one or more locked files; affected items were skipped. (E_ENUM_ACCESS_DENIED)
```

`err=87` is `ERROR_INVALID_PARAMETER`, not a rights problem: `OpenFileById` with a
`FileIdType` descriptor of 0 is rejected outright. The table therefore cannot be
opened the way every other file is.

Record 0 sits at `MftStartLcn` by definition, so the bootstrap reads those 1024
bytes straight off the volume and takes `$MFT`'s run list from its own `$DATA`
attribute. Everything after that is arithmetic. Two consequences worth keeping in
mind:

* the fast path now contains no `OpenFileById` at all, so it cannot be blocked by
  a file being held open;
* `$MFT` may be fragmented, so stream offsets are mapped through the run list
  (`mft::ReadStreamRange`) rather than assuming one contiguous span. Reading a
  fragmented table as contiguous would misparse records silently, which is why
  that mapping is portable code with its own test against a two-fragment table.

`TestMftBootstrapFromRecordZero` builds a 64-cluster volume, places record 0 at
LCN 4 and the rest of the table at LCN 9, and checks that the scan returns all
four records in order across the gap.

### 8. A layout that lags behind a completed move

The first real move reported success and changed nothing:

```
[100.00%] 2/2 moves, 9,715 clusters
Moves completed: 2 | Verified: 2 | Skipped: 0
Files: 160,148 | Fragmented: 14 -> 14
```

Four runs in a row did exactly that, each one rotating the journal with
"different plan" - so the moves were real, the plan was new every time, and only
the *reported* layout stayed frozen. The verification path cannot be the liar: it
reads the file's retrieval pointers through a fresh handle, and it accepted both
moves.

Two things in the fast path can produce that:

1. **Raw sector reads bypass the NTFS metadata cache.** Reading `$MFT` off the
   disk misses the update the filesystem made a second ago - and a move updates
   exactly that record. `EnumerateViaMft` now flushes the volume
   (`FlushFileBuffers` on a write handle) before reading the table, so the raw
   path sees what the cache holds.
2. **`MARK_HANDLE_PROTECT_CLUSTERS` on the move handle.** The flag exists to keep
   *other* defragmenters off a file, it is scoped to the handle, and it was being
   set on the handle we then hand to `FSCTL_MOVE_FILE`. Clusters marked protected
   are by definition not movable by a defragmentation tool, which is us. It bought
   nothing (the handle is closed immediately afterwards) and could turn a move into
   a successful no-op. The move handle no longer sets it.

Reading through a handle on `$MFT` is still tried first - `FileIdType` rejects a
bare index 0, but the full reference (record 0, sequence 1) resolves - with the
raw path as fallback, and the probe trace now names which one ran:

```
Scan: MftScan via $MFT handle (cache-coherent)
Scan: MftScan via raw volume reads (may lag behind the cache)
Scan: MftScanFlushFailed err=5
```

Two more guards came out of it: `MoveExtent` refuses a move whose target equals
the extent's current LCN, and a single-target operation now prints the file's own
layout, read live after the operation:

```
Target: dsymutil: 2 extent(s) before -> 1 extent(s) now (live read)
  vcn 0 + 7568 @ lcn 1234567
  vcn 7568 + 2147 @ lcn 1250000
```

That line is ground truth. If it says one extent while the census above still
counts two, the analysis is what is broken, not the mover - and the emulator
cannot catch that mistake, because it has no metadata cache to be stale.

### 9. Adjacent runs are one extent, not two

The live readback settled it:

```
Moves completed: 2 | Verified: 2 | Skipped: 0
Target: dsymutil: 2 extent(s) before -> 2 extent(s) now (live read)
  vcn 0 + 7568 @ lcn 38431076
  vcn 7568 + 2147 @ lcn 38438644
```

38431076 + 7568 is 38438644. The file occupies **one unbroken stretch of disk**
and the mover had put it exactly there. The two "extents" are two adjacent
entries in the run list, which is what `FSCTL_MOVE_FILE` leaves behind when it
moves a file chunk by chunk into a single free run.

Everything downstream counted entries instead of discontinuities, so the file
looked fragmented forever: each run re-planned the same two moves, moved it to
another contiguous run, reported "2 moves, verified", and the census did not
budge. The journal rotated with "different plan" every time, which is the
signature - the targets were new, the layout was already fine.

`MergeAdjacentExtents` (src/core/types.hpp) now normalises both readers:
the MFT run-list decoder and `FSCTL_GET_RETRIEVAL_POINTERS`. The rule is strict -
`prev.startVcn + prev.clusterCount == next.startVcn` **and**
`prev.startLcn + prev.clusterCount == next.startLcn` - so a sparse hole (no LCN)
or a real gap in LCN is never merged away.

This also settles two older discrepancies: the census read higher than
`defrag.exe` ("1 фрагмент" on this volume), and the `2plus=` column counted run
entries rather than fragments. A file with two adjacent runs is not fragmented,
and no amount of moving it will ever make it more contiguous than it already was.

## What is still open

| Item | Blocked by |
|---|---|
| Map redraw ≤16 ms at 4M / 64M clusters | Direct2D — Windows only |
| PNG export 1080p / 4K ≤2 s | WIC — Windows only |
| Move throughput, ETA accuracy, verification overhead | Real volume — `P9-T05` |
| Bitmap read throughput (`FSCTL_GET_VOLUME_BITMAP`) | Real volume |
| Enumeration 250k files ≤45 s | Real volume |
| UI thread never blocked >16 ms over a 30-minute run | The GUI must exist |
| Analysis of a real 244M-cluster volume ≤90 s | Real volume; the aggregation half is measured, enumeration is not |

Memory was the one remaining item measurable here, and it is now measured:
303 MB peak for a 1M-file volume against a 512 MB budget. The measurement uses
`VmHWM`, the process high-water mark, so it includes the file set the analysis
builds rather than only the grid.

### 5. Analysis spent a syscall per file on naming it, and a seek per file on measuring it

The 160k-file analysis on the reference HDD ran at 74 files/s — 36 minutes. Two
separate costs were hiding in one loop.

**Paths.** Every file asked the filesystem for its own path with
`GetFinalPathNameByHandleW`, on top of the open that was already made for the
extents. The parent reference is already in the USN record the loop is walking,
so `PathComposer` (`src/core/path_tree.hpp`) builds the whole tree in memory and
resolves every path once, after the walk. Measured cost of composing 160,000
paths: **252 ms total, 0.0016 ms per path**. It also names files that could
never be opened, which the handle-based call could not.

**Layout.** `OpenFileById` plus `FSCTL_GET_RETRIEVAL_POINTERS` per file is a
random read into the MFT and into the file's own record. At 14 ms per file that
is seek time, not CPU time: a spinning disk with one outstanding request cannot
reorder anything. `RealNtfsBackend::ResolveLayouts` runs the measurement half on
a pool of workers, each with its own volume handle (never the shared cached one,
which a move or a remount invalidates underneath it). Default is
`min(8, hardware_concurrency())`; `AURALITE_SCAN_WORKERS=1` restores the old
sequential behaviour for a measurement, and any value from 1 to 64 is honoured.

The census had to survive the split: an item a worker never got to is marked
locked, never left at zero size, because a zero-size file counts as `empty` and
`empty` is what produced the original "0% fragmented" report.

## Running it

```bash
make perf                      # one pass, all budgets (blocking, in CI)
make perf-baseline             # rewrite the baseline (deliberate, 5 reps)
THRESHOLD=20 make perf-gate    # report regressions; informational here
ENFORCE=1 THRESHOLD=20 make perf-gate   # the plan's blocking gate, on RH-2
```
