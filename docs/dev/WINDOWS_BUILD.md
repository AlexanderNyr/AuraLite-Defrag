# Building and verifying AuraLite-Defrag on Windows

This is the procedure for going from a clean Windows 10 1809+ / Windows 11 x64
machine to a running GUI and a green test run. Everything below was derived from
the tree as it stands; where a step could not be executed here (no Windows host,
no MSVC), it is marked **[unverified]** rather than presented as tested.

What *was* verified on Linux before shipping these instructions: the CMake
configuration and the full ctest suite now pass through CMake (`8/8`), which they
did not before — see §7 for what was broken and fixed.

---

## 0. What you need

| Requirement | Version | Notes |
|---|---|---|
| Windows | 10 1809+ or 11, x64 | `_WIN32_WINNT=0x0A00`, PerMonitorV2 DPI |
| Visual Studio | 2022, v143 toolset | "Desktop development with C++" workload |
| Windows SDK | 10.0.19041.0 or newer | Direct2D, DirectWrite, WIC, Task Scheduler |
| CMake | 3.25+ | The project sets `cmake_minimum_required(VERSION 3.25)` |
| Git | any | To fetch the tree |
| Python | 3.9+ | Only for the audit scripts; optional for building |

The `v143` toolset is what the `/permissive-`, `/sdl`, `/guard:cf` and
`/CETCOMPAT` flags are tested against. Older MSVC will reject `/CETCOMPAT`.

No third-party libraries are required. The project has no external
dependencies, which is deliberate (ADR-009).

---

## 1. Apply the patch

The patch is generated against commit `a1b15d6` ("Start") on `main`.

```powershell
git clone https://github.com/AlexanderNyr/AuraLite-Defrag.git
cd AuraLite-Defrag
git log --oneline -1                 # expect a1b15d6 Start
git apply --check ..\auralite-p9-t04.patch
git apply ..\auralite-p9-t04.patch
```

If `--check` fails, the base commit has moved. Either check out `a1b15d6`
explicitly (`git checkout a1b15d6`) or regenerate the patch from the tree you
want to build against; do not force-apply over a diverged tree.

The patch touches 106 files, adds ~90k lines and contains **no binary hunks**, so
`git apply` handles it without `--binary`.

---

## 2. Configure and build

```powershell
cmake -G "Visual Studio 17 2022" -A x64 -B build
cmake --build build --config Release --parallel
```

**Do not pass `-DCMAKE_BUILD_TYPE`.** A Visual Studio generator is multi-config:
CMake ignores the variable and warns "Manually-specified variables were not used
by the project". This is harmless, but it is not selecting anything — the
configuration comes from `--config Release` at build time.

**Configure does not compile.** `ctest` run before `cmake --build` reports
`\*\*\*Not Run` for every test, and `build\Release\` does not exist yet, so
`auralite-bench-perf.exe` and `AuraLiteDefrag.exe` are simply not there. Those
are not build failures; the build step was skipped.

What that gives you, on `WIN32`:

| Target | What it is |
|---|---|
| `AuraLiteDefrag.exe` | Win32 GUI, Direct2D/DirectWrite/WIC, `requireAdministrator` manifest |
| `auralite-cli.exe` | Console front end, `analyze` / `defrag` / `--sim` |
| `auralite_tests.exe` | Unit + 20-scenario suite |
| `auralite-{usn,exclusions,reporting,chart-model,differential}-tests` | Focused suites |
| `auralite-golden` | Golden-trace corpus writer/verifier |
| `auralite-bench-perf` | Performance budgets PT-01…PT-08 + PT-MEM |
| `auralite-bench-reporting` | Reporting timings |
| `auralite-fuzz` | Parser fuzzer |
| `auralite-map` | Scenario → SVG renderer |
| ~~`auralite-crash-child` / `auralite-crash-campaign`~~ | **Not built on Windows** — see §7 |

Everything is compiled with `/W4 /WX`, so **any warning is a build failure**.
That is intentional and it is the reason the Windows build is worth running: it
is the only place the Win32 code paths are compiled at all.

---

## 3. Build

```powershell
cmake --build build --config Release --parallel
```

Expect roughly 1–3 minutes on a modern 8-core machine. The plan's budget for
`build + unit` is 6 minutes on RH-2.

To build only the GUI:

```powershell
cmake --build build --config Release --target AuraLiteDefrag
```

---

## 4. Run the tests

```powershell
ctest --test-dir build -C Release --output-on-failure
```

**8 tests, all expected to pass.** They must be run from the repository root —
the suites load `tests/scenarios/*.json` relative to the working directory. The
`WORKING_DIRECTORY` is set in CMake so `ctest` handles this for you; running the
binaries by hand from `build\` will fail on scenario loading, which is a harness
detail and not a product defect.

Individual suites:

```powershell
.\build\Release\auralite_tests.exe                 # 11 tests, includes the 20 scenarios
.\build\Release\auralite-reporting-tests.exe       # 16 tests
.\build\Release\auralite-chart-model-tests.exe     # 12 tests
.\build\Release\auralite-exclusions-tests.exe      # 12 tests
.\build\Release\auralite-differential-tests.exe    # 13 tests
```

---

## 5. Run the performance harness

This is the part that needs Windows specifically, because two budgets cannot be
measured anywhere else: PT-MEM now uses `GetProcessMemoryInfo`, and the redraw
budget needs the real renderer.

```powershell
.\build\Release\auralite-bench-perf.exe
```

Budgets it checks, and what to expect:

| Bench | Budget | Note |
|---|---|---|
| PT-02a 4M aggregation | ≤ 50 ms | Should be ~2 ms; a slower number here means a real regression |
| PT-02b 64M aggregation | ≤ 400 ms | ~12 ms |
| PT-02c 1000 incremental extents | ≤ 2 ms | ~0.25 ms |
| PT-01 1000 hit tests @250k | ≤ 100 ms | ~0.02 ms |
| PT-04 244M analysis + stats | ≤ 90 s | ~150 ms |
| PT-06a/b histogram, Top-N | ≤ 50 / 20 ms | ~3 / 5 ms |
| PT-08a CSV 250k | — | ~80 ms |
| PT-07a chart push | ≤ 16 ms | ~0.03 ms |
| PT-07c pixel buffer 4K | ≤ 16 ms | ~0.21 ms (CPU half; upload is GPU) |
| **PT-MEM peak, 1M files** | **≤ 512 MB** | **~303 MB on Linux; compare on Windows** |
| PT-05 SC-01 defrag | — | Emulator only; not disk throughput |

`--reps 5` takes medians, `--save-baseline <file>` records them, `--baseline
<file> --threshold 20` compares against them. **On a dedicated machine, use
`--enforce 1`** to make a >20% regression a non-zero exit; that is the plan's
criterion 3 and it needs a host that measures twice identically (this sandbox
could not, see `PERFORMANCE.md`).

---

## 6. Run the GUI

```powershell
.\build\Release\AuraLiteDefrag.exe
```

It requests elevation via the manifest. Without admin rights it will fail to
open the volume — the NTFS backend needs `\\.\C:` handle access and
`FSCTL_GET_VOLUME_BITMAP`.

Three things to try, in order, because they exercise the riskiest paths:

1. **Demo mode** — menu → *Run demo*. Uses `EmulatorBackend` against SC-01, no
   volume access, no writes. Confirms the GUI, Direct2D map and charts work.
2. **Analyze only** on a real volume — read-only. Confirm the cluster map
   renders, hover the map to hit-test files, and check the file list populates.
3. **Quick defrag** on a **virtual disk you can afford to break** (see §8),
   never on your system volume first.

SSD and unknown media are refused on every entry point with no override in v1.
If the machine's system disk is an SSD — most are — that refusal is correct and
expected, and the only way to run a real defrag is a spinning virtual disk.

---

## 7. What was fixed to make this work

Checking the CMake path before writing these instructions found defects that
`make` on Linux never exercised. All are fixed in this patch:

| Defect | Effect before the fix |
|---|---|
| `crash_child.cpp` has `#error` under `_WIN32`, target declared unconditionally | **Windows build failed to compile, always** |
| `auralite-crash-campaign` had no include directories | Failed on Linux too, on `backend/usn/usn_parser.hpp` |
| `add_test` without `WORKING_DIRECTORY` | 4 of 8 ctest tests failed on correct code |
| `auralite-bench-perf`, `auralite-map` not in CMakeLists | The P9-T04 and P9-T07 tools were `make`-only |
| `tools/bench_perf.cpp` used `/proc/self/status` and `/tmp` | PT-MEM skipped on Windows |
| RC language never enabled, `app.rc` not compiled | GUI would link with **no manifest** — no elevation, no PerMonitorV2 DPI, no string table |
| `WIN32` target with only `wWinMain` | `LNK2019 unresolved external symbol WinMain` |
| `app.rc` included `../../res/...` | rc.exe resolves includes from its include path, not from the file's directory |

The crash campaign stays POSIX-only by design: it spawns children with
`fork()`, hands them commands through `mkfifo` channels and SIGKILLs them. A
Windows port would be a different tool with a different failure mode, and porting
500 lines of process plumbing that cannot be compiled here would be guessing. It
is excluded explicitly (`if(NOT WIN32)`) so the exclusion is visible in the build
system rather than a silent gap. The durability property it tests is still
covered on Windows by `TestJournalCrashRecovery200Trials` in `auralite_tests.exe`,
which is weaker (in-process) and is documented as such.

---

## 8. Testing against a real NTFS volume safely

Never point the first real defrag at your system volume.

**Option A — VHDX (recommended).** Disk Management → *Action* → *Create VHD* →
VHDX, dynamically expanding, 2–8 GB → initialize as MBR → format NTFS →
mount. Then:

```powershell
# find the drive letter, then analyze it read-only
.\build\Release\auralite-cli.exe analyze --volume X:
```

A VHDX on an SSD is reported as SSD media, so the SSD refusal fires and you get
nothing. To exercise the moving code against a real filesystem you need the
media to be seen as rotational: a virtual disk on a spinning physical disk, or a
machine with an HDD. This is a genuine limitation of v1's fail-closed SSD policy,
not a bug, and it is why `P9-T05` exists.

**Option B — a throwaway physical disk or a scratch partition.** Only if you can
lose the contents.

**Option C — the emulator.** `--sim tests/scenarios/SC-01.json` covers the
scheduling, journal and recovery logic without touching a disk. It is the right
tool for anything except media detection and real `FSCTL_*` behaviour.

---

## 9. What is expected to be green, and what is expected to be open

Green after this patch, on Linux (verified here) and expected on Windows:

* `make test` 11/11, `charts` 12/12, `reporting` 16/16, `exclusions` 12/12,
  `differential` 13/13, `golden-verify` 20/20
* `make audit` — layering, clean-room strings, licence/SBOM
* `make perf` 12/12 within budget
* `ctest` 8/8 through CMake

Still open, by design, and not a reason to think the build is broken:

| Item | Why |
|---|---|
| Map redraw *end-to-end* ≤ 16 ms @4M | Needs the GPU upload measured; CPU half is 0.21 ms |
| PNG export 1080p/4K ≤ 2 s | WIC path exists; budget unmeasured |
| Move throughput / ETA / verification overhead | Needs a real volume — `P9-T05` |
| Enumeration 250k files ≤ 45 s | Real volume |
| Real 244M-cluster analysis ≤ 90 s | Real volume; aggregation half measured |
| UI thread never blocked >16 ms over 30 min | Needs the GUI running |
| `chkdsk` reports 0 errors | Windows-only; the crash campaign SKIPs it explicitly |

If you get a failure, the fastest triage is `ctest --output-on-failure` for
correctness and `auralite-bench-perf.exe` for anything that smells like
slowness. The two do not overlap: the suites assert behaviour, the harness
asserts budgets.
