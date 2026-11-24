# AuraLite-Defrag Developer Architecture

Covers **R-090** (layered architecture, text diagram, interface signatures) and
**R-091** (threading, error handling, i18n, logging, configuration/persistence).

## Layer DAG (`L0` → `L6`)

Dependencies point **upward only**. `tools/check_layers.py` fails the build if any
file includes a module from a higher layer, and additionally forbids `engine/`
from including `backend/` or `platform/`, and `reporting/` from including
`backend/`, `engine/` or `platform/`.

```
L6  presentation      src/ui  src/cli  src/scheduler  src/shell  src/app
                                                                    |
L5  reporting         src/reporting (ClusterAggregator, StatisticsEngine,   |
                      CsvExporter/CsvReader, ChartDataModel)                |
    differential      src/differential (DifferentialOracle, golden traces)  |
                                                                     |
L3/L4  engine         src/engine (OnlineMode, DefragPlanner, BinaryJournal, |
                      PolicyGate, OperationController)               |
                                                                     |
L2  backends          src/backend/ntfs  src/backend/emulator  src/backend/usn
                                                                     |
L1  platform          src/platform (Win32 RAII, elevation, volume enum, paths)
                                                                     |
L0  core              src/core  src/interfaces  (types, Result, errors,
                      JSON, config, ExclusionEngine, string table, contracts)
```

```
        +-------------------------------+
   L6   |  ui    cli    scheduler  shell|
        +-------------------------------+
                    |
        +-------------------------------+
   L5   | reporting        differential |
        +-------------------------------+
                    |
        +-------------------------------+
  L3/L4 | engine: planner, journal,     |
        |         policy gate, modes    |
        +-------------------------------+
                    |  (interfaces only)
        +-------------------------------+
   L2   | ntfs | emulator | usn parser  |
        +-------------------------------+
                    |
        +-------------------------------+
   L1   | platform (Win32 / POSIX)      |
        +-------------------------------+
                    |
        +-------------------------------+
   L0   | core + interfaces             |
        +-------------------------------+
```

### Module map

| Layer | Path | Contents |
|---|---|---|
| L0 | `src/core/`, `src/interfaces/` | types, `Result<T, ErrorInfo>`, error taxonomy, JSONL logger, strict JSON parser, config, `ExclusionEngine`, string table, abstract interfaces |
| L1 | `src/platform/` | Win32 RAII handles, elevation checks, volume enumeration, UTF-8/UTF-16 and `\\?\` path helpers |
| L2 | `src/backend/usn/` | `ParseUsnBuffer`, `Utf16LeToUtf8` — the untrusted-buffer boundary, shared with the Linux fuzz harness (`FUZZING.md`) |
| L2 | `src/backend/ntfs/`, `src/backend/emulator/` | `RealNtfsBackend`, `RealMediaDetector`, deterministic `EmulatorBackend`, `ScenarioLoader` |
| L3/L4 | `src/engine/` | `OnlineMode`, `DefragPlanner`, `BinaryJournal`, `PolicyGate`, `OperationController` (`CRASH_RECOVERY.md`) |
| L5 | `src/reporting/` | `ClusterAggregator`, `StatisticsEngine`, `CsvExporter`/`CsvReader`, `ChartDataModel` (`REPORTING.md`) |
| L5 | `src/differential/` | `DifferentialOracle`, golden traces (`DIFFERENTIAL_TESTING.md`) |
| L6 | `src/ui/`, `src/cli/`, `src/scheduler/`, `src/shell/`, `src/app/` | Win32/D2D/WIC GUI, CLI, Task Scheduler 2.0 COM, shell verb |

## Key interface signatures (R-090)

Short pseudo-signatures; the authoritative declarations are in `src/interfaces/`.

```cpp
// L0 — storage access. The only way the engine touches a volume.
struct IVolumeProvider {
    Result<VolumeGeometry> Geometry() const;
    Result<BitmapSnapshot> Bitmap(Lcn start, ClusterCount count) const;
    Result<std::vector<Extent>> Extents(FileId id) const;
    Result<void> MoveFile(FileId id, Vcn vcn, Lcn source, Lcn target, ClusterCount n);
};

struct IFileSystemAdapter {
    Result<std::vector<FileEntry>> Enumerate(std::uint64_t maxFiles);
    Result<std::uint64_t> ComputeWindowChecksum(FileId id);
};

// L0 — planning: pure, device-independent, no I/O.
struct IDefragPlanner {
    Result<DefragPlan> Plan(const AnalysisSnapshot&, const PlanOptions&,
                            const PolicyGate&);
    Result<DefragPlan> Replan(const AnalysisSnapshot&, const DefragPlan&,
                              const Checkpoint&, const PolicyGate&);
};

// L0 — execution: applies a plan, journaling each step.
struct IExecutionMode {
    Result<ExecutionOutcome> ExecuteWithOutcome(const DefragPlan&, IProgressSink&,
                                                IJournal&, CancelToken&,
                                                const std::vector<JournalRecord>* resume);
};

// L0 — durability: append-only, CRC-protected, two header slots.
struct IJournal {
    Result<void> OpenOrCreate(std::uint64_t opId, std::uint64_t volumeSerial,
                              std::uint64_t planHash);
    Result<void> Append(const JournalRecord&, bool flushNow);
    Result<Checkpoint> Commit();
    Result<std::vector<JournalRecord>> Recover();
    Result<void> Seal();
};

// L0 — progress: called from a worker thread, must not block.
struct IProgressSink {
    void OnSnapshot(const ProgressSnapshot&);   // coalesced to 10 Hz upstream
    void OnEvent(const LogEvent&);
};
```

## Threading model (R-091)

| Thread | Owns | Rules |
|---|---|---|
| UI thread (L6) | window, D2D device, chart consumption | Never blocks on I/O. Pops immutable `ChartFrame`s from `ChartDataModel`; the queue is bounded and coalesces at 10 Hz. |
| Worker (L4) | `OperationController::RunOperation`, the whole move loop | The only thread that calls `MoveFile`. Publishes progress through `IProgressSink`; owns the journal handle for the duration of the run. |
| Scheduler COM (L6) | Task registration | `CoInitializeEx` on its own thread; the defrag work it triggers runs on a worker, not on the COM thread. |
| Test harness | crash-campaign child | Separate process, killed with `_exit()` — see `CRASH_RECOVERY.md`. |

- Cancellation is cooperative via `CancelToken` (atomic flag), checked between
  steps. A cancelled run stops at a step boundary, leaving the journal
  consistent.
- The UI never reads engine state directly; it reads snapshots copied under a
  mutex, which is why the chart model exists.
- No global mutable state in L0–L5. `PolicyGate` and `BinaryJournal` are owned by
  the operation, not by a singleton.

## Error handling (R-091)

- Everything fallible returns `Result<T, ErrorInfo>`. No exceptions escape the
  engine; the codebase does not throw for control flow.
- `ErrorInfo` carries an `ErrorCode`, volume id, file id, Win32 error and a
  diagnostic string. `ErrorCode` maps to a user-facing `IDS_*` message key, so
  the same failure reads correctly in any language.
- Errors are classified: **policy refusal** (SSD, pre-flight, elevation) is a
  normal outcome reported to the user, not an exception; **verification
  failure** stops the operation and leaves the journal resumable; **I/O error**
  is retried per the journal's verify-before-retry rule.
- CLI exit codes are derived from `ErrorCode` by `MapErrorToCliExitCode`; the
  table in `../user/CLI_REFERENCE.md` is transcribed from that function.

## Internationalisation (R-091)

- Every user-visible string is an `IDS_*` identifier in `res/strings/ids.h`,
  resolved through `StringTable::Lookup`. No string literal reaches the UI.
- `tools/check_strings.py` verifies that every identifier referenced in code
  exists in the table, and that wording does not match known third-party product
  phrasing (clean-room rule, ADR-006).
- Locales live in `res/strings/<locale>.rc`; adding one means mirroring the
  identifier set (see `DEVELOPER_GUIDE.md`).
- Numbers and paths: paths are UTF-8 internally and converted to UTF-16 only at
  the Win32 boundary; non-ASCII is preserved byte-exactly in CSV export.

## Logging (R-091)

- JSONL, one object per line, rotated by size with a configurable retention
  window (`logRetentionDays`, `logMaxFileBytes`, `logMaxRotations`).
- Levels: debug, info, warn, error. `--log-level` and the config both set it.
- `verboseLogPaths` is **off by default**: full paths are user data and should
  not be in logs unless the user asks for them.
- Log writes go through the same sink interface as progress events, so a log
  line and a progress snapshot cannot interleave into a corrupt file.

## Configuration and persistence (R-091)

- Versioned JSON with `"schemaVersion"`, strict unknown-key rejection, range
  checks and enum validation. Parsed by the in-tree strict parser (ADR-008).
- Limits: 1 MiB, 8 nesting levels — enforced before parsing, verified by the
  fuzzer.
- Atomic replace: write a temporary file, then rename over the target. A crash
  mid-save leaves the previous config intact.
- Migration functions `vN → vN+1`; `schemaVersion` 1 is the current baseline.
- Portable mode is detected by marker files next to the executable.

## Verification

```
make test charts reporting exclusions usn fuzz-quick crash-campaign
make differential golden-verify
make audit audit-negative
python3 tools/check_layers.py    # layering, part of make audit
```
