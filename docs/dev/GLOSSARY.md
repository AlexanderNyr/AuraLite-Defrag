# Glossary, Verification List and API Inventory (R-100)

## Terms

| Term | Meaning in this project |
|---|---|
| **Cluster** | The allocation unit of an NTFS volume. All planning is in clusters; byte sizes appear only in reports. |
| **LCN / VCN** | Logical / Virtual Cluster Number. A VCN is an offset inside a file; an LCN is an offset on the volume. A move changes LCNs, never VCNs. |
| **Extent** | A contiguous run of clusters `(vcn, lcn, count)` belonging to one file. Fragmentation is many short extents. |
| **Fragmented file** | A file with more than one non-contiguous extent. |
| **Volume fragmentation %** | Share of allocated clusters that belong to fragmented files. Not the same as "free space fragmentation". |
| **MFT** | Master File Table: the NTFS metadata file holding one record per file. Its own fragmentation is the tier-2/3 subject. |
| **MFT Zone** | A reserved region the filesystem uses to grow the MFT contiguously. Occupied by ordinary files over time, which is what tier 2 fixes. |
| **Tier 1/2/3/4** | MFT strategies: 1 = move `$MFT` directly (unreachable in v1), 2 = evacuate the zone, 3 = report only (**default**), 4 = hook documented for P10. |
| **Online defrag** | Moving clusters while the volume is mounted. The opposite of boot-time. |
| **Retrieval pointer** | The mapping from a VCN to an LCN. Re-read after every move as the integrity check. |
| **Journal** | The on-disk record of INTENT/MOVE/DONE steps, used to resume after a crash. Not the NTFS `$LogFile`. |
| **Checkpoint** | The last committed journal position, from which recovery resumes. |
| **Emulator** | The deterministic in-memory volume used as the test oracle. |
| **Golden trace** | A committed, byte-stable record of one emulator run, used to detect behaviour changes. |
| **Differential oracle** | The comparison of two runs (today: emulator vs emulator; eventually emulator vs real NTFS). |
| **Skip reason** | The typed reason a file was not moved: system, locked, in use, excluded, sparse, compressed, EFS, reparse point, insufficient space. |
| **Pre-flight** | The checks that run before the first write: dirty, read-only, BitLocker, unsupported FS, free space, media class. |
| **Policy gate** | The single component every entry point must pass through before writing. |
| **Fail closed** | On doubt, refuse. Unknown media is treated as solid-state. |
| **PMv2** | Per-monitor v2 DPI awareness. |
| **Exit code** | The CLI's numeric result: 0/1/2/3/11/12/13/20/21. |

## Consolidated [VERIFY] list (V-01…V-30)

Each item is something this project could not confirm from documentation and
must observe on real Windows. The "state" column is the truth as of this build.

| V | Question | State |
|---|---|---|
| V-01 | Per-call `ClusterCount` limit for `FSCTL_MOVE_FILE` | Not verified — conservative clamp used |
| V-02 | Behaviour with `$MFT::$DATA` in the stream list | Not verified |
| V-03 | **Can `$MFT` be moved online at all?** | Not verified — tier 3 default assumes no |
| V-04 | Does `FSCTL_MOVE_FILE` succeed on sparse/compressed files | Not verified — skipped by policy |
| V-05 | Resident-file move granularity | Modelled, not verified |
| V-06 | USN journal increment size on a busy volume | Not verified |
| V-07 | `FSCTL_GET_NTFS_FILE_RECORD` stability across writes | Not verified |
| V-08 | Multi-extent / tiered volumes | Not verified |
| V-09 | Virtual disk media class reporting | Not verified |
| V-10 | Direct2D frame time on a 64M-cluster map | Not verified |
| V-11 | WIC PNG metadata and DPI correctness | Not verified |
| V-12 | **PMv2 on Windows 10 1809** | Not verified — claim unproven |
| V-13 | Cluster map aggregation visual density | Not verified |
| V-14 | Shell verb behaviour on the Win11 modern menu | Not verified |
| V-15 | Defender filter-driver interference | Not verified |
| V-16 | **Can a hosted CI runner attach a VHDX?** | Not verified — blocks IT-01…IT-10 |
| V-17 | Task Scheduler `TASK_RUNLEVEL_HIGHEST` without a stored password | Not verified |
| V-18 | Reliable dirty-bit detection without VSS | Not verified |
| V-19 | BitLocker-locked detection | Not verified |
| V-20 | **`FSCTL_MOVE_FILE` atomicity, torn moves, `FlushFileBuffers` on the Windows path** | Not verified — the single remaining durability hole |
| V-21 | MFT zone reservation behaviour | Not verified |
| V-22 | Cross-version FSCTL differences (1809…24H2) | Not verified |
| V-23 | File-ID stability across a move | Assumed stable |
| V-24 | Enumeration completeness on 250k files | Not verified |
| V-25 | NTFS allocator heuristics (fidelity ceiling for the emulator) | Accepted as unknown |
| V-26 | Unsupported-filesystem detection | Not verified |
| V-27 | Read-only media detection | Not verified |
| V-28 | Volume extent layout on dynamic/spanned disks | Not verified |
| V-29 | Certificate availability for signing | Not verified |
| V-30 | Installer residue on uninstall | Not verified |

**Summary: 0 of 30 verified on real hardware.** Twenty-two are gated on a single
missing capability — an elevated Windows host with a scratch VHDX.

## API / FSCTL / IOCTL inventory

Everything the Windows backend is designed to call. Nothing here is exercised
on Linux; the list is the contract the backend must meet.

| API | Purpose | Wrapped by |
|---|---|---|
| `CreateFileW` | Volume and file handles | `platform/win32_handle`, `backend/ntfs` |
| `DeviceIoControl(FSCTL_GET_NTFS_VOLUME_DATA)` | Geometry: clusters, sectors, record size, serial | `RealNtfsBackend::Geometry` |
| `FSCTL_GET_VOLUME_BITMAP` | Cluster allocation bitmap, chunked | `RealNtfsBackend::Bitmap` |
| `FSCTL_GET_RETRIEVAL_POINTERS` | Extent map for a file | `RealNtfsBackend::Extents` |
| `FSCTL_MOVE_FILE` | **The only write operation** | `RealNtfsBackend::MoveFile` |
| `FSCTL_ENUM_USN_DATA` | Enumerate every file on the volume | `backend/usn` |
| `FSCTL_GET_NTFS_FILE_RECORD` | Per-file metadata (size, attributes, parent) | `backend/usn` |
| `FSCTL_IS_VOLUME_DIRTY` | Pre-flight: dirty bit | `PolicyGate` |
| `GetVolumeInformationByHandleW` | Pre-flight: filesystem, flags, read-only | `PolicyGate` |
| `GetVolumePathNamesForVolumeNameW` | Drive letter for a volume GUID | `platform/volume_enum` |
| `GetDiskFreeSpaceExW` | Free space for pre-flight | `PolicyGate` |
| `IOCTL_STORAGE_QUERY_PROPERTY` (`StorageDeviceSeekPenaltyProperty`, `StorageDeviceTrimProperty`) | Media classification | `RealMediaDetector` |
| `GetLogicalDrives` / `FindFirstVolumeW` / `FindNextVolumeW` | Volume enumeration | `platform/volume_enum` |
| `OpenProcessToken` / `GetTokenInformation` / `AllocateAndInitializeSid` | Elevation check | `platform/elevation` |
| `SetThreadDpiAwarenessContext`, `GetDpiForWindow` | DPI | `ui/` |
| `ITaskService` (COM) | Scheduler | `scheduler/` |
| `IExplorerCommand` (COM) | Context menu | `shell/` |
| WIC (`IWICImagingFactory`, `IWICBitmapEncoder`) | PNG export | `reporting/` |
| D2D/DWrite (`ID2D1Factory`, `ID2D1HwndRenderTarget`) | Rendering | `ui/` |

No undocumented API is called. No `NtQuery*` internals, no `\\Device\\Harddisk` raw
I/O, no filter-driver interaction.
