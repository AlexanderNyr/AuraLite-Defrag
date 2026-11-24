# Release Notes

## Unreleased — v1.0.0-preview

Not shippable yet. This file exists so that the release checklist (R-096) has a
place to write into and so that the state of the product is written down by the
people building it rather than inferred from the commit log.

### What works today

| Area | State |
|---|---|
| Engine (planner, 5 strategies, journal, verification, skip policy) | Works, tested against the emulator. 20/20 scenario suite, 20 golden traces byte-stable. |
| Crash and power-loss recovery | 200/200 trials, worst recovery 38.2 ms, 0 silent corruption. Not tested against real power loss. |
| Reporting: histogram, Top-N, before/after, throughput | Works. 250k-row CSV round-trips with 0 lost rows. |
| CSV export | Works. UTF-8 BOM, RFC 4180 quoting, formula defanging. No HTML export, by design. |
| Exclusions | Works. 1,000-pattern corpus, 100% match correctness. |
| USN / MFT record parsing | Works. Fuzzed to 6,000,000 executions, 0 findings. |
| CLI | Works. 10 verbs, documented exit codes verified against the implementation. |
| Licence and provenance audit | Works and is proven non-vacuous by 10 negative tests. SBOM is generated and checked for drift. |
| Emulator and demo mode | Works. Deterministic, seeded, fault-injecting. |

### What does not work yet

| Area | Blocker |
|---|---|
| **Real NTFS volume operations** | The Windows backend compiles under `_WIN32` only and has never run. `P3-T13`. |
| **GUI** | Not built. `P6`. |
| **VHDX integration tests** | No elevated Windows runner. `S-13`. |
| **Installer, portable ZIP, code signing** | Not built. `P8-T07`, `P8-T08`, `P9-T06`. |
| **Task Scheduler integration** | Design recorded (`ADR-010`); COM code unverified. |
| **Shell context menu** | Design recorded (`ADR-007`); unverified. |
| **Performance at 244M clusters** | Not measured. `P9-T04`. |

### Known limitations

* Windows 10 1809+ and Windows 11, x64, NTFS only. No FAT32, exFAT or ReFS.
* Online defragmentation only. No boot-time execution.
* English UI only in v1. String table and the "add a language" process exist.
* Solid-state media is refused outright and there is no override.
* MFT tier 1 (direct `$MFT` move) is unreachable: the shipped backend does not
  advertise the capability.

### Release checklist (R-096)

- [ ] `make test charts reporting exclusions usn differential golden-verify` green
- [ ] `make crash-campaign` 200/200
- [ ] `make fuzz` 6M executions, 0 findings
- [ ] `make audit audit-negative sbom` green, SBOM in sync
- [ ] `python3 tools/docs_check.py` green; traceability matrix regenerated
- [ ] All 94 MUST requirements traced to a passing test (`P9-T10`)
- [ ] 0 Critical/High findings open in `THREAT_MODEL.md`
- [ ] VHDX CI: 20 consecutive clean runs
- [ ] Installer signed; portable ZIP; install/uninstall clean on 3 OS builds
- [ ] User guide, developer guide, ADRs published

**Nothing above the first six lines is currently satisfiable**, because they all
require a Windows machine.
