# ADR-004: Fail-Closed SSD/NVMe Refusal Policy

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** `MediaClass::SolidState` and `MediaClass::Unknown` are both blocked from all write/defrag operations across every entry point (GUI, CLI, Task Scheduler, Shell Context Menu, Planner, and OnlineMode preflight). No `--force` or override switch exists in v1.
- **Detection Order:** `StorageDeviceSeekPenaltyProperty` (`IncursSeekPenalty == FALSE` -> `SolidState`), `DeviceTrimProperty` (`TrimEnabled == TRUE` -> `SolidState`), NVMe/SSD bus heuristics, with `Unknown` failing closed to `SolidState` semantics.
