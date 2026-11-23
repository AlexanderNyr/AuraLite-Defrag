# Scenario JSON Schema (`tests/scenarios/scenario.schema.json`)

Simulation scenarios are strictly validated (`<= 1 MiB`, `<= 8` nesting levels, allowlisted keys only):
- `id`: Scenario identifier (`SC-01`..`SC-20`)
- `seed`: 64-bit unsigned deterministic PRNG seed
- `generator`: `G-1` (general fragmented), `G-2` (near-full packing), `G-3` (MFT zone occupancy), `G-4` (small/resident storm), `G-5` (large files), `G-6` (special NTFS flags)
- `volume`: `{ clusters, bytesPerCluster, freeTargetPct, fs, mediaClass, dirty, readOnly, bitLockerLocked }`
- `mft`: `{ fragments, zonePct, occupiedByUserFilesPct, recordCount, inUseRecords, supportsDirectMftMove }`
- `files`: `{ count, extentsPerFile, sizeMinClusters, sizeMaxClusters, residentPct, compressedCount, sparseCount, encryptedCount, reparseCount, lockedCount, adsCount, unicodeNames, longPaths }`
- `faults`: Fault injections (`IoErrorOnMove`, `IoErrorOnBitmap`, `LockedFile`, `FileVanished`, `InterruptionMidMove`, `PartialMove`)
