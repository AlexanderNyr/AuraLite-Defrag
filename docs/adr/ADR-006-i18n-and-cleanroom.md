# ADR-006: Clean-Room String Table & Internationalization Architecture

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** All user-facing UI/CLI/error strings reside in `res/strings/ids.h`, `res/strings/en-US.rc`, and `src/core/strings.cpp` with `IDS_*` keys and locale-aware numeric/size/percent formatters (`FormatInteger`, `FormatByteSize`, `FormatPercent`).
- **Verification:** Enforced via `tools/check_strings.py`.
## Context

Two requirements meet here. R-046 forbids hardcoded user-visible strings; R-089
forbids wording that resembles a known third-party product. Both are only
enforceable mechanically if every string has an identifier and every identifier
lives in one place.

## Consequences

- `tools/check_strings.py` verifies that every identifier used in code exists in
  the string table, and scans source, resources, tests, packaging, documentation
  and root files for forbidden product names. It was extended during P9-T07 to
  cover `docs/adr` and the root markdown files — both were previously invisible
  to it, and a planted violation proved the gap.
- v1 ships English only (R-045), but the structure is ready: adding a locale
  means mirroring the identifier set, not editing code.
- Locale-aware formatting (`FormatInteger`, `FormatByteSize`, `FormatPercent`)
  is part of the table, not an afterthought, because a number formatted for the
  wrong locale is still a UI bug.
- Layouts must tolerate 2× English length (R-048) even though no longer locale
  ships in v1 — the cost of retrofitting that later is higher than the cost of
  designing for it now.
