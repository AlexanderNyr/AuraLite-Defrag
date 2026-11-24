# Parser fuzzing and input hardening (P9-T01)

Covers **P9-T01** (R-084, R-054) — the last MUST-severity security task that can
be completed without a Windows runner.

## What is fuzzed

| Target | Entry point | What it guards |
|---|---|---|
| `json` | `ParseJsonStrict` | every JSON consumer: scenarios, config, golden traces |
| `scenario` | `LoadScenarioFile` | the untrusted one — scenario files are user-supplied |
| `config` | `ParseConfigJson` + `ExclusionEngine::SetRules` | config and exclusion rules |
| `csv` | `ParseCsv`, `ParseFileList`, `ParseSkipReport`, `ParseStatistics` | import of third-party CSV |
| `usn` | `ParseUsnBuffer` | a corrupt or hostile volume's MFT enumeration buffer |
| `utf16` | `Utf16LeToUtf8` | file names from the same source |

## Running

```
make fuzz-quick                 # 30k executions, ~1 s, for local loops
make fuzz                       # 6,000,000 executions under ASan/UBSan
make usn                        # 14 unit tests for the USN parser
```

The fuzzer is deterministic: `--seed` fixes the PRNG, so any failure replays
exactly from its seed. `--target NAME` narrows to one parser; `--iterations N`
changes the count.

## Results

| Target | Executions | Accepted | Rejected | Slowest input |
|---|---|---|---|---|
| json | 1,000,000 | 29,117 | 970,883 | 648 µs |
| scenario | 1,000,000 | 3,287 | 996,713 | 36.8 ms |
| config | 1,000,000 | 3,856 | 996,144 | 18.4 ms |
| csv | 1,000,000 | 915,079 | 84,921 | 36.8 ms |
| usn | 1,000,000 | 1,000,000 | 0 | 25.0 ms |
| utf16 | 1,000,000 | 299,095 | 700,905 | 5.2 ms |
| **total** | **6,000,000** | | | |

**0 crashes, 0 hangs, 0 ASan/UBSan findings**, 160 s wall clock under
sanitizers. The plan asks for 1,000,000; the run does 6,000,000 so every parser
gets the full budget rather than sharing it.

`usn` accepting every input is correct, not a measurement artefact: a buffer
shorter than the 8-byte header is a legal end-of-enumeration, and the parser is
specified to return success with zero records. The bounds behaviour is asserted
separately by the unit tests and by the invariant checks inside the fuzz target
(`bytesConsumed <= input.size()`, no zero-length record emitted, every emitted
name is valid UTF-8).

## Limit enforcement (acceptance criterion 3)

`CheckLimits()` runs before any fuzzing and fails the build if:

* an input of 1 MiB + 1 byte is **accepted**;
* an input of exactly 1 MiB is **rejected**;
* nesting of exactly 8 levels is **rejected**;
* nesting of 9 levels is **accepted**;
* nesting of 10,000 levels is **accepted** (stack-exhaustion risk);
* the scenario loader accepts an oversize file at its own entry point.

## Structural fix this task required

The USN record walk lived inline in `RealNtfsBackend::Enumerate()` inside
`#ifdef _WIN32`. That made it the largest piece of untrusted-input handling in
the product **and** unreachable by any test, since a Linux CI run cannot compile
the Win32 backend at all.

It moved to `src/backend/usn/usn_parser.{hpp,cpp}` — platform-independent, no
Windows headers, compiled and fuzzed on Linux. The Win32 backend now calls it,
so there is one implementation and it is the tested one. `USN_RECORD_V2` no
longer appears in `ntfs_backend.cpp`.

Every field in that buffer is attacker-controlled. The parser checks, in order:

| Check | Failure mode it prevents |
|---|---|
| `offset + RecordLength` computed in 64-bit | 32-bit wrap makes a 0xFFFFFFF0 length look in-bounds |
| `RecordLength != 0` | infinite loop in the record walk |
| `FileNameOffset >= 60` (the V2 fixed size) | name read from inside the header |
| `nameOffset + nameLength <= recordEnd` | name read past the record |
| `FileNameLength` is even | half a UTF-16 unit decoded as a character |
| version is 2 | V3's 128-bit FRN parsed as a 64-bit one |
| UTF-16 decodes without unpaired surrogates | invalid UTF-8 reaching the file list |
| `maxRecords` bound | 1 MiB of minimal records exhausting memory |

## Three bugs found in the harness itself

Worth recording, because each one would have made the campaign report a clean
bill of health that meant nothing:

1. **Per-target counters were never incremented.** Targets incremented the
   run-wide totals while the per-target line printed a fresh local `Stats`, so
   every line read `accepted=0 rejected=0`. The parsers were fine; the report
   was blind. Fixed with a `gCurrent` pointer.
2. **The mutator spliced across formats.** Seeds were drawn from one global
   list, so a scenario document would be spliced with a CSV sample and could
   never parse — `scenario` showed 0/5000 accepted and the interesting decode
   paths were never reached. Fixed with a per-target corpus.
3. **The scenario seed used invented field names.** The loader rejects unknown
   keys, so the seed was rejected instantly. Fixed to match the real schema
   (`volume.clusters`, not `volume.totalClusters`).

## Still open

* Long-running campaign: the plan's release gate wants the 1,000,000-run figure
  from CI, which is in place; a multi-hour campaign is not run here.
* `SEC-02` also covers path traversal and TOCTOU in file handling — that is
  P9-T02 and needs a real volume for the TOCTOU half.
* libFuzzer/AFL integration would give coverage guidance. The current driver is
  deterministic reproduction-first, which is the right trade for a 1M-run CI
  gate; switching later does not invalidate these results.
