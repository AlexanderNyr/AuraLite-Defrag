# Threat and Failure Model (R-098, R-084, R-097)

The thing being protected is **the user's data**. A defragmenter is a program
that rewrites a mounted filesystem on purpose; the failure modes that matter are
the ones that lose or corrupt bytes, not the ones that crash.

Scope: the Windows product. The Linux build cannot reach a real volume.

## Severity scale

| Level | Meaning |
|---|---|
| **Critical** | Silent data loss or corruption of user data. |
| **High** | Volume left in a state that requires repair, or operation on media it should have refused. |
| **Medium** | Operation aborts, but data is intact and the journal can resume. |
| **Low** | Information disclosure, annoyance, or denial of service limited to the app. |

## Failure and threat table

| ID | Threat / failure | Impact | Likelihood | Severity | Mitigation | Residual risk |
|---|---|---|---|---|---|---|
| T-01 | **Silent extent-map corruption** after a move | A file points at the wrong clusters — silent data loss | Low | **Critical** | Every move is re-verified by re-reading retrieval pointers; a mismatch stops the operation and reports exit 20 (`IExecutionMode`, `CRASH_RECOVERY.md`) | The verification compares what NTFS *reports*, not what the disk *contains*. `FSCTL_MOVE_FILE` semantics are unverified on real media (S-16). |
| T-02 | Power loss mid-move | Torn move, unmountable volume | Low | **Critical** | Journal INTENT→DONE pairs with CRC32C; recovery replays only committed steps; 200/200 trials recovered with 0 silent corruption | Tested on the emulator and by killing a real process — never by cutting power to a real disk. `chkdsk` never ran. |
| T-03 | **Writing to solid-state media** | Drive wear; irreversible device damage | Medium if detection is wrong | **High** | Fail-closed: `SolidState` and `Unknown` both refused; no override flag exists (`ADR-004`) | A false HDD classification is the worst case in the product and is only as good as S-03, which has not run. |
| T-04 | Operating on a dirty / unmounted / BitLocker-locked volume | Filesystem damage | Medium | **High** | Pre-flight refuses (exit 11) before the first `FSCTL_MOVE_FILE` | The probes themselves are modelled, not observed (S-15). |
| T-05 | Moving `$MFT` itself | Volume unusable | Low | **High** | Tier 3 is report-only by default; tier 1 requires `--confirm-mft` **and** a backend capability that is not advertised; therefore no `$MFT` move is reachable in v1 | If tier 1 is ever enabled without S-01 passing, this becomes Critical. |
| T-06 | Two writers on the same volume | Conflicting moves, lost updates | Medium | **High** | Single-instance guard per volume; the journal's `opId` is `planHash ^ volumeSerialNumber`, so a foreign journal is detected and refused (`E_JOURNAL_SERIAL_MISMATCH`) | Not a filesystem lock. A second *copy* of the program with a different build could still interfere; the plan accepts this. |
| T-07 | Malicious or malformed scenario / config file | Code execution or DoS from parsed input | Medium (files are shareable) | **High** | Untrusted-by-default parsing: strict JSON, 1 MiB and 8-level limits, allowlisted keys, fuzzed to 6M executions with 0 findings (`FUZZING.md`) | Parser is small and fuzzed; no known issue. |
| T-08 | Path traversal via config or CSV import | Writing outside the intended directory | Low | **Medium** | Path components rejected; export paths resolved and confined | — |
| T-09 | **CSV formula injection** (`=cmd\|...`) in exported reports | Code execution on the machine that opens the export | Medium | **Medium** | Formula-leading cells defanged with a leading apostrophe (`REPORTING.md`) | A user who strips the apostrophe re-arms it. |
| T-10 | Log files leak file names and paths | Privacy | Medium | **Low–Medium** | `verboseLogPaths` off by default; user can enable it deliberately | Logs are plain text in the user profile — by design, they are user data. |
| T-11 | Elevation abuse: the whole app runs elevated | Larger attack surface while elevated | Medium | **Medium** | Least-privilege evaluation recorded in ADR-010; the scheduler path elevates the *task*, not the UI; non-elevated processes open zero volume write handles (accept criterion P1-T07 #1) | The privileged-worker split is **evaluated, not implemented** (assumption A-08). |
| T-12 | Tampered installer or binary | Full compromise | Low | **High** | Code signing planned; not yet implemented (`P8-T08`, `P9-T06`) | **Open.** Until signing exists, distribute from the repository only. |
| T-13 | Journal file tampering or truncation | Refusal to resume, or worse | Low | **Medium** | Header and per-record CRC32C; plan-hash and serial must match before any resume | A valid-CRC forgery by a local attacker is out of scope — they already have write access to the volume. |
| T-14 | Denial of service: enormous volume exhausts memory | App dies | Medium | **Low** | Chunked bitmap reads, streaming enumeration, bounded aggregation | 244M-cluster ceiling is untested (P9-T04). |
| T-15 | Antivirus blocks a move mid-operation | Partial operation | **High** | **Medium** | Move failure is a clean skip, verification decides, journal allows resume (S-09 mitigation) | Whether real Defender behaves this way is unverified. |
| T-16 | User runs it on the system volume without understanding | Slow machine, unexpected I/O | High | **Low** | Pre-flight warns; system files are skipped by policy; dry-run available | Advisory only. |

## What is *not* in the threat model

Network exposure (there is none), multi-user privilege escalation beyond the
elevated process, supply-chain attacks on build tools beyond the licence audit,
and physical access to the machine. Stating the boundary is part of the model.

## Risk register (R-097)

| Risk | Likelihood | Impact | Status | Owner |
|---|---|---|---|---|
| Real NTFS backend never exercised on Windows (P3-T13) | **High** | Blocks GA | **Open** — largest single risk | Core engineer |
| `FSCTL_MOVE_FILE` durability semantics unknown (S-16, V-20) | Medium | Data loss on power failure | Mitigated by verify-before-retry, not eliminated | Core engineer |
| `$MFT` online move impossible (S-01, V-03) | High | One feature tier lost | Accepted: tier 3 default, tier 1 unreachable | TPM |
| Renderer choice fails the performance gate (S-06) | Medium | GUI rework | Data model is renderer-agnostic | UI engineer |
| Performance budgets missed at 244M clusters (P9-T04) | Medium | Rework of aggregation | Open | Core engineer |
| Docs drift from the CLI after RC | Medium | Support burden | `docs_check.py` + traceability matrix in CI | TPM |
| Windows CI without elevation (S-13, V-16) | Medium | No VHDX tests anywhere | **Open** | Build engineer |

## Verification

* `make crash-campaign` — 200/200 recoveries, 0 silent corruption (T-02).
* `make fuzz` — 6,000,000 executions, 0 crashes/hangs/UB (T-07).
* `make audit` — licences, layers, clean-room strings (T-12 supply chain, in part).
* `python3 tools/docs_check.py` — documentation consistency (T-15 for docs).
