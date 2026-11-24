# Differential testing and golden traces

Implements **P2-T06** (R-063, R-064) and the comparison contract fixed by **S-07**.
See `src/differential/differential_oracle.hpp` for the normative API.

## Why this exists

The emulator is the primary test substrate: it is fast, deterministic and needs
no administrator rights. That only holds if the emulator tells the truth about
NTFS. The oracle is the mechanism that keeps it honest: the same engine code is
driven against two backends, both runs are normalised into the same shape, and
the shapes are compared with documented tolerances.

Without this, a green test suite proves that the emulator agrees with itself.

## What is compared (DT-01…DT-06)

| ID | Metric | Rule |
|---|---|---|
| DT-01 | file set | every file present on one side must exist on the other, matched by canonical path, with equal size; ≥99% agreement |
| DT-01 | extent map | LCN placement may differ (allocator policy); extent *count* must agree within 10% or a single fragment |
| DT-02 | fragmentation % | volume and per-file percentages within ±2 percentage points |
| DT-02 | MFT geometry | fragment count, record counts, zone bounds must match exactly |
| DT-03 | aggregation | `ClusterAggregator` output compared on the same snapshot (used by the VHDX harness) |
| DT-04 | move plan | ≥98% of normalised moves identical after ordering; differing target LCNs are classified, not failed |
| DT-06 | skip reasons | **100%** agreement — a file must be skipped for the same reason on both backends |
| DT-06 | error code | runs that end in error must end with the same code |

Aggregate gate: **≥98% overall agreement** and **zero unexplained divergences**.
`Tolerances` in the header is the single place these numbers live.

## Divergence classes

Every recorded divergence carries a class, so triage is mechanical:

- `unexplained` — fails the run. This is the only class that blocks CI.
- `explained:allocator-policy` — NTFS is free to place clusters differently; the
  engine must not depend on exact placement.
- `explained:capability` — the backends genuinely differ in capability (for
  example, Tier 1 MFT moves are not available everywhere).
- `explained:error-path` — the runs failed, but at the same place.

A report fails the gate if *any* `unexplained` divergence exists, even when the
percentage thresholds are met. Percentages hide single bad rows; this does not.

## Normalisation rules

- **Paths, not ids.** `FileId` is a dense index in the emulator and a 64-bit MFT
  reference on real NTFS. Files are matched by `MakePathKey`: case-folded,
  separator-normalised, with `\\?\` and `\\.\` prefixes stripped.
- **Sorting.** Files are sorted by key, extents by VCN, skips by (key, reason).
  Two runs of the same input therefore serialise byte-identically.
- **No host data.** Traces contain no absolut paths, timestamps, drive letters
  or machine identifiers, so they are safe to commit and to share.

## Golden traces

`tests/golden/<scenario>.trace.json` is the frozen observable behaviour of a
scenario. It is the versioned regression corpus required by R-064.

```bash
make golden          # regenerate the corpus (intentional changes only)
make golden-verify   # CI: current behaviour must match the corpus
```

Format: `traceFormatVersion` (currently `1`), `summary`, `mft`, `files`,
`moves`, `skips`, `moveCallCount`, `errorCodeValue`. Unknown versions are
rejected, so a format change is an explicit, reviewable edit.

**Rule.** A golden diff in a pull request must be accompanied by a sentence
explaining the behaviour change. Regenerating the corpus to make CI green is a
review blocker.

## Current state and what is still open

- **Done and enforced in CI:** the oracle, tolerances, divergence
  classification, markdown/JSON reports, golden corpus for all 20 scenarios,
  byte-stability check (regenerating twice produces identical files), and the
  end-to-end analyse→defrag→compare cycle on the emulator.
- **Open (V-25 / DT-05):** the second side of the comparison is currently the
  emulator driven twice, which proves determinism but not fidelity. Feeding a
  real NTFS run requires the VHDX harness (**P3-T13**), which needs an
  administrator Windows runner (**V-16**). The oracle API already accepts any
  `NormalizedRun`, so no oracle code changes when that lands — only a new
  producer of the right-hand run.
- **Open (S-07):** the tolerance values are the ones the plan fixed
  provisionally. They must be re-baselined once real NTFS data exists; the
  first run against a VHDX volume is what turns them from assumptions into
  measurements.
