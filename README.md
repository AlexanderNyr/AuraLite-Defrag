# AuraLite-Defrag

An Apache-2.0-licensed, native C++20/Win32 desktop defragmenter for **NTFS volumes in ONLINE mode on Windows 10/11 x64**, featuring a clean-room layered architecture (`L0`–`L6`) and a deterministic in-memory disk emulator (`EmulatorBackend`) that doubles as Demo/Simulation mode and the CI test oracle.

## Key Safety Principles
1. **Do no harm:** Every write passes a fail-closed `PolicyGate`, an explicit `DefragPlan`, an append-only CRC32C binary journal (`MOVE_INTENT` flushed before move, `MOVE_DONE` after post-move retrieval-pointer verification), and skip policies for NTFS metadata, pagefile/hiberfil/swapfile, locked, and unsafe files.
2. **SSD/NVMe is analysis-only:** Solid-state and `Unknown` media are blocked across every entry point (UI, CLI, scheduler, context menu, planner) with **no override** in v1.
3. **Honest MFT handling:** Four-tier strategy (Tier 1 direct online `$MFT` move if capability-gated, Tier 2 indirect zone clearing, Tier 3 report-only + `fsutil behavior query mftzone` guidance, Tier 4 post-v1 boot-time hook).
4. **Clean room, always:** 100% original code, assets, string tables, and identifiers.

## Building & Testing

### Portable / Linux CI (L0–L5, Emulator, Engine, Reporting, CLI, Unit/Scenario/Fuzz/Perf Tests)
```bash
make test
```
Or with CMake 3.25+:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Windows 10/11 x64 (MSVC v143 — Full Win32 GUI + NTFS Backend + CLI + Tests)
```powershell
cmake -G "Visual Studio 17 2022" -A x64 -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure   # 8/8
```

Step-by-step Windows instructions, including a safe way to test against a real
NTFS volume and what is expected to be open: **[docs/dev/WINDOWS_BUILD.md](docs/dev/WINDOWS_BUILD.md)**.

## CLI Quick Examples
```bash
# Run deterministic simulation on scenario SC-01 (dry-run)
auralite-cli analyze --sim tests/scenarios/SC-01.json
auralite-cli defrag --sim tests/scenarios/SC-01.json --dry-run

# Defragment simulated scenario and export CSV report
auralite-cli defrag --sim tests/scenarios/SC-01.json --export-csv out.csv

# SSD refusal verification (returns exit code 12, zero moves)
auralite-cli defrag --sim tests/scenarios/SC-14.json
```
