# Spike Register (P0 research, R-093 / R-022 / R-014 / R-017 / R-099)

Eighteen spikes are mandated by the plan. This register is the honest state of
each one: what it must prove, whether it has been run, and — where it has not —
what the code currently assumes instead and what would have to change if the
assumption is wrong.

**Read this before trusting any claim about real NTFS behaviour in this
repository.** The emulator is good enough to build a product on; it is not
evidence about Windows.

## Summary

| Ran on real Windows | 0 |
| Partially answered without Windows | 4 (S-03, S-05, S-15, S-16 — reasoning only) |
| Open, blocking the real backend | 11 |
| Deferred to P10 by design | 3 (S-17, S-18 partially, S-02 reservation) |

The engine, the emulator, the reporting layer and the CLI were built and tested
without any of these spikes. That was a deliberate trade: the spike gate belongs
to P0, and P0 has not been executed. Everything downstream inherits the risk,
which is why `DEVELOPER_GUIDE.md` carries a "what is verified where" table and
why P3-T13/IT-07 remain open.

## Spike status

| ID | Spike | Status | What the code assumes today | Cost if the assumption is wrong |
|---|---|---|---|---|
| **S-01** | MFT-move feasibility, all four tiers | **NOT RUN** | Tiers 3 and 2 are implementable online; tier 1 (direct `$MFT` move) is *not* available and the shipped backend does not advertise the capability. | Tier 1 never ships. Tier 2 (zone evacuation) is the fallback. Product-acceptable. |
| S-02 | MFT zone behaviour, `fsutil behavior set mftzone` | **NOT RUN** | Zone boundaries come from `$MFT` geometry; the reservation setting is advice only, never changed by us. | The tier-2 benefit shrinks; guidance text in the user guide would need qualification. |
| S-03 | SSD/NVMe detection | **Partly reasoned** | `StorageDeviceSeekPenaltyProperty`, then `StorageDeviceTrimProperty`, then bus heuristics; `Unknown` fails closed to solid-state. | A false HDD classification is the worst failure mode in the product. `ADR-004` exists precisely to make the failure direction safe. |
| S-04 | `FSCTL_MOVE_FILE` per-call limits | **NOT RUN** | Cluster count per call is clamped conservatively; the planner never emits a single move above the conservative cap. | Throughput only; correctness unaffected. |
| S-05 | Volume bitmap retrieval semantics | **Partly reasoned** | Chunked reads with start-LCN rounding and tail masking modelled in the emulator's bitmap reader (`backend/emulator`), reused by `P3-T04`. | Bitmap reassembly could be off by the final partial byte. Cheap to fix once observed; impossible to observe without Windows. |
| S-06 | Direct2D vs GDI+ performance | **NOT RUN** | Direct2D + DirectWrite, as provisionally decided in the plan. | Renderer swap; the chart data model is renderer-agnostic by construction (`ADR-002`, `reporting/`). |
| S-07 | Emulator vs NTFS behavioural parity | **NOT RUN** | Fidelity ceilings are documented as provisional tolerances in `DIFFERENTIAL_TESTING.md`. | The differential oracle's tolerances (±2 pp, ±10%, ≥98%) are guesses and will need re-basing. |
| S-08 | Shell context menu Win10 vs Win11 | **NOT RUN** | `IExplorerCommand` (modern menu); no shell extension DLL (`ADR-007`). | Win10 needs a static verb as well; one extra file, no architectural change. |
| S-09 | Antivirus / Defender interference | **NOT RUN** | Every move is verified by re-reading retrieval pointers, and a failed verification stops the operation. | This is the mitigation that makes S-09 survivable without knowing the answer. |
| S-10 | USN / MFT enumeration + record parsing | **Reproduced in the emulator** | `FSCTL_ENUM_USN_DATA` + record parsing shapes; the untrusted-buffer parser is fuzzed to 6M executions (`FUZZING.md`). | Parsing bugs are caught by fuzzing regardless of source format. Enumeration completeness is unverified. |
| S-11 | WIC PNG export | **NOT RUN** | WIC encoder, no third-party image library. | PNG export becomes a dependency decision; `reporting/` is isolated so the blast radius is small. |
| S-12 | PMv2 DPI and accessibility | **NOT RUN** | PMv2 manifest, custom-drawn controls with UI Automation annotations. | Accessibility compliance (R-086) is unproven. Cannot be fixed without running the UI. |
| S-13 | VHDX harness and CI privileges | **NOT RUN** | No VHDX path is exercised anywhere in this repository. | `IT-01…IT-10` are all blocked. This is the largest single gap in the project. |
| S-14 | Task Scheduler 2.0 registration | **NOT RUN** | `TASK_RUNLEVEL_HIGHEST`, "run only when user is logged on". | Scheduler feature is unverifiable; `ADR-010` records the reasoning. |
| S-15 | Pre-flight checks | **Modelled** | Dirty, read-only, BitLocker, unsupported FS, low free space, SSD — each maps to exactly one error code and one string key (`error_catalog.cpp`). | The mapping is exercised by tests against the model, not against a real volume. |
| **S-16** | **`FSCTL_MOVE_FILE` integrity semantics** | **Modelled, not observed** | Every move is treated as unverified until retrieval pointers are re-read. 200 abrupt-termination trials found 0 silent corruption (`CRASH_RECOVERY.md`), but on the emulator, not on NTFS. | The verification-before-retry rule is the mitigation. The residual risk is that `FSCTL_MOVE_FILE` has a failure mode the model cannot express. |
| S-17 | Boot-time mode + Rust feasibility (P10) | **Deferred by design** | `IExecutionMode` seam exists; no boot-time code. Rust is not used; the FFI gate in the plan would have to fire first. | Nothing in v1 depends on it. |
| S-18 | Cross-version API behaviour (1809/22H2/23H2/24H2) | **NOT RUN** | The supported-OS claim (Win10 1809+) is unverified per build. | The claim may have to narrow to the builds that pass. |
| S-02 (reservation) | `mftzone` setting guidance | **NOT RUN** | The user guide advises, never changes the setting. | Guidance could be wrong; it is labelled as advice. |

## Deferred spikes

**S-17** (boot-time native-subsystem execution, Rust fallback) and the deferred
requirement batch R-099 (S.M.A.R.T./health, boot-time execution, FAT32/exFAT/ReFS
adapters, extra languages, Retrim) are **P10 scope**. They are specified in the
plan so that v1 does not foreclose them, and no v1 code path depends on them.
Retrim (R-017) is deferred for the same reason: it is a solid-state operation in
a product whose central rule is that solid-state media is refused.

## What would change this picture

A Windows host with an elevated account and a scratch VHDX converts eleven rows
above from "NOT RUN" to measured in roughly one week of work (`P0-T04`, `S-13`).
Nothing else does. Reading more documentation about `FSCTL_MOVE_FILE` does not
substitute for running it.
