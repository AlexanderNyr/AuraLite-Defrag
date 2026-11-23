# Contributing to AuraLite-Defrag

## Clean-Room & Licensing Rules
- All contributions are licensed under **Apache-2.0**.
- Never copy, trace, or adapt logos, icons, bitmaps, product names, resource identifiers, or UI strings from any proprietary disk utility.
- Every UI-visible string must live in the string-table registry (`res/strings/ids.h`, `res/strings/en-US.rc`, and `src/core/strings.cpp`) and pass the 2x pseudo-locale expansion test (`tools/gen_pseudo_locale.py`).
- No GPL, LGPL, or AGPL dependencies or tools may be introduced. Run `python3 tools/check_licences.py` before opening a PR.

## Architecture Layer Rule
Dependencies flow strictly downward (`L6 -> L5 -> L4 -> L3 -> L2 -> L1`, with `L0` available to all):
- `L6` (`src/ui/`, `src/cli/`) must never call `L1`/Win32 storage FSCTLs directly or branch on real vs. emulator backends.
- `L2` (`src/backend/ntfs/`, `src/backend/emulator/`) owns all volume handles and backend-specific types.
- Run `python3 tools/check_layers.py` and `python3 tools/check_strings.py` to verify compliance.

## Coding Standards
- C++20 (`/std:c++20`, `/W4 /WX`, `-Wall -Wextra -Werror -pedantic`).
- RAII for all OS handles; fixed-width integers for journal/wire structs; zero exceptions across thread boundaries.
- Format code with `clang-format` and run ASan/UBSan test suites before submitting.
