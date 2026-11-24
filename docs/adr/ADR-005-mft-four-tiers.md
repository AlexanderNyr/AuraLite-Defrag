# ADR-005: Four-Tier Online `$MFT` Optimization Strategy

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** Implement explicit four-tier MFT handling:
  - Tier 1 (`Direct`): Online `FSCTL_MOVE_FILE` on `$MFT` only when capability probe confirms support and user explicitly confirms (`--confirm-mft`).
  - Tier 2 (`Indirect`): Evacuate regular user files adjacent to `$MFT` and inside the MFT Zone so future `$MFT` growth stays contiguous.
  - Tier 3 (`ReportOnly`, default): Report `$MFT` extent count, zone occupancy, and `fsutil behavior query mftzone` guidance with 0 metadata moves.
  - Tier 4 (`Deferred`): Post-v1 `BootTimeMode` native executable seam (`IBootTimeExecution`).
## Context

The MFT is the file that lists every file. Moving it while the volume is
mounted is not a documented operation, and the project's own rule is that
unverified filesystem assumptions are not shipped (R-022).

## Consequences

- **Tier 3 is the default** and performs zero metadata moves. A product that
  defaults to the safest tier cannot be broken by a user clicking the biggest
  button.
- Tier 1 is gated three ways: the capability probe on the backend (which the
  shipped backend does not advertise), the `--confirm-mft` second confirmation,
  and the tier selection itself. In practice tier 1 is **unreachable in v1**,
  which is the intended outcome of an unverified assumption.
- Tier 2 (zone evacuation) delivers most of the practical benefit — reserving
  contiguous space for MFT growth — without touching `$MFT` at all.
- Tier 4 is a documented hook, not code, so v1 does not foreclose boot-time work.

## Verification

Tier 3's report path is tested; tiers 1 and 2 are implemented and exercised
against the emulator. Acceptance on 3 OS builds (E2E-06) requires Windows and is
open. The decision itself — "do not default to an unverified operation" — needs
no verification.
