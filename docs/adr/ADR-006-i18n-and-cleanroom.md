# ADR-006: Clean-Room String Table & Internationalization Architecture

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** All user-facing UI/CLI/error strings reside in `res/strings/ids.h`, `res/strings/en-US.rc`, and `src/core/strings.cpp` with `IDS_*` keys and locale-aware numeric/size/percent formatters (`FormatInteger`, `FormatByteSize`, `FormatPercent`).
- **Verification:** Enforced via `tools/check_strings.py`.
