# AuraLite-Defrag Developer Architecture

## Layer DAG (`L0` → `L6`)
- `L0` `src/core/`, `src/interfaces/`: Core types, `Result<T, ErrorInfo>`, error taxonomy, JSONL logger, strict JSON parser, config, string table, abstract interfaces.
- `L1` `src/platform/`: Win32 RAII handles, elevation checks, volume enumeration, UTF-8/UTF-16 & `\\?\` path helpers.
- `L2` `src/backend/ntfs/`, `src/backend/emulator/`: `RealNtfsBackend`, `RealMediaDetector`, deterministic `EmulatorBackend`, and `ScenarioLoader` (`G-1`..`G-6`).
- `L3/L4` `src/engine/`: `OnlineMode`, `DefragPlanner`, `BinaryJournal`, `PolicyGate`, `OperationController`.
- `L5` `src/reporting/`: `ClusterAggregator`, `StatisticsEngine`, `CsvExporter`.
- `L6` `src/ui/`, `src/cli/`, `src/scheduler/`, `src/shell/`, `src/app/`: Win32/D2D/WIC GUI, CLI, Task Scheduler 2.0 COM wrapper, Shell Context Menu.

Run `make test`, `make cli`, and the three Python audit scripts (`tools/check_{layers,strings,licences}.py`) before submitting changes.
