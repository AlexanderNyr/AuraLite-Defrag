# Third-Party Components and Tooling Notices

AuraLite-Defrag is licensed under the Apache License, Version 2.0.
No GPL, LGPL, AGPL, MPL, CDDL, EPL, SSPL or Commons-Clause dependency is
permitted in runtime, build-time or packaging toolchains (R-003, R-009).

This file is checked for parity against [`licenses.json`](licenses.json) by
`tools/check_licences.py`: every entry here must appear there and vice versa, so
the two documents cannot drift apart silently. Hashes for every visual and
string resource are in [`sbom.json`](sbom.json).

## Runtime Dependencies

**None.** AuraLite-Defrag links solely against the C++20 standard library and
operating-system / Windows SDK components (Win32, Direct2D, DirectWrite, WIC,
Task Scheduler 2.0 COM, Storage and Volume IOCTLs/FSCTLs). No third-party
library is compiled into or redistributed with the shipped binaries.

This is not a claim taken on trust. `tools/check_licences.py` enumerates every
`#include <...>` under `src/`, `tools/` and `tests/` and fails the build if a
header is neither a C++ standard header nor an OS/SDK header. The current tree
resolves to zero undeclared headers.

The repository also ships its own strict JSON parser and test runner rather than
adopting external ones, which is what keeps the runtime dependency set empty.

## Build-Time, QA and Packaging Tools (not redistributed)

| Name | Purpose | Licence | Source |
|---|---|---|---|
| CMake | Build system generator | BSD-3-Clause | https://gitlab.kitware.com/cmake/cmake/-/raw/master/LICENSE.rst |
| MSVC v143 / Windows SDK headers | Compiler and OS headers | Proprietary-Microsoft-SDK | https://visualstudio.microsoft.com/license-terms/ |
| Windows SDK (Win32, D2D1, DWrite, WIC, TaskSched) | Platform APIs | OS-Component | https://learn.microsoft.com/en-us/windows/win32/ |
| LLVM clang-tidy / clang-format | Static analysis and formatting | Apache-2.0 WITH LLVM-exception | https://llvm.org/LICENSE.txt |
| Python 3 | Repository audit and build scripts | PSF-2.0 | https://docs.python.org/3/license.html |
| Inno Setup (optional installer) | Windows installer generator | Inno-Setup-License | https://jrsoftware.org/files/is/license.txt |
| MSIX SDK tools (MakeAppx/SignTool) | Packaging and Authenticode signing | OS-Component | https://learn.microsoft.com/en-us/windows/msix/ |
| Catch2 (optional, not used) | Optional external test framework | BSL-1.0 | https://github.com/catchorg/Catch2/blob/devel/LICENSE.txt |
| nlohmann/json (optional, not used) | Optional tooling JSON library | MIT | https://github.com/nlohmann/json/blob/develop/LICENSE.MIT |

Catch2 and nlohmann/json are named in the CMake files as optional external
dependencies, but no build configuration in this repository links either of
them: the default build uses the in-tree JSON parser and test runner. They are
listed here rather than omitted so that the SBOM reflects the CMake surface
honestly.

Three entries carry licences that are not open-source licences
(`OS-Component`, `Proprietary-Microsoft-SDK`, `Inno-Setup-License`). Each one
has a written justification in `licenses.json` explaining why it is acceptable;
the audit refuses to pass an entry that needs one and does not have it.

## Visual and String Assets

All visual assets in `res/assets/` were authored from scratch for this project
under Apache-2.0, and every one is registered in
`res/assets/asset_register.json` with author, source, licence and a SHA-256 that
the audit re-computes on every run. Provenance details are in
`docs/cleanroom/asset_provenance.md`.

## Verification

```
make audit           # runs check_layers, check_strings, check_licences
```

`check_licences.py` enforces all four P9-T08 acceptance criteria:

1. zero GPL/LGPL/AGPL — by allowlist, by word-boundary scan of all source, and
   by inspecting what the build actually includes;
2. every asset has author, licence and a matching hash — asset register vs disk,
   in both directions (an unregistered file on disk also fails);
3. every non-OSS licence has a written justification of at least 20 characters;
4. the SBOM matches the tree exactly — `sbom.json` is regenerated from the
   repository on every run and compared byte-for-byte at the JSON level.

Negative tests confirming the audit is not vacuous live in
`docs/dev/LICENCE_AUDIT.md`.
