# ADR-011: Emulator as Test Oracle, Differential and Golden Corpus

- **Status:** Accepted
- **Date:** 2026-11-24
- **Context:** The product moves clusters on a live NTFS volume. A test suite that
  needs a real volume cannot run in CI, cannot run on a developer's laptop
  without damage risk, and cannot be made deterministic. Yet the engine's whole
  value is behaving correctly against a real filesystem.

## Decision

Three testing mechanisms, layered:

1. **A deterministic in-memory emulator** (`EmulatorBackend`) implementing the
   same interfaces as the real NTFS backend, driven by declarative scenario
   files with seeded generation and injectable faults.
2. **A differential oracle** that compares two normalised runs
   (emulator vs emulator today, emulator vs real volume once a VHDX stand
   exists) and classifies every difference as *unexplained* (fails) or
   *explained* (allocator policy, capability, error path).
3. **A golden corpus** of 20 committed traces, byte-stable, verified on every CI
   run, so that a behaviour change is a visible diff rather than a surprise.

## Rationale

- The emulator makes the engine testable on any platform, under sanitizers, at
  speed. `make test` runs in about two minutes.
- The oracle exists because "the emulator passes" and "the product works" are
  different claims. Writing the comparison down, with explicit tolerances, keeps
  that gap visible instead of pretending it does not exist.
- The golden corpus catches the class of regression that unit tests miss: the
  behaviour nobody thought to assert.

## Consequences

- **The emulator is a maintained artifact, not a mock.** It models geometry,
  bitmap, MFT zone, extents, retrieval pointers, fault injection and invariant
  checking. It is roughly as much code as the real backend.
- **Tolerances are provisional.** The numbers in the oracle (±2 pp fragmentation,
  ±10% extents, ≥98% plan match) came from the plan, not from measurements
  against a real volume. They must be re-based after the first VHDX run, and
  `docs/dev/DIFFERENTIAL_TESTING.md` says so.
- **Golden traces contain no absolute paths, timestamps or host names**, so the
  corpus is portable and does not leak a developer's machine layout.
- A behaviour change is a two-step commit: regenerate the corpus, review the
  diff. Hiding it by loosening a test is not available.

## Known limitation

The right-hand side of the differential has never been a real volume.
`DT-05` is open until a VHDX stand exists (`P3-T13`, requires an elevated
Windows runner). Until then the oracle validates emulator-vs-emulator only,
which proves internal consistency, not fidelity to NTFS.
