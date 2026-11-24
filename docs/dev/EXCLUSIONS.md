# Exclusions engine (P8-T02 / R-037)

Single implementation of "is this file excluded", shared by the planner, the
CLI, the scheduler and the UI filter. Lives in `src/core/exclusions.{hpp,cpp}`
(L0) so every layer can reach it; `PolicyGate` delegates to it, so there is
exactly one set of semantics in the product.

## Rule kinds

| Kind | Pattern form | Matches |
|---|---|---|
| `Path` | `c:\windows\`, `*\cache\*`, `d:\vm` | glob against full path, then bare name, then directory prefix |
| `Extension` | `*.tmp`, `.tmp`, `ext:.tmp` | the file's extension |
| `NameGlob` | `name:*.bak` | the bare filename only |
| `Category` | `category:archive` | the file's `FileCategory` |
| `SizeRange` | `size:` + bounds | size bounds only, no pattern |

A bare `.tmp` or `*.tmp` is recognised as an extension rule, not a path — that
is what a user means by it. `minSizeBytes` / `maxSizeBytes` apply to every kind,
so "exclude `.log` files larger than 10 MB" is one rule, not two.

## Glob semantics

| Token | Meaning |
|---|---|
| `*` | any run of characters **except** a path separator |
| `**` | any run including separators |
| `?` | exactly one non-separator character |
| `[abc]`, `[a-z]`, `[!a-z]` | character class, ranges, negation |

A leading `**\` means "at any depth, including the root": `**\*.tmp` matches
both `c:\x.tmp` and `c:\a\b\x.tmp`. Without that, the most natural way to write
"every temp file" would silently miss the volume root.

Matching is case-insensitive **for ASCII only**. Non-ASCII comparisons are
byte-exact, because NTFS case tables are volume- and locale-dependent and
guessing produces both over- and under-exclusion. This is a deliberate,
documented limitation.

Paths are canonicalised before comparison: `\\?\` and `\\.\` prefixes are
stripped, `/` becomes `\`, separator runs collapse, trailing separators drop,
and ASCII is lowercased. Patterns go through the same normalisation, so a rule
written in long-path form matches a canonical file path.

## Performance and safety

The matcher is iterative with a backtrack **stack**, no recursion. A single
backtrack pointer is not enough once `*` is separator-bounded: for
`**\*.tmp` against `a\b\file.tmp` the inner star must stop at the second
separator, and the only correct recovery is to unwind to the outer `**`. The
stack keeps that option; the frame array is fixed-size, so matching cannot
allocate or overflow the call stack.

Rejected at load time rather than silently ignored:

* unbalanced `[` in a pattern;
* `minSizeBytes > maxSizeBytes`;
* unknown category name;
* empty pattern on a non-size rule.

A rejected rule set does **not** replace the previous one, so a bad config edit
cannot leave the process with no exclusions at all.

## Verdicts

`Evaluate()` returns not just a boolean but the index of the rule that matched
and its normalised pattern. The UI needs this to show "matched rule #3:
`*.log`" in the settings editor, and the skip report uses it to attribute an
exclusion rather than emitting a generic reason.

## Import / export

`ImportJson` / `ExportJson` use the same object shape as the `exclusions` array
in the config file. Imports are compiled before being accepted, so a rule set
that would match nothing is a hard error instead of a silent no-op. Export
writes the normalised form, which means the round-trip is idempotent.

## Verification

`make exclusions` — 12 tests:

| Test | Proves |
|---|---|
| `TestThousandPatternCorpus` | acceptance (1): 1000 generated patterns, each with a positive and a negative case, 1000/1000 |
| `TestNoExcludedFileEntersPlan` | acceptance (2): no excluded id appears in a `DefragPlan` |
| `TestRulesSurviveConfigRoundTrip` | acceptance (3): semantics preserved through config save/load |
| `TestImportExportRoundTrip` | import→export→import is stable, including rule attribution |
| `TestGlobDoesNotCrossSeparators` | `*` vs `**` boundary behaviour |
| `TestPathologicalPatternsTerminateFast` | 4000-char inputs, 200-star patterns; linear-ish, no stack growth |
| `TestInvalidRulesAreRejected` | every malformed rule is an error and leaves the old set intact |
| `TestVerdictIdentifiesTheRule` | correct rule index for each kind |
| `TestLongPathAndUnicode` | `\\?\`, `\\.\`, >260-char paths, Cyrillic directory names |
| `TestDisabledRulesAreInert` | `enabled:false` counts but never matches |
| `TestDeterminismAcrossEvaluations` | 1000 repeated evaluations give identical verdicts |
| `TestPolicyGateSharesTheImplementation` | `PolicyGate` and `ExclusionEngine` agree exactly |

End-to-end through the CLI (`--exclude`), on SC-01 (960 moves baseline):

| Rule | Moves | Effect |
|---|---|---|
| none | 960 | — |
| `*.log` | 642 | −318 |
| `*.zip` | 636 | −324 |
| `category:archive` | 636 | −324 (identical to `*.zip`, as expected) |

## Still open

* A GUI editor for the rules is P6-T07 and needs a Windows build; the engine
  already exposes the normalised form and the matched-rule index it needs.
* Scheduler-side enforcement (P8-T01) calls the same `PolicyGate`, so it needs
  no extra work once the scheduler exists.
* Shell context-menu "Add to exclusions" (P8-T03) is Windows-only.
