# ADR-003: Dual-Slot Atomic Binary Journal with CRC32C and Verify-Before-Retry

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** `BinaryJournal` writes two alternating 512-byte `DiskHeaderSlot` headers at offsets `0` and `512` followed by 96-byte `DiskRecord` entries from offset `1024`, each protected by Castagnoli CRC32C and bound to `volumeSerial` and `planHash`.
- **Recovery:** On resume, `Recover()` picks the valid header slot with highest `generation`, validates records sequentially, truncates torn trailing bytes, and `OnlineMode` runs `VerifyMovedExtent()` on any `MOVE_INTENT` lacking a matching `MOVE_DONE` before retrying.
