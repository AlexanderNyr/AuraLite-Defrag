# Crash / power-loss behaviour and recovery SLA (P9-T03)

Covers **P9-T03** (R-079, R-083).


## A journal from an earlier operation

The journal file lives in `%TEMP%` under a per-volume name, and until recently it
was never cleaned up, so the *second* operation on a volume found a file whose
plan hash could not match and refused to start with
`E_JOURNAL_PLAN_HASH_MISMATCH` — before touching a single cluster. On a real
machine that reads as "the defragmenter stopped working after the first run".

`OpenOrCreate` now distinguishes the two cases it used to conflate:

| Existing journal | Behaviour |
|---|---|
| same volume serial **and** same plan hash | resume; this is the crash-recovery path and stays strict |
| different plan, or different volume serial | the old file is **renamed to `<name>.prev`** and a new operation starts |

Nothing is deleted: a `.prev` file is the only record of what the previous run
did, and a resume that still wants it never takes the rotation branch. The CLI
prints one line when this happens, because silently moving the evidence aside
would be the same mistake in the other direction.

## What is actually tested

The existing `TestJournalCrashRecovery200Trials` in `tests/unit/test_main.cpp`
injects *simulated* faults in-process: it stops the executor early and then
corrupts the journal file by hand. That is a useful test, but it cannot say
anything about durability, because the process never dies — the page cache,
the standard-library buffers and the process heap all survive, and the
"crash" loses nothing that a real crash would lose.

`tests/crash/` runs a genuinely separate process and kills it with `_exit()`,
so the kernel really does discard everything the process had buffered. That is
the strongest thing a userspace test can do short of pulling a disk.

| File | Role |
|---|---|
| `crash_channel.hpp` | the parent↔child event protocol |
| `crash_child.cpp` | executes the plan step by step, commits, and dies on command |
| `crash_campaign.cpp` | drives 5 crash points × N trials, verifies and resumes |

`make crash-campaign` builds all three and runs 200 trials.

## The five crash points

| Point | Where the process dies | What it probes |
|---|---|---|
| `BEFORE_FIRST_STEP` | journal created, no work done | empty-but-valid journal recovery |
| `MID_STEP_INTENT` | after `MoveIntent`, before `MoveDone` | a torn step must not count as applied |
| `AFTER_STEP_DONE` | after `MoveDone`, before commit | in-flight record with no published checkpoint |
| `AFTER_COMMIT` | immediately after `Commit()` returned | **the central case** — a committed step must never be lost |
| `MID_CHUNK` | partway through a run of steps | partial batch |

## Results

200 trials, 40 per crash point, on SC-01 (960-step plan):

| Metric | Result | Plan criterion |
|---|---|---|
| Trials | 200 | 200 |
| Resumed from journal | **200 / 200** | — |
| Committed promise kept | **200 / 200** | no lost committed step |
| Content intact (0 bad CRC) | **200 / 200** | (1) 0 silent corruption |
| Completed or clean abort | **200 / 200** | (2) 100% |
| Recovery, worst | **38 ms** | (4) ≤ 10 s |
| Recovery, mean | **3.1 ms** | (4) ≤ 10 s |
| `chkdsk` | **SKIPPED** | (3) 0 errors |

Criterion (3) is a Windows volume check (`chkdsk /f` on a mounted VHDX) and is
reported as SKIPPED, never as PASS. It is covered by P3-T13 / IT-07 on a real
volume; claiming it here would be a false pass.

The CRC check is deliberately implemented independently in the campaign rather
than calling the product's `ComputeCrc32c`: a verifier that reuses the writer's
own routine cannot catch a bug in that routine.

## A real product bug this campaign found

`BinaryJournal::Recover()` reopens the file stream for append so the operation
can continue. `BinaryJournal::OpenOrCreate()` then tried to open the same stream
a second time:

```cpp
stream_.open(filePath_, std::ios::binary | std::ios::in | std::ios::out);
```

`fstream::open` fails on an already-open stream. Any caller that did
`Recover()` → `OpenOrCreate()` — the natural resume sequence — got
`E_JOURNAL_IO` with an empty diagnostic. Fixed by flushing and closing the
stream first.

`OperationController::ResumeOperation` did not hit this because it passes the
recovered records straight to `ExecuteWithOutcome`, which skips `OpenOrCreate`.
The bug was latent, not live — but "latent" here means any future resume path
that opened the journal explicitly would have failed, and the error message
would have pointed nowhere.

A second, self-inflicted bug is worth recording because it would have made the
campaign meaningless: the child used `planHash` as the journal `opId`, while the
product uses `planHash ^ volumeSerialNumber` (`online_mode.cpp`). The journal
opened fine on its own terms, so the campaign was recovering a journal whose
identity did not match what the product writes.

## Power-loss behaviour analysis

What the journal guarantees, and what it does not:

**Guaranteed.** A `Commit()` that returned success has written a `Checkpoint`
record and the alternating header slot, and flushed both. Recovery reads the
newer valid header slot and takes `lastCompletedStep` from it. A step is counted
as applied only when its `MoveDone` record is present, so an intent without a
completion is re-executed rather than assumed done.

**Guaranteed.** A torn tail is truncated at the last record with a valid CRC
(`Recover()` calls `resize_file` to `validEnd`). A half-written record therefore
degrades to "that step did not happen", never to "that step happened twice".

**Not guaranteed, and cannot be with this design.** The volume itself is the
authority on whether a cluster move reached the platter. `FSCTL_MOVE_FILE` is
journaled by NTFS, so a torn move is rolled back by the filesystem, but the
journal cannot prove it. This is why the campaign's content check is "the
journal is self-consistent and never claims more than was committed", not "the
disk image is byte-identical to a clean run" — the latter would need a real
volume and a real detach.

**Limitation, as the plan notes.** A process kill is not a power loss. Pulling
power can also lose OS page-cache writes that `fsync` claimed were durable,
depending on drive write caching. `FlushStream()` on POSIX calls `fsync`; on
Windows the equivalent needs `FlushFileBuffers`, which the Win32 path does not
yet call — the current implementation relies on `fstream::flush` only. **That is
a real gap and is listed below as open work.**

**Recovery SLA.** Measured worst case 38 ms for a 960-step plan on this
hardware; the plan budgets 10 s on RH-1 with a real volume, which allows for
disk seek time on the header read and journal scan. The algorithmic part is
O(records), and the scan is the only unbounded term: a journal with 100k records
would take proportionally longer. At the measured ~0.4 µs per 96-byte record,
100k records is ~40 ms, so the 10 s budget has three orders of magnitude of
headroom before the scan itself matters.

## Still open

* **`FlushFileBuffers` on the Windows path.** `FlushStream()` has a POSIX
  `fsync` branch and a `_WIN32` branch that does nothing beyond
  `fstream::flush`. Without `FlushFileBuffers` a commit is not durable against
  power loss on Windows. This is the one finding that must be fixed before the
  durability claim is true on the target platform; it cannot be tested here.
* **`chkdsk` criterion (3)** needs a real volume — P3-T13 / IT-07.
* **VHDX detach** as a power-loss proxy is documented in the plan as a known
  approximation (risk noted at S-16); not attempted here.
* The campaign drives the journal directly rather than through `OnlineMode`,
  because `OnlineMode` applies moves to a live volume. A Windows run against a
  VHDX would exercise the full path including the move loop.
