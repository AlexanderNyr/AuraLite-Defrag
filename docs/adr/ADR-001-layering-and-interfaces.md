# ADR-001: Layering and Decoupled Interfaces (L0–L6)

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** AuraLite-Defrag is structured into 7 strictly ordered layers (`L0` Core/Interfaces, `L1` Win32 Platform, `L2` Storage Backends, `L3` Execution Modes, `L4` Operation Control & Journal, `L5` Reporting & Aggregation, `L6` Presentation: Win32 UI, CLI, Scheduler, Shell).
- **Invariants:** Dependencies point strictly upward or to `L0` interfaces. `engine` (`L3/L4`) and `reporting` (`L5`) never include `backend/` (`L2`) or `platform/` (`L1`), enabling portable Linux compilation, ASan/UBSan fuzzing, and deterministic simulation parity (`RealNtfsBackend` vs `EmulatorBackend`).
- **Verification:** Enforced in CI via `tools/check_layers.py`.
