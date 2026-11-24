# ADR-004: Fail-Closed SSD/NVMe Refusal Policy

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** `MediaClass::SolidState` and `MediaClass::Unknown` are both blocked from all write/defrag operations across every entry point (GUI, CLI, Task Scheduler, Shell Context Menu, Planner, and OnlineMode preflight). No `--force` or override switch exists in v1.
- **Detection Order:** `StorageDeviceSeekPenaltyProperty` (`IncursSeekPenalty == FALSE` -> `SolidState`), `DeviceTrimProperty` (`TrimEnabled == TRUE` -> `SolidState`), NVMe/SSD bus heuristics, with `Unknown` failing closed to `SolidState` semantics.
## Context

The single most damaging mistake this program can make is running a
cluster-moving operation on flash media. The write amplification is invisible at
the time and irreversible in effect. The plan demands refusal, and the plan is
right.

## Consequences

- No `--force`, no registry key, no "I know what I'm doing" checkbox. An
  override would be used, and the user would not be wrong to use it — which is
  exactly why it must not exist.
- `MediaClass::Unknown` is treated as solid-state. The cost of a false refusal is
  a user who cannot defragment a drive they could have; the cost of a false
  acceptance is a damaged device. The asymmetry decides it.
- The refusal is a *policy outcome*, not an error: exit code 12, one string key,
  analysis still offered.
- Virtual disks are a special case: a VHDX on an SSD reports what the bus says.
  Analysing a virtual disk is allowed; the risk is accepted and documented.

## Risk

Detection quality is entirely S-03, which has not run on real hardware. The
mitigations are detection layering and failing closed; neither substitutes for
measuring a classification matrix. This remains an open High risk in
`docs/dev/THREAT_MODEL.md` (T-03).
