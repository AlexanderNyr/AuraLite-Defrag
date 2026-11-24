# ADR-008: Strict JSON Configuration & Scenario Validation

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** Implement a self-contained strict recursive-descent JSON parser (`src/core/json.{hpp,cpp}`) rejecting payloads > 1 MiB, nesting depth > 8, duplicate object keys, trailing garbage, invalid UTF-8, unknown schema keys, and path traversal (`..`).
## Context

Config files and scenario files are the only external input this program accepts,
and both are shareable — a user pastes a scenario from a bug report. Treating
them as trusted would be a mistake.

## Consequences

- A self-contained recursive-descent parser, ~500 lines, no dependency. Rejecting
  is cheap; a general-purpose parser's permissive defaults are exactly the wrong
  starting point.
- Limits enforced *before* allocation: 1 MiB payload, 8 nesting levels. The
  plan's numbers, and they are checked by the fuzzer rather than asserted.
- Unknown schema keys are errors, not ignored. A typo in `"exlude"` must not
  silently produce "no exclusions" — silent acceptance of a misspelled safety
  setting is worse than a refused start.
- Duplicate keys, trailing garbage, invalid UTF-8 and `..` path traversal are
  rejected.
- **6,000,000 fuzz executions, 0 crashes, 0 hangs, 0 UB** (`docs/dev/FUZZING.md`).
  Note the honest finding: the *hypothesis* that first motivated the work — a
  length-field overflow in the USN parser — was tested and **disproved** at
  0.74 µs per parse. The fuzzing was still worth doing; it just did not find the
  bug it was looking for.

## Known limitation

The parser rejects rather than repairs. Malformed input gets a clear error and a
non-zero exit code instead of a best-effort interpretation.
