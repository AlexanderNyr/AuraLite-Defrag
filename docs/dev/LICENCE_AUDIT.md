# Licence and provenance audit (P9-T08)

Covers **P9-T08** (R-003, R-009).

## What the audit checks

`tools/check_licences.py` enforces the four acceptance criteria against the
repository itself, not against `licenses.json` in isolation.

| Criterion | How it is checked |
|---|---|
| (1) 0 GPL/LGPL/AGPL | allowlist per entry, word-boundary scan of every source file, and enumeration of every `#include <...>` in `src/`, `tools/`, `tests/` |
| (2) 100% of assets have author/licence/hash | asset register vs disk, both directions, SHA-256 recomputed |
| (3) every non-OSS licence justified | `OS-Component`, `Proprietary-Microsoft-SDK` and `Inno-Setup-License` require a justification of ≥20 characters |
| (4) SBOM matches artifacts | `sbom.json` regenerated from the tree and compared field by field |

## Why the previous check was not an audit

The original `check_licences.py` validated `licenses.json` against a hard-coded
allowlist and nothing else. It was circular: the file agreed with itself, so it
always passed. Three concrete consequences were found:

1. **A phantom dependency passed.** Adding an invented entry
   (`cmark-gfm`, BSD-2-Clause, `redistributed: true`) to `licenses.json` gave
   `PASS (6 entries verified)`. Nothing cross-referenced it against the build,
   THIRD-PARTY.md, or any actual artifact.
2. **THIRD-PARTY.md had already drifted by four entries.** Python 3, MSVC,
   MSIX tools and the two optional dependencies were documented there and
   absent from `licenses.json`. The audit could not see it because it never
   read THIRD-PARTY.md.
3. **No hash was ever verified.** `asset_register.json` existed with correct
   SHA-256 values, but no tool compared them with the files on disk.

## What the audit actually finds in this repository

The include-graph check is the load-bearing one. Every `#include <...>` across
the tree resolves to either a C++ standard header or an OS/SDK header — the
project has **zero third-party library dependencies**, which is what makes the
"no GPL" claim true by construction rather than by assertion. The complete set
is C++20 standard headers plus `windows.h`, `winioctl.h`, `d2d1.h`, `dwrite.h`,
`wincodec.h`, `taskschd.h`, `commctrl.h`, `commdlg.h`, `comutil.h`, `shlwapi.h`,
`shobjidl.h`, `shellapi.h`, and POSIX `unistd.h`, `fcntl.h`, `sys/stat.h`,
`sys/types.h`, `sys/wait.h`.

## Proving the audit is not vacuous

`tools/test_audit_negative.py` (`make audit-negative`) breaks one thing at a
time and asserts the audit notices:

| Case | Expected finding |
|---|---|
| `#include <nlohmann/json.hpp>` added to a source file | `undeclared library header` |
| One byte appended to an asset | `sha256 mismatch` |
| An extra PNG dropped into `res/assets/` | `asset on disk is not registered` |
| A `GPL-3.0` entry added to `licenses.json` | `forbidden copyleft` |
| An `OS-Component` entry with no justification | `requires a written justification` |
| A component only in `licenses.json`, not THIRD-PARTY.md | `not mentioned in THIRD-PARTY.md` |
| `sbom.json` edited by hand | `SBOM … differs` |
| A comment containing the word GPL in a source file | `copyleft reference in source` |

10/10 pass, and the tree is restored afterwards even if a case raises.

## Word-boundary subtlety

The copyleft scan uses `\b(GPL|LGPL|AGPL|COPYLEFT)\b`. A substring scan
flagged **48 lines** in this repository as copyleft violations because the
project's own type `DefragPlan` contains the letters `GPL`. That is a
false-positive class worth knowing about: a naive grep-based licence gate would
have been unusable here, and, worse, would have been switched off.

## SBOM

`sbom.json` (`auralite-sbom/1`) records:

* every component with scope, licence and whether it is redistributed;
* every asset with its SHA-256;
* every string resource with its SHA-256;
* the source file count.

It is generated, never hand-edited: `make sbom` regenerates it and then runs
`git diff --exit-code sbom.json`, so a stale SBOM fails the build rather than
drifting.

## Results

```
check_licences.py: PASS — 10 licence entries, 3 assets hashed,
                           0 undeclared headers, 0 copyleft references, SBOM in sync.
```

## Still open

* **Legal sign-off** is a human step and is not something this repository can
  perform. The audit produces the evidence a reviewer needs; it does not
  substitute for the review.
* **Installer artefacts** are not in the tree (only Inno Setup templates), so
  the audit covers the source repository. Once P8-T08 produces a real
  installer, its bundled files must be added to the SBOM for criterion (4) to
  cover the shipped artifact rather than the source tree.
* `SPDX`/`CycloneDX` export is not implemented; the in-tree format is
  deliberately small and exactly checkable. Converting to CycloneDX would be
  mechanical if a consumer needs it.
