# Third-Party Components and Tooling Notices

AuraLite-Defrag is licensed under the Apache License, Version 2.0.
No GPL, LGPL, or AGPL dependencies are permitted in runtime, build-time,
or packaging toolchains (R-009).

## Runtime Dependencies
AuraLite-Defrag links solely against standard C++20 runtime libraries and
operating-system/Windows SDK components (Win32, Direct2D, DirectWrite, WIC,
Task Scheduler 2.0 COM, Storage/Volume IOCTLs/FSCTLs). No third-party
libraries are redistributed inside the shipped binaries.

## Build-Time and Packaging Tools (Not Redistributed)
| Name | Purpose | Licence | Source | Notes |
|---|---|---|---|---|
| CMake | Build system generator | BSD-3-Clause | https://gitlab.kitware.com/cmake/cmake/-/raw/master/LICENSE.rst | Build-time only |
| MSVC v143 / Windows SDK | Compiler & OS headers | Proprietary Microsoft SDK/VS | Microsoft Visual Studio | Build-time only |
| LLVM / clang-tidy / clang-format | Static analysis & formatting | Apache-2.0 WITH LLVM-exception | https://llvm.org/LICENSE.txt | Build-time only |
| Python 3 | Build/audit scripts | PSF-2.0 | https://docs.python.org/3/license.html | Build-time only |
| Inno Setup (optional installer) | Windows installer generator | Inno Setup License (custom permissive) | https://jrsoftware.org/files/is/license.txt | Build-time only; see ADR-009 |
| MSIX SDK tools (MakeAppx/SignTool) | Package & Authenticode signing | Microsoft Windows SDK | Windows SDK | Build-time only |

Note: Catch2 (BSL-1.0) and nlohmann/json (MIT) are supported as optional
external test/tooling dependencies in CMake, while the core repository ships
a zero-dependency hardened strict JSON parser and test runner to guarantee
offline, reproducible builds with minimal supply-chain surface.
