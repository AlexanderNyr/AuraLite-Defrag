# Architecture Decision Records

An ADR records a decision that was expensive to make and would be expensive to
reverse: the context, the choice, and what it costs. If you are wondering "why is
it like this?", this is the index.

Status values: **Accepted** (in force), **Accepted (partial)** (decision stands,
implementation incomplete), **Superseded**.

| ADR | Title | Status | Covers (P9-T07 criterion 2) |
|---|---|---|---|
| [001](ADR-001-layering-and-interfaces.md) | Layering and decoupled interfaces (L0–L6) | Accepted | build, architecture |
| [002](ADR-002-win32-d2d-ui.md) | Win32 + Direct2D/WIC UI, no third-party UI toolkit | Accepted | **renderer** |
| [003](ADR-003-binary-journal.md) | Two-slot CRC32C binary journal, verify-before-retry | Accepted | **journal** |
| [004](ADR-004-ssd-fail-closed.md) | Fail-closed SSD/NVMe refusal, no override | Accepted | **media policy** |
| [005](ADR-005-mft-four-tiers.md) | Four-tier MFT strategy with report-only default | Accepted | **MFT tier** |
| [006](ADR-006-i18n-and-cleanroom.md) | String-table i18n and clean-room wording rules | Accepted | **i18n**, clean-room |
| [007](ADR-007-shell-context-menu.md) | Shell context menu via IExplorerCommand, no shell extension DLL | Accepted | shell integration |
| [008](ADR-008-strict-json-security.md) | Strict JSON parser, 1 MiB / 8 levels, untrusted input | Accepted | input hardening |
| [009](ADR-009-licence-policy.md) | Apache-2.0, zero copyleft, zero runtime deps | Accepted | **licence** |
| [010](ADR-010-scheduler-integration.md) | Task Scheduler 2.0 COM, not a service | Accepted (partial) | **scheduler** |
| [011](ADR-011-testing-and-oracle.md) | Emulator as oracle, differential testing, golden corpus | Accepted | testing strategy |
| [012](ADR-012-documentation-and-cleanroom.md) | Documentation as a build artifact | Accepted | **clean-room**, docs |

## Coverage check

The P9-T07 acceptance criterion asks ADRs to cover: renderer, build, journal,
MFT tier, media policy, privilege, licence, clean-room, scheduler, i18n.

| Topic | ADR |
|---|---|
| renderer | 002 |
| build | 001, 009 |
| journal | 003 |
| MFT tier | 005 |
| media policy | 004 |
| **privilege** | 004 (fail-closed gating), 010 (per-task elevation) |
| licence | 009 |
| clean-room | 006, 012 |
| scheduler | 010 |
| i18n | 006 |

All ten are covered.

## Decision log in one paragraph

The project is a seven-layer system where dependencies only point upward, so the
engine never knows what filesystem it is talking to (001). The UI is Win32 and
Direct2D directly, because a UI toolkit would be the only third-party runtime
dependency in the product (002). Every cluster move is journaled with a
two-slot CRC32C header and a verify-before-retry loop, because losing a move is
worse than repeating one (003). Solid-state media is refused outright and
unidentifiable media is treated as solid-state, because the failure mode of
guessing wrong is write endurance, not speed (004). The MFT is handled in four
tiers and defaults to *report only*, because online `$MFT` movement is not a
documented operation and must not be assumed to work (005). Strings live in a
table and every identifier is audited, which also keeps the wording
clean-room (006, 012). The context menu is an `IExplorerCommand` with no DLL, to
avoid an in-process shell extension (007). All input is untrusted: strict JSON,
size and depth limits, fuzzed parsers (008). The licence is Apache-2.0 with zero
runtime dependencies and a mechanical audit of every include (009). Scheduling
goes through Task Scheduler 2.0 so the user can see and delete it (010). Testing
rests on a deterministic emulator, a differential oracle and a frozen golden
corpus (011).
