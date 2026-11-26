#!/usr/bin/env python3
"""Syntax-check the Windows-only translation units from a non-Windows host.

Why this exists
---------------
`src/backend/ntfs/ntfs_backend.cpp`, `media_detector.cpp`, `win32_platform.cpp`,
`task_scheduler.cpp` and `context_menu.cpp` are behind `if(WIN32)` in CMake, so a
Linux `make test` never parses them. Two build breaks reached the Windows machine
that way (C2676 `operator+` with `std::string_view`), each costing a full
push/pull/build cycle on the user's side.

A cross compiler parses them here first. The check is a *syntax and type* check
only: mingw's SDK headers are not MSVC's, so it cannot replace the real build.
It catches exactly the class of mistake that keeps slipping through - invalid
expressions, wrong overloads, missing declarations - not SDK differences.

Usage:
    python3 tools/win32_syntax_check.py            # human readable
    python3 tools/win32_syntax_check.py --quiet    # exit code only

Exit codes: 0 = all TUs parse, 1 = a TU failed, 2 = no cross compiler available.
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys

CXX = "x86_64-w64-mingw32-g++"

# Windows-only TUs, kept in sync with the `if(WIN32)` blocks in CMakeLists.txt.
TUS = [
    "src/platform/win32_platform.cpp",
    "src/backend/ntfs/media_detector.cpp",
    "src/backend/ntfs/ntfs_backend.cpp",
    "src/scheduler/task_scheduler.cpp",
    "src/shell/context_menu.cpp",
    # AuraLiteDefrag (WIN32 GUI target) - same trap: never compiled on Linux.
    "src/ui/theme.cpp",
    "src/ui/d2d_renderer.cpp",
    "src/ui/wic_exporter.cpp",
    "src/ui/main_window.cpp",
    "src/app/win_main.cpp",
]

# mingw ships MFT_ENUM_DATA but not the _V0 alias MSVC defines; it is the same
# structure, so aliasing it locally keeps the check honest without touching src/.
COMPAT_DEFINES = [
    "-DMFT_ENUM_DATA_V0=MFT_ENUM_DATA",
    # CMake adds these for a WIN32 GUI target on Windows; without them every
    # TCHAR/LPSTR site in src/ui looks like a type error.
    "-DUNICODE",
    "-D_UNICODE",
]

FLAGS = ["-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-Wno-unknown-pragmas"]

# CRT entry points MSVC deprecates with C4996 ("This function or variable may be
# unsafe"). The Windows build treats C4996 as an error, and mingw does not warn
# about any of them, so `getenv` in ntfs_backend.cpp reached the user's machine
# and stopped the build after this check had reported PASS. The list is scanned
# in src/ only: the benchmark and test tools legitimately parse argv with atoi.
BANNED_CRT = {
    "getenv": "use GetEnvironmentVariableW (Win32) or _dupenv_s",
    "strcpy": "use std::string / strcpy_s",
    "strcat": "use std::string",
    "sprintf": "use std::format-style or snprintf",
    "sscanf": "use std::from_chars",
    "scanf": "use std::from_chars",
    "strncpy": "use std::string",
    "localtime": "use gmtime_s / localtime_s",
    "asctime": "use a formatted string",
    "gets": "use std::getline",
}


def scan_banned_crt(root: pathlib.Path) -> list[tuple[str, int, str, str]]:
    """Find deprecated CRT calls that MSVC turns into errors and mingw does not."""
    import re
    found: list[tuple[str, int, str, str]] = []
    # std:: is allowed in front: MSVC deprecates the C names under both spellings,
    # which is exactly how std::getenv slipped through the first version of this scan.
    pattern = re.compile(r"(?<![\w.>])(?:std::)?(" + "|".join(BANNED_CRT) + r")\s*\(")
    for path in sorted((root / "src").rglob("*")):
        if path.suffix not in {".cpp", ".hpp"}:
            continue
        for lineno, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            code = line.split("//", 1)[0]
            for m in pattern.finditer(code):
                found.append((str(path.relative_to(root)), lineno, m.group(1),
                              BANNED_CRT[m.group(1)]))
    return found


def main(argv: list[str]) -> int:
    quiet = "--quiet" in argv
    root = pathlib.Path(__file__).resolve().parent.parent

    if shutil.which(CXX) is None:
        print(f"{CXX} not found; install g++-mingw-w64-x86-64 to enable this check",
              file=sys.stderr)
        return 2

    failures: list[tuple[str, str]] = []
    for tu in TUS:
        path = root / tu
        if not path.exists():
            failures.append((tu, "file missing"))
            continue
        cmd = [CXX, *FLAGS, *COMPAT_DEFINES, "-I", str(root / "src"), str(path)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            errs = [ln for ln in proc.stderr.splitlines() if "error" in ln]
            failures.append((tu, "\n".join(errs[:10]) or "unknown failure"))
        elif not quiet:
            print(f"OK   {tu}")

    banned = scan_banned_crt(root)
    if banned:
        print("\nwin32_syntax_check.py: FAIL (MSVC-deprecated CRT call in src/)",
              file=sys.stderr)
        for rel, lineno, fn, advice in banned:
            print(f"  {rel}:{lineno}: {fn}() -> {advice}", file=sys.stderr)
        return 1

    if failures:
        print("\nwin32_syntax_check.py: FAIL", file=sys.stderr)
        for tu, msg in failures:
            print(f"\n{tu}:\n{msg}", file=sys.stderr)
        return 1

    if not quiet:
        print(f"\nwin32_syntax_check.py: PASS ({len(TUS)} Windows-only TUs parse, "
              f"no MSVC-deprecated CRT calls in src/)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
