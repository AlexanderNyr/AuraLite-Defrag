# ADR-003: Dual-Slot Atomic Binary Journal with CRC32C and Verify-Before-Retry

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** `BinaryJournal` writes two alternating 512-byte `DiskHeaderSlot` headers at offsets `0` and `512` followed by 96-byte `DiskRecord` entries from offset `1024`, each protected by Castagnoli CRC32C and bound to `volumeSerial` and `planHash`.
- **Recovery:** On resume, `Recover()` picks the valid header slot with highest `generation`, validates records sequentially, truncates torn trailing bytes, and `OnlineMode` runs `VerifyMovedExtent()` on any `MOVE_INTENT` lacking a matching `MOVE_DONE` before retrying.
## Context

A crash in the middle of a cluster move must not leave the volume in a state
that requires repair, and must not silently lose the record of what was moved.
The journal is written on the hot path — once per move — so its format is a
performance decision as well as a durability one.

## Consequences

- Two header slots mean a torn header write cannot destroy the journal: the
  older, intact slot keeps the last generation.
- Recovery never *replays blindly*. A `MOVE_INTENT` without a matching
  `MOVE_DONE` is ambiguous, so the extent is re-read and the move is either
  completed or retried. This is why `JournalRecordType::StepBegin` does not
  exist — the engine journals the pair and advances the step only on
  `MOVE_DONE`.
- `opId` is `planHash ^ volumeSerialNumber`, so a journal from another volume or
  another plan is rejected (`E_JOURNAL_SERIAL_MISMATCH`,
  `E_JOURNAL_PLAN_HASH_MISMATCH`) rather than misinterpreted.
- Records are fixed at 96 bytes with explicit type, length and CRC32C. A record
  this project cannot parse is a bug, not an input to guess at.
- **A real bug was found by this design**: `OpenOrCreate` after `Recover()`
  failed with `E_JOURNAL_IO` (6003) because the previous handle was still open.
  Fixed by flushing and closing before re-opening.

## Consequences accepted

JSON was rejected for the hot path: parsing cost and the impossibility of a torn
write being detectable at record granularity. A sidecar JSON *summary* for
diagnostics is acceptable and does not conflict with this decision.

## Verification

200 abrupt-termination trials, 200/200 recovered, worst 38.2 ms, mean 3.1 ms,
0 silent corruption (`docs/dev/CRASH_RECOVERY.md`). **Not verified against real
power loss on real NTFS** — see V-20.
