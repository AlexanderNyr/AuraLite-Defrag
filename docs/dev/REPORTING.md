# Reporting: charts, Top-N, CSV export

Covers **P7-T05** (R-071 fragment histogram), **P7-T06** (R-072 Top-N),
**P7-T08** (R-073, R-084 CSV export) and the data model from **P7-T01** (R-074).

The whole layer is platform-independent and is exercised on Linux by
`make reporting`, so it can be verified without a Windows runner.

## Commands

| Command | What it proves |
|---|---|
| `make reporting` | 16 acceptance tests, ASan/UBSan |
| `make bench-reported` | release-build timings against the plan's budgets |
| `make charts` | chart data model (P7-T01), 12 tests |

## Fragment histogram (P7-T05)

Bins are fixed by the plan: `1`, `2–4`, `5–16`, `17–64`, `65+`, with resident
files counted separately rather than folded into the `1` bin.

Invariant enforced on every snapshot:

```
bins[0] + bins[1] + bins[2] + bins[3] + bins[4] + residentCount == totalFiles
```

The test runs this over all 20 scenarios and additionally re-derives the bins
with an independent implementation to confirm the counts are exact — that is
what makes the tooltips trustworthy.

Budget: **50 ms at 250k files**. Measured **2.3 ms** (release `-O2`).

## Top-N (P7-T06)

Ranking key, in order: `fragmentCount` desc, `sizeBytes` desc, `id` asc. The
`id` tiebreak is what makes the ordering stable and reproducible — without it
`std::sort` may permute equal elements and the UI table would reshuffle between
identical runs.

`ComputeTopN` was rewritten for this task. The original materialised a
`TopFragmentedEntry` — two `std::string` copies — for every fragmented file and
then fully sorted the result: **478 ms** at 250k files, 24× over budget. It now
ranks pointers to `FileEntry` with `std::partial_sort` (O(m log n)) and copies
strings only for the winners: **1.6 ms**. Same ranking, verified against an
independent ground-truth implementation over 20 snapshots × 4 values of N.

Selection linkage is by `FileId`, and the test asserts that every extent of a
selected file is reachable, so the map highlight covers 100% of its clusters.

Budget: **20 ms at 250k files**. Measured **1.6 ms**.

> Note on measurement: the unit tests run under ASan, which inflates these
> timings roughly 10×. Budgets are asserted there against a widened bound to
> still catch algorithmic regressions, while the plan's actual numbers are
> checked by `make bench-reported` in a plain `-O2` build.

## CSV export (P7-T08)

Three exports, all RFC 4180, all prefixed with a UTF-8 BOM and a
`SchemaVersion` column:

| Export | Columns |
|---|---|
| file list | `SchemaVersion, FileId, Name, Path, SizeBytes, AllocatedClusters, Fragments, State, InMftZone` |
| statistics | volume/MFT/histogram summary, plus before-after columns when supplied |
| skip report | `SchemaVersion, FileId, Name, SkipReason` |

### Injection neutralisation

Any field starting with `=`, `+`, `-`, `@`, `\t` or `\r` has a single quote
prepended before quoting, which makes spreadsheet software treat it as text.
20 fixtures are asserted, including DDE payloads, `HYPERLINK`, `WEBSERVICE`,
`IMAGE`, and the tab/CR-prefixed variants that bypass naive `=`-only checks.

The round-trip contract is exact: the parsed value is either the original string
or the original with exactly one leading quote — nothing else is accepted.

### Round-trip

`src/reporting/csv_reader.{hpp,cpp}` is the reader half. It exists because
acceptance criterion (1) — *250k rows round-trip with 0 lost or duplicated
records* — cannot be checked without a parser, and a quoting bug in the writer
is otherwise invisible until a customer's spreadsheet mangles a path.

The test writes and re-reads 250,000 rows, seeded with hostile content every
thousandth row (commas, quotes, embedded newlines, CRLF, non-ASCII, formulas),
then verifies field-by-field equality and that ids are exactly `1..N` in order.
Write: **285 ms**, read: **2.1 s**, 15 MiB.

The reader rejects malformed input rather than guessing: unterminated quotes,
wrong column counts, and unsupported schema versions are all errors.

### No HTML export

Acceptance criterion (4) forbids an HTML path outright — HTML reports are the
standard bypass for CSV-injection defences. A tripwire test fails the build if a
source file matching `html`/`Html` appears under `src/`, or if `html` appears in
the exporter's public API.

## Integration notes

`CsvExporter` already existed before this task; what was missing was the reader,
the fixture corpus, the schema documentation, and a rigorous check of the
Top-N histogram path. `StatisticsEngine::ComputeTopN` changed behaviourally
(nothing observable, only the sort strategy) and is covered by the ground-truth
comparison.

The CSV format is versioned. Adding a column requires bumping
`SchemaVersion` and extending `kSupportedSchemaVersion` in `csv_reader.cpp`;
otherwise the reader will correctly reject the file.

## What is still open

* The UI wiring — menus, Save dialog, progress — belongs to P7-T09 and needs a
  Windows build.
* Histogram *rendering* (the log scale and tooltips) is P7-T05's UI half; only
  the data and the exact counts are proven here.
* Locale: the exporter always writes `.` as the decimal separator. Excel in a
  comma-decimal locale will still parse it correctly because the columns are
  comma-separated and the BOM tells it UTF-8, but a `sep=` hint line is a
  possible future addition should field reports ask for it.
