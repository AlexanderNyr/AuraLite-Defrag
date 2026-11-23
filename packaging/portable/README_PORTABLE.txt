AuraLite-Defrag Portable Mode
============================

To run AuraLite-Defrag in portable mode without writing to %APPDATA% or the Windows Registry:
1. Extract AuraLiteDefrag.exe and auralite-cli.exe to a writable folder.
2. Place an empty marker file named `auralite.portable` beside the executables.
3. Configuration (`config.json`), logs, and journals will stay inside that folder.

Note: Portable mode never alters the fail-closed SSD protection policy.
