# ADR-005: Four-Tier Online `$MFT` Optimization Strategy

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** Implement explicit four-tier MFT handling:
  - Tier 1 (`Direct`): Online `FSCTL_MOVE_FILE` on `$MFT` only when capability probe confirms support and user explicitly confirms (`--confirm-mft`).
  - Tier 2 (`Indirect`): Evacuate regular user files adjacent to `$MFT` and inside the MFT Zone so future `$MFT` growth stays contiguous.
  - Tier 3 (`ReportOnly`, default): Report `$MFT` extent count, zone occupancy, and `fsutil behavior query mftzone` guidance with 0 metadata moves.
  - Tier 4 (`Deferred`): Post-v1 `BootTimeMode` native executable seam (`IBootTimeExecution`).
