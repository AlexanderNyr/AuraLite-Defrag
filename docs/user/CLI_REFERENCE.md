# AuraLite-Defrag CLI Reference

```
auralite-cli <verb> [options]
```

## Verbs
`analyze`, `defrag`, `quick`, `free-space`, `move-large`, `file`, `folder`, `mft`, `status`, `export`

## Options
- `--volume <drive>` — Target NTFS volume (e.g., `D:` or `\\?\Volume{...}\`)
- `--target <path>` — Target file or folder for `file`/`folder` verbs
- `--sim <scenario.json>` — Run deterministically against `EmulatorBackend` (no real disk access)
- `--dry-run` — Compute and preview plan without executing moves
- `--mft-tier <1|2|3>` — Select MFT optimization tier (default `3`, report-only)
- `--confirm-mft` — Required explicit confirmation for Tier 1 direct `$MFT` move
- `--export-csv <path>` — Atomically write RFC 4180 UTF-8 BOM CSV report (formula-injection safe)
- `--format <text|json>` — Output format (default `text`)
- `--journal <path>` — Custom binary journal path

## Exit Codes
- `0` — Success
- `10` — Elevation required (`E_PREFLIGHT_NOT_ELEVATED`)
- `11` — Pre-flight failure (dirty / read-only / BitLocker / unsupported FS / `<5%` free)
- `12` — SSD / Unknown media blocked (`E_PREFLIGHT_SSD_BLOCKED`)
- `20` — Verification / I/O error
- `21` — Journal recovery / binding error
- `30` — Invalid arguments / schema / policy violation
