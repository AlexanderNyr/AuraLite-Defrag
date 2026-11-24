# Clean-Room Terminology & Identifier Register (R-002, R-089)

The rule is simple and absolute: every word this product shows the user was
written for this product. Not paraphrased from another defragmenter's UI, not
translated from its help file, not "close enough". Familiarity of *workflow* is
required by the plan; familiarity of *wording* is forbidden.

## Our own identifiers

| Thing | Identifier |
|---|---|
| Product name | `AuraLite-Defrag` |
| CLI binary | `auralite-cli` |
| GUI binary | `AuraLiteDefrag.exe` |
| Context-menu label | `Defragment with AuraLite-Defrag` |
| String-table prefix | `IDS_*` in `res/strings/ids.h` |

## Prohibited terms

Checked mechanically — word-boundary matched, case-insensitive — across `src/`,
`res/`, `tests/`, `packaging/`, `docs/` and the root markdown files, by
`tools/check_strings.py`:

* The three product names this project must never resemble. They are **not
  reproduced here** — an earlier draft of this file spelled them out and the
  scanner correctly failed the build on its own register. The authoritative list
  is the `FORBIDDEN_TERMS` array in `tools/check_strings.py`:

  ```
  $ grep -A3 'FORBIDDEN_TERMS = ' tools/check_strings.py
  ```

That self-inflicted failure is worth keeping in the record: a clean-room policy
that exempts the document describing it is not a policy. There is no allowlist.

### Why the scan directories matter

The scanner originally covered `docs/user/` and `docs/dev/` only. During P9-T07
that was found to leave `docs/adr/`, `docs/cleanroom/`, `docs/spikes/` and the
root files unscanned — and a planted violation in `docs/adr/README.md` was
invisible to it. The scope was widened to all of `docs/` plus the root files, and
the planted violation then failed the build as it should. A clean-room check that
does not scan the files people actually read is decoration.

## Wording rules applied when writing UI copy

1. **Describe what happens, not what it resembles.** "Move fragmented files to
   contiguous space" rather than a phrase remembered from elsewhere.
2. **The user's vocabulary, not the industry's.** "Files that are split into
   pieces", not "extent discontiguity".
3. **No borrowed error phrasing.** Error text is derived from our own error
   taxonomy (`IDS_*` per `ErrorCode`), one key per code.
4. **Numbers are formatted by our own formatters**, not copied string formats.

## Review checklist (DOC-01)

- [ ] Every user-visible string is an `IDS_*` key, none inline.
- [ ] Every `IDS_*` key used in code exists in `res/strings/en-US.rc`.
- [ ] No prohibited term anywhere outside this file and the scanner.
- [ ] Every asset in `res/assets/` is registered with a matching hash.
- [ ] No screenshot, icon or font of unknown origin.
- [ ] Sample copy reviewed for resemblance, not just for plagiarism — the
      clean-room failure that matters is unconscious imitation, not copying.
