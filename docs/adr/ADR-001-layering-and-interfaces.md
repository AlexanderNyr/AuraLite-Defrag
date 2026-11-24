# ADR-001: Layering and Decoupled Interfaces (L0–L6)

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** AuraLite-Defrag is structured into 7 strictly ordered layers (`L0` Core/Interfaces, `L1` Win32 Platform, `L2` Storage Backends, `L3` Execution Modes, `L4` Operation Control & Journal, `L5` Reporting & Aggregation, `L6` Presentation: Win32 UI, CLI, Scheduler, Shell).
- **Invariants:** Dependencies point strictly upward or to `L0` interfaces. `engine` (`L3/L4`) and `reporting` (`L5`) never include `backend/` (`L2`) or `platform/` (`L1`), enabling portable Linux compilation, ASan/UBSan fuzzing, and deterministic simulation parity (`RealNtfsBackend` vs `EmulatorBackend`).
- **Verification:** Enforced in CI via `tools/check_layers.py`.
## Context

The product must run against a real NTFS volume on Windows and against a
deterministic emulator on any platform, without duplicating the engine. A flat
codebase would have made the emulator a second implementation that drifts.

## Consequences

- The engine compiles and is tested on Linux with no Windows headers present,
  which is what made 6M-execution fuzzing and 200-trial crash campaigns possible.
- Every cross-layer call goes through an interface, so changing a backend cannot
  change engine behaviour by accident.
- Slight indirection cost: `IVolumeProvider` virtual calls on the move path. Not
  measurable against a syscall.
- `tools/check_layers.py` blocks upward includes. The rule caught a real
  violation when `ExecutionOutcome` was moved out of `engine/` into
  `interfaces/`.

## Alternatives rejected

| Option | Why rejected |
|---|---|
| One binary with `#ifdef _WIN32` throughout | Untestable on Linux; emulator would become a second code path. |
| Link-time seam (templates / CRTP) | Faster, but makes the interface contract invisible and testability worse. |
| Separate engine library per platform | Two implementations to keep in sync — the original problem. |
