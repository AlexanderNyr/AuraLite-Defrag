# AuraLite-Defrag — Developer Guide

How to build, test and extend the project. Acceptance criterion (4) for P9-T07 is
that a clean clone reproduces the build on a reference host; every command below
was run on a clean clone of this repository before being written down.

---

## 1. Repository layout

```
src/            production code, layered L0..L6 (see ARCHITECTURE.md)
tests/unit/     unit and scenario suites, plus the reporting/exclusions/USN suites
tests/differential/  synthetic differential tests for the oracle
tests/golden/   20 committed golden traces (byte-stable corpus)
tests/scenarios/ 20 JSON scenarios SC-01..SC-20 + schema
tests/crash/    crash/power-loss campaign (child + runner)
tools/          audit scripts, benchmarks, fuzzers, golden generator
docs/           user, developer, ADR, clean-room documentation
res/            string tables and registered assets
ci/             GitHub Actions workflow
build_linux/    build output (not tracked)
```

---

## 2. Building on Linux (CI and development)

Only `make` and a C++20 compiler are required. There are no third-party
libraries — the include graph is verified to contain only standard and OS
headers by `make audit`.

```bash
git clone <repo>
cd AuraLite-Defrag
make test          # unit + scenario suites, ASan/UBSan
```

The Linux build compiles L0–L5 plus the emulator, engine, reporting layer and
the CLI. It deliberately does **not** compile the Win32 UI or the real NTFS
backend; those are guarded by `#ifdef _WIN32`.

### Every target

| Command | What it runs | Time |
|---|---|---|
| `make cli` | CLI binary | fast |
| `make test` | 11 unit/scenario tests under ASan/UBSan | ~2 min |
| `make charts` | 12 chart-data-model tests (P7-T01) | ~1 min |
| `make reporting` | 16 reporting tests (P7-T05/T06/T08) | ~2 min |
| `make exclusions` | 12 exclusion tests, 1000-pattern corpus (P8-T02) | ~2 min |
| `make usn` | 14 USN parser tests (P9-T01) | ~1 min |
| `make fuzz-quick` | 30k parser executions | ~1 s |
| `make fuzz` | **6,000,000** executions under ASan/UBSan | ~3 min |
| `make crash-campaign` | 200 abrupt-termination trials | ~1 min |
| `make differential` | 13 oracle tests | ~1 min |
| `make golden-verify` | 20 golden traces vs current behaviour | ~1 min |
| `make bench-reported` | release-build timings vs plan budgets | ~30 s |
| `make audit` | layering, strings, licences | ~1 s |
| `make audit-negative` | proves the licence audit is not vacuous | ~2 s |
| `make sbom` | regenerates `sbom.json` and fails on drift | ~1 s |

### Building with CMake

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

> The CMake build is configured but **has not been exercised in this
> environment** (no CMake on the reference Linux host during development). The
> `Makefile` is the authoritative build for verification. Treat a CMake
> regression as a bug report, not as a surprise.

### Building on Windows

`cmake -B build -G "Visual Studio 17 2022" -A x64` then build the `AuraLiteGui`
and `auralite-cli` targets. Requires the Windows 10/11 SDK for D2D, DWrite, WIC
and Task Scheduler headers. This path is **not** verified in the Linux-only
environment where the engine work was done — see "What is verified where".

---

## 3. The emulator is the test oracle

`EmulatorBackend` implements the same `IVolumeProvider` / `IFileSystemAdapter`
interfaces as the real NTFS backend, over an in-memory cluster map. It is
deterministic: the same scenario and seed always produce the same geometry, the
same file set and the same move sequence.

That determinism is what makes most of this repository testable without a
Windows machine or a scratch volume:

* scenario tests assert exact outcomes;
* the differential oracle compares emulator runs against each other;
* the golden corpus freezes 20 runs into committed JSON traces;
* the crash campaign kills a real process on top of it.

### Writing a scenario

See `docs/dev/SCENARIO_FORMAT.md` and `tests/scenarios/scenario.schema.json`.
Key points: `<= 1 MiB`, `<= 8` nesting levels, allowlisted keys only, unknown
keys rejected. Generators `G-1`..`G-6` cover general fragmentation, near-full
packing, MFT zone occupancy, resident-file storms, large files and special NTFS
flags. Fault injection (`IoErrorOnMove`, `LockedFile`, `FileVanished`,
`InterruptionMidMove`, `PartialMove`, …) lets a scenario reproduce a failure
deterministically.

---

## 4. Verifying a change

Before submitting:

```bash
make test charts reporting exclusions usn differential golden-verify
make audit audit-negative
make fuzz-quick
```

If you changed the planner or the engine, also run `make golden-verify`: it will
fail if behaviour moved. If the move is intentional, regenerate with
`make golden` and review the trace diff — the corpus is the record of what the
engine promises.

If you changed anything under `res/`, run `make sbom`; a stale SBOM fails.

### Adding a language

1. Add `res/strings/<locale>.rc` mirroring `en-US.rc`.
2. Add the same IDS_* identifiers — `tools/check_strings.py` verifies every
   identifier referenced from code exists in the table.
3. Add the locale to the string-table loader.
4. Run `make audit`; the clean-room scan will reject any string that matches a
   known third-party product's wording.

### Adding a filesystem backend

Implement `IVolumeProvider` and `IFileSystemAdapter` (see
`src/interfaces/`), then register it. The layering audit will reject an include
of `backend/` from `engine/`, so all filesystem knowledge stays behind the
interfaces. A new backend also needs a media detector: `PolicyGate` fails closed
on `Unknown` media, and an SSD-capable backend must say so explicitly.

---

## 5. What is verified where

Being explicit about this matters more than the claim "it builds".

| Area | Verified on Linux | Needs Windows |
|---|---|---|
| L0 core, L4 engine, L5 reporting | ✅ full | — |
| Emulator backend + 20 scenarios | ✅ full | — |
| Differential oracle + golden corpus | ✅ full | — |
| USN record parsing | ✅ unit + 1M fuzz inputs | — (real volume) |
| CSV export and round-trip | ✅ full | — |
| Exclusions engine | ✅ 1000-pattern corpus | — |
| Crash / power-loss recovery | ✅ 200 real process kills | `chkdsk`, `FlushFileBuffers` |
| Licence / provenance audit | ✅ full | installer artefacts |
| **Real NTFS backend** | ❌ compiles only under `_WIN32` | ✅ required |
| **Win32/D2D GUI** | ❌ | ✅ required |
| **Installer + signing** | ❌ | ✅ required |
| **VHDX integration stand** | ❌ | ✅ required |

The real backend has never been compiled by MSVC in this environment and has
never touched a real volume. That is the single largest remaining risk and it is
tracked as P3-T13 / IT-07.

---

## 6. Debugging

* Set `--log-level debug`; logs are JSONL under the configured log directory.
* `verboseLogPaths` in the config controls whether full paths appear in logs
  (off by default, because paths are user data).
* The journal is a binary file: two 512-byte header slots, then 96-byte records,
  each with a CRC32C. `docs/dev/CRASH_RECOVERY.md` describes the layout.
* To reproduce a user report, ask for the `--dry-run` output plus the journal;
  `--sim` with the same scenario reproduces the engine path exactly.

---

## 7. Further reading

| Topic | Document |
|---|---|
| layers, interfaces, threading | [ARCHITECTURE.md](ARCHITECTURE.md) |
| why decisions were made | [../adr/README.md](../adr/README.md) |
| differential testing | [DIFFERENTIAL_TESTING.md](DIFFERENTIAL_TESTING.md) |
| crash and power-loss behaviour | [CRASH_RECOVERY.md](CRASH_RECOVERY.md) |
| fuzzing | [FUZZING.md](FUZZING.md) |
| exclusions | [EXCLUSIONS.md](EXCLUSIONS.md) |
| reporting and CSV | [REPORTING.md](REPORTING.md) |
| licence policy | [LICENCE_AUDIT.md](LICENCE_AUDIT.md) |
| scenario format | [SCENARIO_FORMAT.md](SCENARIO_FORMAT.md) |
