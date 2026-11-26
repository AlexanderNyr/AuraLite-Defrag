# AuraLite-Defrag — CLI Reference

```
auralite-cli <verb> [options]
auralite-cli --help
```

Every option, verb and exit code below was verified against the current build.
Where the implementation and an older document disagreed, the implementation
won and the document was corrected — see "Documented vs actual" at the end.

---

## Verbs

| Verb | Purpose |
|---|---|
| `analyze` | Read geometry, bitmap, `$MFT` zone and fragment counts. Read-only. |
| `defrag` | Full defragmentation. Refused on solid-state media. |
| `quick` | Defragment only heavily fragmented files, within a bounded budget. |
| `free-space` | Consolidate fragmented free space. |
| `move-large` | Relocate large files toward the end of the volume. |
| `file` | Defragment a single file. Requires `--target`. |
| `folder` | Defragment a folder subtree. Requires `--target`. |
| `mft` | Analyse or optimise `$MFT` under the tiered strategy. |
| `status` | Print a volume and MFT status summary. |
| `export` | Write an analysis or file list to CSV. Requires `--export-csv`. |

---

## Options

| Option | Argument | Applies to | Meaning |
|---|---|---|---|
| `--volume <vol>` | `D:` or `\\?\Volume{...}` | all real-volume verbs | Target NTFS volume. On non-Windows hosts only `--sim` works. |
| `--sim <scenario.json>` | path | all verbs | Run against the deterministic emulator; no disk is touched. |
| `--dry-run` | — | any writing verb | Compute and report the plan without moving clusters. |
| `--target <path>` | path | `file`, `folder` | File or folder to operate on. |
| `--exclude <pattern>` | glob / `category:` / ext | all | Add an exclusion rule. Repeatable. |
| `--mft-tier <1\|2\|3>` | 1, 2 or 3 | `mft` | MFT strategy tier. Default 3 (report-only). |
| `--confirm-mft` | — | `mft --mft-tier 1` | Required second confirmation for a direct `$MFT` move. |
| `--export-csv <path>` | path | `export`, others | Write an RFC 4180 CSV atomically. |
| `--config <path>` | path | all | Load a JSON config file. |
| `--journal <path>` | path | writing verbs | Journal file location. |
| `--format <text\|json>` | text or json | all | Output format. Default `text`. |
| `--max-files <n>` | positive integer | `analyze`, `status`, `export` | Read at most `n` files. The report then describes that sample and says so. Used to time a scan of a large volume without waiting for all of it. |
| `--log-level <lvl>` | debug, info, warn, error | all | Log verbosity. |
| `--help` | — | — | Print usage. |

---

## Exit codes

These are the values `MapErrorToCliExitCode` actually returns.

| Code | Meaning | Representative errors |
|---|---|---|
| **0** | Success | — |
| **1** | Unclassified runtime error | anything without a specific mapping |
| **2** | Invalid arguments, config or schema | `E_CONFIG_PARSE`, `E_CONFIG_SCHEMA`, `E_CONFIG_SIZE_LIMIT`, `E_CONFIG_DEPTH_LIMIT`, `E_CONFIG_PATH_TRAVERSAL`, unknown flag, missing `--target` |
| **3** | Cancelled | `E_INTERNAL_CANCELLED` (Ctrl+C) |
| **11** | Pre-flight refusal (volume state) | dirty, read-only, BitLocker-locked, unsupported filesystem, <5% free, volume inaccessible |
| **12** | Solid-state or unknown media refused, **or** internal policy violation | `E_PREFLIGHT_SSD_BLOCKED`, `E_INTERNAL_POLICY_VIOLATION` |
| **13** | Elevation required | `E_PREFLIGHT_NOT_ELEVATED` |
| **20** | Verification failed after a move | `E_VERIFY_EXTENT_MISMATCH`, `E_VERIFY_CHECKSUM_MISMATCH`, `E_VERIFY_READBACK_FAILED` |
| **21** | Journal error or recovery failure | `E_JOURNAL_CORRUPT_HEADER`, `E_JOURNAL_CRC_MISMATCH`, `E_JOURNAL_IO`, `E_JOURNAL_PLAN_HASH_MISMATCH`, `E_JOURNAL_SERIAL_MISMATCH` |

### Working with exit codes in scripts

```powershell
auralite-cli defrag --volume D: --dry-run
switch ($LASTEXITCODE) {
    0  { "plan computed" }
    12 { "drive is solid-state — analysis only, this is expected" }
    11 { "volume needs chkdsk or is read-only / locked / full" }
    13 { "run elevated" }
    default { "unexpected failure: $LASTEXITCODE" }
}
```

A refusal is not a crash: 11, 12 and 13 are policy decisions with a one-line
explanation on stderr and no partial work performed.

---

## Examples

### Analyse before committing to anything

```powershell
auralite-cli analyze --volume D: --format json
```

`--format json` emits a machine-readable summary suitable for monitoring.

### Dry run, then the real thing

```powershell
auralite-cli defrag --volume D: --dry-run
auralite-cli defrag --volume D:
```

The dry run prints the same counters the real run will, including the projected
fragmentation percentage.

### Exclude a folder and an extension

```powershell
auralite-cli defrag --volume D: --exclude "D:\VMs\" --exclude "*.vhdx"
```

Verified effect on the 960-step reference scenario: excluding `*.log` drops the
plan to 642 moves, `*.zip` to 636.

### Report to CSV

```powershell
auralite-cli export --volume D: --export-csv D:\reports\frag.csv
```

The file starts with a UTF-8 BOM and a `SchemaVersion` column. Formula-like
names are defanged with a leading apostrophe.

### MFT: look, then escalate deliberately

```powershell
auralite-cli mft --volume D: --mft-tier 3            # report only (default)
auralite-cli mft --volume D: --mft-tier 2            # evacuate the zone
auralite-cli mft --volume D: --mft-tier 1 --confirm-mft   # direct $MFT move
```

Tier 1 additionally requires the backend to advertise the capability, which the
shipped NTFS backend does not.

### Simulator (any platform)

```powershell
auralite-cli defrag --sim tests\scenarios\SC-01.json
auralite-cli defrag --sim tests\scenarios\SC-14.json    # SSD refusal demo → exit 12
```

---

## Documented vs actual

The previous version of this file listed exit code **10** for "elevation
required" and **30** for "invalid arguments". Neither exists in the
implementation: elevation returns **13** and argument errors return **2**
(`error_catalog.cpp`). It also omitted code **1** and code **3**, and did not
mention `--exclude`, `--config`, `--log-level` or `--confirm-mft`. The table
above is the corrected one.

This is worth flagging rather than silently fixing: a script written against the
old table would have branched on an exit code the program can never produce, so
its error handling would never have run.
