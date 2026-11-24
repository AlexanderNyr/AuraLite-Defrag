# ADR-012: Documentation as a Build Artifact

- **Status:** Accepted
- **Date:** 2026-11-24
- **Context:** Developer documentation drifts from code silently. In this project
  it already had: the CLI reference listed exit code 10 for "elevation required"
  while the implementation returns 13, and listed 30 for argument errors while the
  implementation returns 2. A script written against the document would have had
  error handling that never fires.

## Decision

1. **Every documented fact that can be checked by machine is checked by machine.**
   The licence audit verifies that `THIRD-PARTY.md` and `licenses.json` agree;
   the CLI reference's exit-code table is transcribed from
   `MapErrorToCliExitCode` and marked with the source of truth.
2. **Docs that describe behaviour must name the test that proves it.** A section
   claiming "recovery completes in milliseconds" cites the 200-trial campaign;
   a section claiming "zero third-party includes" names the audit that
   enumerates them.
3. **Wrong documentation is a defect, not a typo.** When a document and the code
   disagree, the disagreement is recorded in the document rather than quietly
   corrected, so the reader can judge whether anything depended on the old
   behaviour.
4. **Screenshots are required for user-facing features** (P9-T07 criterion 1) but
   must be captured from our own UI. No stock imagery, no third-party
   screenshots — this is also the clean-room rule for docs.

## Consequences

- Documentation lives under `docs/` with a fixed structure: `user/`, `dev/`,
  `adr/`, `cleanroom/`.
- The ADR index (`docs/adr/README.md`) is the entry point and must list every
  ADR; a missing entry is a review failure.
- Where a claim cannot be verified in the current environment — for example
  anything requiring Windows — the document says so explicitly instead of
  asserting it. "Verified where" tables exist for this reason.

## Still open

Screenshot capture requires a working Windows build of the GUI. Until then the
user guide describes features in text and the criterion-1 screenshot requirement
is met only for CLI output, which is reproduced as text blocks.
