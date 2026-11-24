# ADR-009: Apache-2.0 Licence & Zero-Copyleft Dependency Policy

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** AuraLite-Defrag is licensed under Apache-2.0 with zero runtime third-party library dependencies and zero GPL/LGPL/AGPL build or QA tools (e.g., Cppcheck is strictly excluded).
- **Verification:** Enforced via `licenses.json` and `tools/check_licences.py`.
## Context

The product is Apache-2.0 and must remain so. The dangerous failure is not a
deliberate choice of a GPL library; it is a transitive or build-time dependency
arriving unnoticed.

## Consequences

- Zero runtime libraries. The include graph is verified to contain only standard
  and OS headers — measured, not asserted.
- Zero GPL/LGPL/AGPL tooling, including QA tools. Cppcheck is excluded for this
  reason; the static analysis in CI uses MSVC `/analyze` and clang-tidy instead.
- The audit is **proven non-vacuous**: a negative test planted a fabricated
  `cmark-gfm` dependency, and the previous audit passed it. That was the finding
  that turned a checklist into an audit. Ten negative tests now guard it
  (`make audit-negative`).
- A machine-readable SBOM is generated from the tree and checked for drift in CI,
  so "we have no dependencies" is a test result, not a claim.
- Assets are hashed and registered in both directions: no unregistered file on
  disk, no registered file missing.

## Residual risk

Build-time tools are enumerated but the list depends on the build host. The
Windows toolchain (MSVC, Windows SDK, MSIX packaging) is registered with
justification rather than licence review — their terms are not open-source and
cannot be audited the same way. This is stated in `THIRD-PARTY.md` rather than
glossed over.
