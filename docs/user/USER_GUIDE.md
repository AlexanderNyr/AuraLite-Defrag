# AuraLite-Defrag User Guide

## Safety Policies
- **Fail-Closed SSD Protection:** AuraLite-Defrag automatically detects SSD/NVMe drives (and any drive whose media type cannot be verified) and restricts them to **Analyze-only** mode. There is no override flag.
- **Pre-Flight Checks:** Dirty volumes, read-only volumes, BitLocker-locked volumes, non-NTFS volumes, and volumes with `< 5%` free space are blocked from writes before any cluster is moved. Volumes with `5%–15%` free space permit `Quick Defrag` and `Defrag Free Space` with a warning.

## Modes
- **Analyze:** Enumerates volume geometry, cluster bitmap, `$MFT` zone, and file fragments read-only.
- **Defrag:** Consolidates fragmented user files contiguously outside the `$MFT` Zone.
- **Quick Defrag:** Targets larger multi-fragment files using a bounded cluster budget.
- **Defrag Free Space:** Compacts scattered files downward into smaller free gaps without splitting the largest free run.
- **Move Large Files to End:** Places files ≥ threshold (default 100 MiB) into the upper LCN region.
- **MFT Zone Evacuate / MFT Optimize:** Tiered `$MFT` optimization (Tier 3 report-only by default, Tier 2 zone evacuation, Tier 1 direct move when supported & confirmed).
- **Demo / Simulation Mode:** Available from Tools → Demo / Simulation Mode in the GUI or `--sim <scenario.json>` in the CLI.
