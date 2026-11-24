#include "backend/ntfs/ntfs_backend.hpp"
#include "core/error_catalog.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstring>
#endif
#include <string_view>

// Deliberately outside the _WIN32 guard: the record walk below is shared with the
// Linux test suite and the P9-T01 fuzzer, which is the whole point of extracting it.
#include "backend/usn/usn_parser.hpp"

namespace auralite {

RealNtfsBackend::RealNtfsBackend(VolumeId volumeId)
    : volumeId_(std::move(volumeId)) {}

CapabilityBits RealNtfsBackend::Capabilities() const {
    CapabilityBits caps{};
    caps.supportsMftAnalysis = true;
    caps.supportsOnlineMetadataMove = false; // Conservative default pending S-01 runtime matrix
    caps.supportsDirectMftMove = false;
    caps.supportsSparseAndCompressed = true;
    caps.moveGranularityClusters = 1;
    caps.sparseMoveGranularityClusters = 16;
    caps.maxMoveClustersPerCall = 65536;
    caps.bootTimeAvailable = false;
    return caps;
}

#ifndef _WIN32

Result<VolumeGeometry> RealNtfsBackend::Geometry() {
    return Result<VolumeGeometry>::Failure(MakeError(ErrorCode::E_PREFLIGHT_VOLUME_INACCESSIBLE, volumeId_));
}
MediaClass RealNtfsBackend::Media() { return MediaClass::Unknown; }
MediaEvidence RealNtfsBackend::Evidence() { return {}; }
Result<PreflightResult> RealNtfsBackend::Preflight(bool, double) {
    return Result<PreflightResult>::Failure(MakeError(ErrorCode::E_PREFLIGHT_VOLUME_INACCESSIBLE, volumeId_));
}
Result<BitmapSnapshot> RealNtfsBackend::ReadBitmap(Lcn, ClusterCount) {
    return Result<BitmapSnapshot>::Failure(MakeError(ErrorCode::E_BITMAP_IO, volumeId_));
}
Result<std::vector<FileEntry>> RealNtfsBackend::Enumerate(std::uint64_t) {
    return Result<std::vector<FileEntry>>::Failure(MakeError(ErrorCode::E_ENUM_IO, volumeId_));
}
Result<std::vector<Extent>> RealNtfsBackend::Extents(FileId fileId) {
    return Result<std::vector<Extent>>::Failure(MakeError(ErrorCode::E_ENUM_IO, volumeId_, fileId));
}
Result<FileFlags> RealNtfsBackend::Flags(FileId fileId) {
    return Result<FileFlags>::Failure(MakeError(ErrorCode::E_ENUM_IO, volumeId_, fileId));
}
MoveResult RealNtfsBackend::MoveExtent(FileId, Vcn, Lcn, std::uint32_t) {
    return {false, 0, 50, static_cast<std::uint32_t>(ErrorCode::E_MOVE_UNSUPPORTED_STREAM), false, SkipReason::ReadOnlyOrPreFlightBlocked};
}
Result<MftInfo> RealNtfsBackend::Mft() {
    return Result<MftInfo>::Failure(MakeError(ErrorCode::E_ENUM_IO, volumeId_));
}
Result<MftZoneInfo> RealNtfsBackend::Zone() {
    return Result<MftZoneInfo>::Failure(MakeError(ErrorCode::E_ENUM_IO, volumeId_));
}
Result<std::uint64_t> RealNtfsBackend::ComputeWindowChecksum(FileId fileId) {
    return Result<std::uint64_t>::Failure(MakeError(ErrorCode::E_VERIFY_READBACK_FAILED, volumeId_, fileId));
}

#else

std::wstring RealNtfsBackend::VolumeDevicePath() const {
    std::wstring w = Win32Platform::Utf8ToWide(volumeId_);
    if (!w.empty() && w.back() == L'\\') w.pop_back();
    if (w.size() == 2 && w[1] == L':') return L"\\\\.\\" + w;
    if (w.starts_with(L"\\\\.\\") || w.starts_with(L"\\\\?\\")) return w;
    return L"\\\\.\\" + w;
}

std::wstring RealNtfsBackend::VolumeRootPath() const {
    std::wstring w = Win32Platform::Utf8ToWide(volumeId_);
    if (!w.empty() && w.back() != L'\\') w.push_back(L'\\');
    return w;
}

UniqueHandle RealNtfsBackend::OpenVolumeHandle(DWORD desiredAccess) const {
    return UniqueHandle(::CreateFileW(VolumeDevicePath().c_str(),
                                      desiredAccess,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS,
                                      nullptr));
}

UniqueHandle RealNtfsBackend::OpenVolumeForFsctl() const {
    // Every FSCTL this backend issues - FSCTL_GET_NTFS_VOLUME_DATA,
    // FSCTL_GET_VOLUME_BITMAP, FSCTL_GET_RETRIEVAL_POINTERS,
    // FSCTL_MARK_HANDLE - requires FILE_READ_DATA on the volume handle.
    // FILE_READ_ATTRIBUTES is not a substitute: the ioctl fails with
    // ERROR_ACCESS_DENIED, which used to be misreported as "unsupported
    // filesystem" and "bitmap I/O error" on perfectly good volumes.
    //
    // If FILE_READ_DATA is refused the process is not elevated. In that case the
    // attribute-only handle is opened anyway, so the caller fails on the ioctl
    // and reports E_PREFLIGHT_NOT_ELEVATED rather than a filesystem error.
    UniqueHandle h = OpenVolumeHandle(FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    if (!h.Valid() && ::GetLastError() == ERROR_ACCESS_DENIED) {
        h = OpenVolumeHandle(FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    }
    return h;
}

UniqueHandle RealNtfsBackend::OpenFileByReference(FileId fileId, DWORD desiredAccess) const {
    UniqueHandle hVol = OpenVolumeForFsctl();
    if (!hVol.Valid()) return UniqueHandle{};

    FILE_ID_DESCRIPTOR desc{};
    desc.dwSize = sizeof(desc);
    desc.Type = FileIdType;
    desc.FileId.QuadPart = static_cast<LONGLONG>(fileId);

    HANDLE hFile = ::OpenFileById(hVol.Get(), &desc, desiredAccess,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, FILE_FLAG_BACKUP_SEMANTICS);
    if (hFile != INVALID_HANDLE_VALUE) {
        // Protect our own handle from external defrag interference (D-01)
        MARK_HANDLE_INFO mhi{};
        mhi.HandleInfo = MARK_HANDLE_PROTECT_CLUSTERS;
        DWORD ret = 0;
        ::DeviceIoControl(hFile, FSCTL_MARK_HANDLE, &mhi, sizeof(mhi), nullptr, 0, &ret, nullptr);
    }
    return UniqueHandle(hFile);
}

MediaClass RealNtfsBackend::Media() {
    return mediaDetector_.Classify(volumeId_);
}

MediaEvidence RealNtfsBackend::Evidence() {
    return mediaDetector_.Detail();
}

Result<VolumeGeometry> RealNtfsBackend::Geometry() {
    UniqueHandle hVol = OpenVolumeForFsctl();
    if (!hVol.Valid()) {
        return Result<VolumeGeometry>::Failure(
            MakeError(ErrorCode::E_PREFLIGHT_VOLUME_INACCESSIBLE, volumeId_, 0, ::GetLastError()));
    }

    struct ExtendedNtfsBuf {
        NTFS_VOLUME_DATA_BUFFER base;
        NTFS_EXTENDED_VOLUME_DATA ext;
    } buf{};

    DWORD bytesRet = 0;
    if (!::DeviceIoControl(hVol.Get(), FSCTL_GET_NTFS_VOLUME_DATA,
                           nullptr, 0, &buf, sizeof(buf), &bytesRet, nullptr)) {
        // ERROR_ACCESS_DENIED here means the handle lacks FILE_READ_DATA, which
        // is a privilege problem, not a filesystem problem.
        const DWORD err = ::GetLastError();
        const ErrorCode code = (err == ERROR_ACCESS_DENIED)
                                   ? ErrorCode::E_PREFLIGHT_NOT_ELEVATED
                                   : ErrorCode::E_PREFLIGHT_UNSUPPORTED_FS;
        // Record why, so the caller can print it instead of a bare error code.
        mediaDetector_.NoteProbe("GeometryFailed err=" + std::to_string(err) +
                                 (err == ERROR_ACCESS_DENIED ? " (needs administrator)" : ""));
        return Result<VolumeGeometry>::Failure(
            MakeError(code, volumeId_, static_cast<std::int64_t>(err), err));
    }

    VolumeGeometry g{};
    g.volumeSerialNumber = static_cast<std::uint64_t>(buf.base.VolumeSerialNumber.QuadPart);
    g.numberSectors = static_cast<std::uint64_t>(buf.base.NumberSectors.QuadPart);
    g.totalClusters = static_cast<ClusterCount>(buf.base.TotalClusters.QuadPart);
    g.freeClusters = static_cast<ClusterCount>(buf.base.FreeClusters.QuadPart);
    g.totalReserved = static_cast<ClusterCount>(buf.base.TotalReserved.QuadPart);
    g.bytesPerSector = buf.base.BytesPerSector;
    g.bytesPerCluster = buf.base.BytesPerCluster;
    g.bytesPerFileRecordSegment = buf.base.BytesPerFileRecordSegment;
    g.clustersPerFileRecordSegment = buf.base.ClustersPerFileRecordSegment;
    g.mftValidDataLength = static_cast<std::uint64_t>(buf.base.MftValidDataLength.QuadPart);
    g.mftStartLcn = static_cast<Lcn>(buf.base.MftStartLcn.QuadPart);
    g.mft2StartLcn = static_cast<Lcn>(buf.base.Mft2StartLcn.QuadPart);
    g.mftZoneStart = static_cast<Lcn>(buf.base.MftZoneStart.QuadPart);
    g.mftZoneEnd = static_cast<Lcn>(buf.base.MftZoneEnd.QuadPart);
    if (bytesRet >= sizeof(ExtendedNtfsBuf)) {
        g.majorVersion = buf.ext.MajorVersion;
        g.minorVersion = buf.ext.MinorVersion;
    }

    wchar_t label[MAX_PATH]{};
    wchar_t fsName[MAX_PATH]{};
    DWORD serial = 0, maxComp = 0, flags = 0;
    if (::GetVolumeInformationW(VolumeRootPath().c_str(), label, MAX_PATH, &serial, &maxComp, &flags, fsName, MAX_PATH)) {
        g.label = Win32Platform::WideToUtf8(label);
        g.fsName = Win32Platform::WideToUtf8(fsName);
    }
    g.driveLetter = volumeId_;
    cachedGeom_ = g;
    hasCachedGeom_ = true;
    return Result<VolumeGeometry>::Success(std::move(g));
}

Result<PreflightResult> RealNtfsBackend::Preflight(bool isElevated, double fullDefragThresholdPct) {
    PreflightResult pf{};
    pf.mediaClass = Media();
    pf.evidence = Evidence();

    wchar_t fsName[MAX_PATH]{};
    DWORD flags = 0;
    if (!::GetVolumeInformationW(VolumeRootPath().c_str(), nullptr, 0, nullptr, nullptr, &flags, fsName, MAX_PATH)) {
        const DWORD err = ::GetLastError();
        pf.passed = false;
        pf.status = (err == ERROR_ACCESS_DENIED) ? PreflightStatus::BitLockerLocked : PreflightStatus::VolumeInaccessible;
        pf.messageKey = (pf.status == PreflightStatus::BitLockerLocked) ? "IDS_PREFLIGHT_BITLOCKER" : "IDS_PREFLIGHT_FAILED_GENERIC";
        return Result<PreflightResult>::Success(pf);
    }

    if (std::wstring(fsName) != L"NTFS") {
        pf.passed = false;
        pf.status = PreflightStatus::UnsupportedFileSystem;
        pf.messageKey = "IDS_PREFLIGHT_UNSUPPORTED_FS";
        return Result<PreflightResult>::Success(pf);
    }

    if (IsDefragBlockedMedia(pf.mediaClass)) {
        pf.passed = false;
        pf.status = PreflightStatus::SolidStateBlocked;
        pf.messageKey = "IDS_SSD_BLOCKED_BODY";
        return Result<PreflightResult>::Success(pf);
    }

    if ((flags & FILE_READ_ONLY_VOLUME) != 0) {
        pf.passed = false;
        pf.status = PreflightStatus::ReadOnlyVolume;
        pf.messageKey = "IDS_PREFLIGHT_READ_ONLY";
        return Result<PreflightResult>::Success(pf);
    }

    if (!isElevated) {
        pf.passed = false;
        pf.status = PreflightStatus::NotElevated;
        pf.messageKey = "IDS_ELEVATION_REQUIRED";
        return Result<PreflightResult>::Success(pf);
    }

    UniqueHandle hVol = OpenVolumeHandle(FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    if (!hVol.Valid()) {
        pf.passed = false;
        pf.status = PreflightStatus::VolumeInaccessible;
        pf.messageKey = "IDS_PREFLIGHT_FAILED_GENERIC";
        return Result<PreflightResult>::Success(pf);
    }

    // Dirty bit probe (D-15 / S-15)
    DWORD dirtyFlags = 0;
    DWORD dirtyRet = 0;
    if (::DeviceIoControl(hVol.Get(), FSCTL_IS_VOLUME_DIRTY, nullptr, 0,
                          &dirtyFlags, sizeof(dirtyFlags), &dirtyRet, nullptr)) {
        if ((dirtyFlags & VOLUME_IS_DIRTY) != 0) {
            pf.passed = false;
            pf.status = PreflightStatus::DirtyVolume;
            pf.messageKey = "IDS_DIRTY_VOLUME";
            return Result<PreflightResult>::Success(pf);
        }
    }

    auto geomRes = Geometry();
    if (!geomRes) {
        pf.passed = false;
        pf.status = PreflightStatus::VolumeInaccessible;
        pf.messageKey = "IDS_PREFLIGHT_FAILED_GENERIC";
        return Result<PreflightResult>::Success(pf);
    }

    const auto& g = geomRes.Value();
    pf.freePercent = g.totalClusters > 0
                         ? (100.0 * static_cast<double>(g.freeClusters) / static_cast<double>(g.totalClusters))
                         : 0.0;

    if (pf.freePercent < 5.0) {
        pf.passed = false;
        pf.status = PreflightStatus::InsufficientFreeSpace;
        pf.messageKey = "IDS_PREFLIGHT_FAILED_GENERIC";
        return Result<PreflightResult>::Success(pf);
    }
    if (pf.freePercent < fullDefragThresholdPct) {
        pf.passed = true;
        pf.quickOnly = true;
        pf.status = PreflightStatus::QuickOnlyLowSpace;
        pf.messageKey = "IDS_STATE_IDLE";
        return Result<PreflightResult>::Success(pf);
    }

    pf.passed = true;
    pf.status = PreflightStatus::Clean;
    pf.messageKey = "IDS_STATE_IDLE";
    return Result<PreflightResult>::Success(pf);
}

Result<BitmapSnapshot> RealNtfsBackend::ReadBitmap(Lcn start, ClusterCount count) {
    // FSCTL_GET_VOLUME_BITMAP requires FILE_READ_DATA. Opening with
    // FILE_READ_ATTRIBUTES alone made the very first step of analysis fail with
    // E_BITMAP_IO on a healthy NTFS volume.
    UniqueHandle hVol = OpenVolumeForFsctl();
    if (!hVol.Valid()) {
        return Result<BitmapSnapshot>::Failure(MakeError(ErrorCode::E_BITMAP_IO, volumeId_, 0, ::GetLastError()));
    }

    BitmapSnapshot out{};
    out.startLcn = start;
    out.totalClusters = count;
    out.generation = 1;
    out.bits.assign(static_cast<std::size_t>((count + 7ULL) / 8ULL), 0U);

    // Chunked read in 16 MiB bitmap buffer increments (S-05)
    constexpr std::size_t kChunkBytes = 16U * 1024U * 1024U;
    std::vector<BYTE> buf(sizeof(VOLUME_BITMAP_BUFFER) + kChunkBytes);
    Lcn cursor = start;
    ClusterCount remaining = count;

    while (remaining > 0) {
        STARTING_LCN_INPUT_BUFFER in{};
        in.StartingLcn.QuadPart = static_cast<LONGLONG>(cursor);
        DWORD bytesRet = 0;
        BOOL ok = ::DeviceIoControl(hVol.Get(), FSCTL_GET_VOLUME_BITMAP,
                                    &in, sizeof(in), buf.data(), static_cast<DWORD>(buf.size()),
                                    &bytesRet, nullptr);
        DWORD err = ::GetLastError();
        if (!ok && err != ERROR_MORE_DATA) {
            return Result<BitmapSnapshot>::Failure(MakeError(ErrorCode::E_BITMAP_IO, volumeId_, 0, err));
        }

        const auto* vbb = reinterpret_cast<const VOLUME_BITMAP_BUFFER*>(buf.data());
        const Lcn returnedStart = static_cast<Lcn>(vbb->StartingLcn.QuadPart);
        const ClusterCount returnedSize = static_cast<ClusterCount>(vbb->BitmapSize.QuadPart);
        if (returnedSize == 0) break;

        // Mask off leading rounded clusters (D-04 / S-05)
        const ClusterCount leadSkip = cursor > returnedStart ? (cursor - returnedStart) : 0;
        const ClusterCount availAfterLead = returnedSize > leadSkip ? (returnedSize - leadSkip) : 0;
        const ClusterCount take = std::min(remaining, availAfterLead);

        for (ClusterCount i = 0; i < take; ++i) {
            const ClusterCount srcBit = leadSkip + i;
            const bool isSet = ((vbb->Buffer[srcBit >> 3] >> (srcBit & 7U)) & 1U) != 0;
            if (isSet) {
                out.SetAllocated(cursor + i, true);
            }
        }
        if (take == 0) break;
        cursor += take;
        remaining -= take;
        if (ok) break;
    }

    return Result<BitmapSnapshot>::Success(std::move(out));
}

Result<std::vector<Extent>> RealNtfsBackend::ReadRetrievalPointers(HANDLE fileHandle) const {
    std::vector<Extent> out;
    std::vector<BYTE> buf(64U * 1024U);
    STARTING_VCN_INPUT_BUFFER in{};
    in.StartingVcn.QuadPart = 0;

    while (true) {
        DWORD bytesRet = 0;
        BOOL ok = ::DeviceIoControl(fileHandle, FSCTL_GET_RETRIEVAL_POINTERS,
                                    &in, sizeof(in), buf.data(), static_cast<DWORD>(buf.size()),
                                    &bytesRet, nullptr);
        DWORD err = ::GetLastError();
        if (!ok && err == ERROR_HANDLE_EOF) {
            // Resident file or no extents
            return Result<std::vector<Extent>>::Success(std::move(out));
        }
        if (!ok && err != ERROR_MORE_DATA) {
            return Result<std::vector<Extent>>::Failure(MakeError(ErrorCode::E_ENUM_IO, volumeId_, 0, err));
        }

        const auto* rpb = reinterpret_cast<const RETRIEVAL_POINTERS_BUFFER*>(buf.data());
        Vcn prevVcn = static_cast<Vcn>(rpb->StartingVcn.QuadPart);
        for (DWORD i = 0; i < rpb->ExtentCount; ++i) {
            const Vcn nextVcn = static_cast<Vcn>(rpb->Extents[i].NextVcn.QuadPart);
            const Lcn lcn = static_cast<Lcn>(rpb->Extents[i].Lcn.QuadPart);
            if (rpb->Extents[i].Lcn.QuadPart != -1 && nextVcn > prevVcn) {
                out.push_back({prevVcn, lcn, nextVcn - prevVcn});
            }
            prevVcn = nextVcn;
        }
        if (ok) break;
        in.StartingVcn.QuadPart = static_cast<LONGLONG>(prevVcn);
    }
    return Result<std::vector<Extent>>::Success(std::move(out));
}

Result<std::vector<FileEntry>> RealNtfsBackend::Enumerate(std::uint64_t maxFiles) {
    UniqueHandle hVol = OpenVolumeHandle(FILE_READ_ATTRIBUTES | FILE_READ_DATA);
    if (!hVol.Valid()) {
        return Result<std::vector<FileEntry>>::Failure(MakeError(ErrorCode::E_ENUM_IO, volumeId_, 0, ::GetLastError()));
    }

    MFT_ENUM_DATA_V0 med{};
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = MAXLONGLONG;

    std::vector<BYTE> buf(256U * 1024U);
    std::vector<FileEntry> entries;
    pathCache_.clear();
    flagsCache_.clear();

    while (entries.size() < maxFiles) {
        DWORD bytesRet = 0;
        BOOL ok = ::DeviceIoControl(hVol.Get(), FSCTL_ENUM_USN_DATA,
                                    &med, sizeof(med), buf.data(), static_cast<DWORD>(buf.size()),
                                    &bytesRet, nullptr);
        if (!ok) {
            DWORD err = ::GetLastError();
            if (err == ERROR_HANDLE_EOF) break;
            return Result<std::vector<FileEntry>>::Failure(MakeError(ErrorCode::E_ENUM_IO, volumeId_, 0, err));
        }
        if (bytesRet < sizeof(USN)) break;

        // The buffer is walked by the shared, fuzzed parser instead of by inline
        // pointer arithmetic. The data comes from the OS, but a corrupt or
        // hostile volume can still make its embedded lengths lie, and this is the
        // component that has been exercised with a million malformed inputs.
        const std::string_view raw(reinterpret_cast<const char*>(buf.data()), bytesRet);
        auto parsed = ParseUsnBuffer(raw, maxFiles > entries.size() ? maxFiles - entries.size() : 0);
        if (!parsed.Ok()) {
            return Result<std::vector<FileEntry>>::Failure(parsed.Error());
        }
        med.StartFileReferenceNumber = parsed.Value().nextStartFileReferenceNumber;

        for (const auto& rec : parsed.Value().records) {
            if (entries.size() >= maxFiles) break;

            FileEntry fe{};
            fe.id = static_cast<FileId>(rec.fileReferenceNumber & 0x0000FFFFFFFFFFFFULL);
            fe.name = rec.name;
            if (fe.name.empty()) continue;

            fe.flags.isDirectory = (rec.fileAttributes & kFileAttributeDirectory) != 0;
            fe.flags.compressed = (rec.fileAttributes & kFileAttributeCompressed) != 0;
            fe.flags.sparse = (rec.fileAttributes & kFileAttributeSparseFile) != 0;
            fe.flags.encrypted = (rec.fileAttributes & kFileAttributeEncrypted) != 0;
            fe.flags.reparsePoint = (rec.fileAttributes & kFileAttributeReparsePoint) != 0;
            fe.flags.isMetadata = (!fe.name.empty() && fe.name[0] == '$');

            if (!fe.flags.isDirectory) {
                // FILE_READ_DATA, not FILE_READ_ATTRIBUTES: FSCTL_GET_RETRIEVAL_POINTERS
                // is issued on this handle and refuses to run without it.
                UniqueHandle hf = OpenFileByReference(fe.id, FILE_READ_ATTRIBUTES | FILE_READ_DATA);
                if (hf.Valid()) {
                    LARGE_INTEGER fsize{};
                    if (::GetFileSizeEx(hf.Get(), &fsize)) {
                        fe.sizeBytes = static_cast<std::uint64_t>(fsize.QuadPart);
                    }
                    wchar_t pathBuf[4096]{};
                    DWORD pLen = ::GetFinalPathNameByHandleW(hf.Get(), pathBuf, 4096, FILE_NAME_NORMALIZED);
                    if (pLen > 0 && pLen < 4096) {
                        fe.path = Win32Platform::WideToUtf8(std::wstring_view(pathBuf, pLen));
                    } else {
                        fe.path = "\\" + fe.name;
                    }
                    auto extRes = ReadRetrievalPointers(hf.Get());
                    if (extRes) {
                        fe.extents = std::move(extRes.Value());
                        if (fe.extents.empty()) {
                            fe.flags.resident = true;
                            fe.fragmentCount = 0;
                        } else {
                            fe.fragmentCount = static_cast<std::uint32_t>(fe.extents.size());
                            for (const auto& e : fe.extents) fe.allocatedClusters += e.clusterCount;
                        }
                    } else {
                        // The extent read failed. Leaving fragmentCount at 0 made
                        // the file indistinguishable from a contiguous one, so a
                        // volume of 160k unreadable files reported "0 fragmented".
                        // Count it as unknown and let the caller decide; never as
                        // "not fragmented".
                        fe.flags.isLocked = true;
                        ++extentReadFailures_;
                        if (extentReadFailures_ <= 3) {
                            mediaDetector_.NoteProbe("ExtentsFailed file=" + fe.name +
                                                     " err=" + std::to_string(extRes.Error().win32Error) +
                                                     " code=" + ErrorCodeName(extRes.Error().code));
                        }
                    }
                } else {
                    fe.flags.isLocked = true;
                    fe.path = "\\" + fe.name;
                }
            } else {
                fe.path = "\\" + fe.name;
            }

            pathCache_[fe.id] = fe.path;
            flagsCache_[fe.id] = fe.flags;
            entries.push_back(std::move(fe));
        }
    }

    if (extentReadFailures_ > 0) {
        mediaDetector_.NoteProbe("ExtentReadFailures total=" + std::to_string(extentReadFailures_) +
                                 " of " + std::to_string(entries.size()) +
                                 " (these files were NOT counted as fragmented)");
    }
    extentReadFailures_ = 0;
    return Result<std::vector<FileEntry>>::Success(std::move(entries));
}

Result<std::vector<Extent>> RealNtfsBackend::Extents(FileId fileId) {
    UniqueHandle hf = OpenFileByReference(fileId, FILE_READ_ATTRIBUTES);
    if (!hf.Valid()) {
        return Result<std::vector<Extent>>::Failure(MakeError(ErrorCode::E_MOVE_ACCESS_DENIED, volumeId_, fileId, ::GetLastError()));
    }
    return ReadRetrievalPointers(hf.Get());
}

Result<FileFlags> RealNtfsBackend::Flags(FileId fileId) {
    auto it = flagsCache_.find(fileId);
    if (it != flagsCache_.end()) return Result<FileFlags>::Success(it->second);
    return Result<FileFlags>::Success(FileFlags{});
}

MoveResult RealNtfsBackend::MoveExtent(FileId fileId, Vcn startVcn, Lcn targetLcn, std::uint32_t clusters) {
    std::lock_guard lock(moveMutex_);
    UniqueHandle hVol = OpenVolumeHandle(GENERIC_READ | GENERIC_WRITE);
    if (!hVol.Valid()) {
        return {false, 0, ::GetLastError(), static_cast<std::uint32_t>(ErrorCode::E_MOVE_ACCESS_DENIED), false, SkipReason::AccessDenied};
    }

    UniqueHandle hFile = OpenFileByReference(fileId, FILE_READ_ATTRIBUTES);
    if (!hFile.Valid()) {
        const DWORD err = ::GetLastError();
        return {false, 0, err, static_cast<std::uint32_t>(MapWin32ToMoveError(err)), false, SkipReason::AccessDenied};
    }

    MOVE_FILE_DATA mfd{};
    mfd.FileHandle = hFile.Get();
    mfd.StartingVcn.QuadPart = static_cast<LONGLONG>(startVcn);
    mfd.StartingLcn.QuadPart = static_cast<LONGLONG>(targetLcn);
    mfd.ClusterCount = clusters;

    DWORD bytesRet = 0;
    if (!::DeviceIoControl(hVol.Get(), FSCTL_MOVE_FILE, &mfd, sizeof(mfd), nullptr, 0, &bytesRet, nullptr)) {
        const DWORD err = ::GetLastError();
        const ErrorCode ec = MapWin32ToMoveError(err);
        return {false, 0, err, static_cast<std::uint32_t>(ec), false, DescribeError(ec).retryable ? SkipReason::StaleBitmapConflict : SkipReason::AccessDenied};
    }
    return {true, clusters, 0, static_cast<std::uint32_t>(ErrorCode::Ok), false, SkipReason::None};
}

Result<MftInfo> RealNtfsBackend::Mft() {
    auto gRes = Geometry();
    if (!gRes) return Result<MftInfo>::Failure(gRes.Error());
    const auto& g = gRes.Value();

    MftInfo info{};
    info.mftStartLcn = g.mftStartLcn;
    info.mft2StartLcn = g.mft2StartLcn;
    info.mftValidDataLength = g.mftValidDataLength;
    info.totalRecords = g.bytesPerFileRecordSegment > 0 ? (g.mftValidDataLength / g.bytesPerFileRecordSegment) : 0;
    info.mftMirrMovable = false;

    auto extRes = Extents(kMftFileId);
    if (extRes) {
        info.mftExtents = std::move(extRes.Value());
        info.fragmentCount = static_cast<std::uint32_t>(info.mftExtents.size());
    }
    return Result<MftInfo>::Success(std::move(info));
}

Result<MftZoneInfo> RealNtfsBackend::Zone() {
    auto gRes = Geometry();
    if (!gRes) return Result<MftZoneInfo>::Failure(gRes.Error());
    const auto& g = gRes.Value();

    MftZoneInfo z{};
    z.zoneStartLcn = g.mftZoneStart;
    z.zoneEndLcn = g.mftZoneEnd;
    z.totalZoneClusters = g.mftZoneEnd > g.mftZoneStart ? (g.mftZoneEnd - g.mftZoneStart) : 0;
    if (z.totalZoneClusters > 0) {
        auto bmp = ReadBitmap(z.zoneStartLcn, z.totalZoneClusters);
        if (bmp) {
            ClusterCount freeCount = 0;
            for (ClusterCount i = 0; i < z.totalZoneClusters; ++i) {
                if (!bmp.Value().IsAllocated(z.zoneStartLcn + i)) ++freeCount;
            }
            z.freeZoneClusters = freeCount;
        }
    }
    return Result<MftZoneInfo>::Success(z);
}

Result<std::uint64_t> RealNtfsBackend::ComputeWindowChecksum(FileId fileId) {
    UniqueHandle hf = OpenFileByReference(fileId, FILE_READ_DATA);
    if (!hf.Valid()) {
        return Result<std::uint64_t>::Failure(MakeError(ErrorCode::E_VERIFY_READBACK_FAILED, volumeId_, fileId, ::GetLastError()));
    }
    std::vector<BYTE> buf(1024U * 1024U);
    DWORD readBytes = 0;
    if (!::ReadFile(hf.Get(), buf.data(), static_cast<DWORD>(buf.size()), &readBytes, nullptr)) {
        return Result<std::uint64_t>::Failure(MakeError(ErrorCode::E_VERIFY_READBACK_FAILED, volumeId_, fileId, ::GetLastError()));
    }
    std::uint64_t h = 14695981039346656037ULL;
    for (DWORD i = 0; i < readBytes; ++i) {
        h ^= buf[i];
        h *= 1099511628211ULL;
    }
    return Result<std::uint64_t>::Success(h);
}

#endif

} // namespace auralite
