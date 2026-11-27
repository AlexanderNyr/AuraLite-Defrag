#include "backend/ntfs/ntfs_backend.hpp"
#include "core/error_catalog.hpp"
#include "backend/ntfs/mft/mft_scan.hpp"
#include "core/path_tree.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <thread>
#endif
#include <string_view>

// Deliberately outside the _WIN32 guard: the record walk below is shared with the
// Linux test suite and the P9-T01 fuzzer, which is the whole point of extracting it.
#include "backend/usn/usn_parser.hpp"

namespace auralite {
namespace {
// $MFT is record 0 with sequence number 1. The index alone is not a valid
// FileIdType descriptor; the pair is.
constexpr std::uint64_t kMftCanonicalReference = 1ULL << 48;
} // namespace

RealNtfsBackend::RealNtfsBackend(VolumeId volumeId)
    : volumeId_(std::move(volumeId)) {
    // Both engines must produce the same census; AURALITE_SCAN_ENGINE exists so
    // that claim can be checked on a real volume instead of being asserted.
    wchar_t engine[16]{};
    const DWORD len = ::GetEnvironmentVariableW(L"AURALITE_SCAN_ENGINE", engine,
                                                static_cast<DWORD>(std::size(engine)));
    if (len > 0 && len < std::size(engine)) {
        std::wstring_view value(engine, len);
        if (value == L"handles") scanEngine_ = ScanEngine::Handles;
        else if (value == L"mft") scanEngine_ = ScanEngine::Mft;
    }
}

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

void RealNtfsBackend::NoteOpenFailure(const std::string& text) const {
    // Bounded: a volume where nothing opens would otherwise emit 144k lines.
    // Called from the layout workers, so the counter and the probe log are guarded.
    std::lock_guard<std::mutex> lock(probeMutex_);
    if (openFailures_ <= 3) mediaDetector_.NoteProbe(text);
    ++openFailures_;
}

HANDLE RealNtfsBackend::FsctlVolumeHandle() const {
    std::lock_guard<std::mutex> lock(volHandleMutex_);
    if (!cachedVolHandle_.Valid()) {
        cachedVolHandle_ = OpenVolumeForFsctl();
    }
    return cachedVolHandle_.Get();
}

void RealNtfsBackend::InvalidateFsctlVolumeHandle() const {
    std::lock_guard<std::mutex> lock(volHandleMutex_);
    cachedVolHandle_.Reset();
}

UniqueHandle RealNtfsBackend::OpenFileByReference(FileId fileId, DWORD desiredAccess,
                                                 bool protectClusters, HANDLE volOverride) const {
    const bool ownHandle = (volOverride == nullptr);
    HANDLE hVol = ownHandle ? FsctlVolumeHandle() : volOverride;
    if (hVol == INVALID_HANDLE_VALUE || hVol == nullptr) {
        NoteOpenFailure("OpenFileById volume handle failed err=" +
                        std::to_string(::GetLastError()));
        return UniqueHandle{};
    }

    // A USN record carries a 64-bit file reference number: a 48-bit record index
    // plus a 16-bit sequence number. FileId keeps only the index, because that is
    // the identity the rest of the engine uses, but OpenFileById matches on the
    // pair. Trying the full reference first and falling back to the bare index
    // covers both, and every attempt is recorded - 144,838 files reported as
    // "locked" with no error code is how the extent census stayed at zero.
    std::uint64_t candidates[3]{};
    std::size_t count = 0;
    const auto it = refCache_.find(fileId);
    if (it != refCache_.end() && it->second != 0) candidates[count++] = it->second;
    if (count == 0 || candidates[0] != static_cast<std::uint64_t>(fileId)) {
        candidates[count++] = static_cast<std::uint64_t>(fileId);
    }
    // Record 0 as a bare index is rejected with ERROR_INVALID_PARAMETER, but the
    // full reference (record 0, sequence 1) resolves. Without this, $MFT could
    // only be read as raw sectors - see EnumerateViaMft.
    if (fileId == kMftFileId) candidates[count++] = kMftCanonicalReference;

    DWORD lastErr = 0;
    for (std::size_t i = 0; i < count; ++i) {
        FILE_ID_DESCRIPTOR desc{};
        desc.dwSize = sizeof(desc);
        desc.Type = FileIdType;
        desc.FileId.QuadPart = static_cast<LONGLONG>(candidates[i]);

        HANDLE hFile = ::OpenFileById(hVol, &desc, desiredAccess,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, FILE_FLAG_BACKUP_SEMANTICS);
        if (hFile != INVALID_HANDLE_VALUE) {
            if (protectClusters) {
                // Protect our own handle from external defrag interference (D-01).
                // Only for a move: an analysis handle is read-only and has no reason
                // to withhold clusters from VSS or another defragmenter.
                MARK_HANDLE_INFO mhi{};
                mhi.HandleInfo = MARK_HANDLE_PROTECT_CLUSTERS;
                DWORD ret = 0;
                ::DeviceIoControl(hFile, FSCTL_MARK_HANDLE, &mhi, sizeof(mhi), nullptr, 0, &ret, nullptr);
            }
            return UniqueHandle(hFile);
        }
        lastErr = ::GetLastError();
        if (lastErr == ERROR_INVALID_HANDLE && ownHandle) {
            // The volume was remounted or the cached handle went stale: reopen once
            // so a long operation survives it instead of failing every remaining file.
            InvalidateFsctlVolumeHandle();
            hVol = FsctlVolumeHandle();
            if (hVol == INVALID_HANDLE_VALUE || hVol == nullptr) break;
        }
    }

    NoteOpenFailure(std::string("OpenFileById failed id=") + std::to_string(fileId) +
                    " ref=" + std::to_string(count > 0 ? candidates[0] : 0) +
                    " tries=" + std::to_string(count) +
                    " access=" + std::to_string(desiredAccess) +
                    " err=" + std::to_string(lastErr));
    return UniqueHandle{};
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

void RealNtfsBackend::SetScanProgressCallback(std::function<void(std::uint64_t)> callback) {
    scanProgressCb_ = std::move(callback);
}

void RealNtfsBackend::ResolveFileLayout(FileEntry& fe, HANDLE hVol,
                                       std::atomic<std::uint64_t>& failures) const {
    // FILE_READ_DATA, not FILE_READ_ATTRIBUTES: FSCTL_GET_RETRIEVAL_POINTERS
    // is issued on this handle and refuses to run without it.
    UniqueHandle hf = OpenFileByReference(fe.id, FILE_READ_ATTRIBUTES | FILE_READ_DATA,
                                         /*protectClusters=*/false, hVol);
    if (!hf.Valid()) {
        fe.flags.isLocked = true;
        return;
    }
    LARGE_INTEGER fsize{};
    if (::GetFileSizeEx(hf.Get(), &fsize)) {
        fe.sizeBytes = static_cast<std::uint64_t>(fsize.QuadPart);
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
        return;
    }
    // The extent read failed. Leaving fragmentCount at 0 made the file
    // indistinguishable from a contiguous one, so a volume of 160k unreadable
    // files reported "0 fragmented". Count it as unknown and let the caller
    // decide; never as "not fragmented".
    fe.flags.isLocked = true;
    const std::uint64_t n = failures.fetch_add(1) + 1;
    if (n <= 3) {
        // Explicit std::string on every piece: "const char* + std::string_view"
        // is not a valid operator+ in C++20, and ErrorCodeName returns a
        // string_view. MSVC rejects it at /permissive- (C2676).
        std::lock_guard<std::mutex> lock(probeMutex_);
        mediaDetector_.NoteProbe(std::string("ExtentsFailed file=") + std::string(fe.name) +
                                 " err=" + std::to_string(extRes.Error().win32Error) +
                                 " code=" + std::string(ErrorCodeName(extRes.Error().code)));
    }
}

void RealNtfsBackend::ResolveLayouts(std::vector<FileEntry>& files) {
    std::vector<std::size_t> todo;
    todo.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!files[i].flags.isDirectory) todo.push_back(i);
    }
    if (todo.empty()) return;

    // Why a thread pool and not a faster syscall
    // ------------------------------------------
    // This loop is one OpenFileById plus one FSCTL_GET_RETRIEVAL_POINTERS per
    // file - a random read into the MFT and into each file's record. On the
    // reference volume that is ~14 ms per file (70/s, ~38 minutes for 160k
    // files), which is the seek time of the disk, not CPU work: a single
    // outstanding request leaves a HDD nothing to reorder. Several in flight let
    // the volume stack and the drive batch seeks by LBA.
    //
    // Each worker opens its own volume handle. Sharing the cached one would race
    // with InvalidateFsctlVolumeHandle() and with FSCTL_MOVE_FILE on the same
    // handle during a move.
    const std::size_t total = todo.size();
    unsigned workers = std::thread::hardware_concurrency();
    if (workers == 0) workers = 1;
    workers = std::min<unsigned>(workers, 8U);
    // Read through the Win32 API, not getenv/atoi: MSVC's CRT marks both as
    // deprecated (C4996 "This function or variable may be unsafe") and the build
    // treats that as an error, which is how this line broke the Windows build
    // after passing the mingw type check. Parsing a small unsigned number by hand
    // removes the CRT from the question entirely.
    wchar_t workersEnv[16]{};
    const DWORD workersEnvLen =
        ::GetEnvironmentVariableW(L"AURALITE_SCAN_WORKERS", workersEnv,
                                  static_cast<DWORD>(std::size(workersEnv)));
    if (workersEnvLen > 0 && workersEnvLen < std::size(workersEnv)) {
        unsigned v = 0;
        bool digitsOnly = true;
        for (DWORD i = 0; i < workersEnvLen; ++i) {
            if (workersEnv[i] < L'0' || workersEnv[i] > L'9') {
                digitsOnly = false;
                break;
            }
            v = v * 10U + static_cast<unsigned>(workersEnv[i] - L'0');
            if (v > 64U) break;
        }
        if (digitsOnly && v > 0) workers = std::min(v, 64U);
    }
    workers = static_cast<unsigned>(std::min<std::size_t>(workers, total));

    std::vector<char> resolved(total, 0);
    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> failures{0};
    std::atomic<std::uint64_t> finished{0};
    const std::function<void(std::uint64_t)>& cb = scanProgressCb_;
    // The sink is written from every worker; serialise the calls rather than
    // assume the callback is thread-safe.
    std::mutex cbMutex;
    auto report = [&](std::uint64_t n) {
        std::lock_guard<std::mutex> lock(cbMutex);
        cb(n);
    };

    auto work = [&]() {
        UniqueHandle hVol = OpenVolumeForFsctl();
        if (!hVol.Valid()) return; // the sweep below marks everything locked
        for (;;) {
            const std::size_t k = next.fetch_add(1, std::memory_order_relaxed);
            if (k >= total) break;
            ResolveFileLayout(files[todo[k]], hVol.Get(), failures);
            resolved[k] = 1;
            const std::uint64_t n = finished.fetch_add(1, std::memory_order_relaxed) + 1;
            if (cb && (n % 512U) == 0) report(n);
        }
    };

    if (workers <= 1) {
        work();
    } else {
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (unsigned i = 0; i < workers; ++i) pool.emplace_back(work);
        for (auto& t : pool) t.join();
    }

    // A worker that could not open its volume handle leaves items behind. They
    // must not stay at fragmentCount 0 and sizeBytes 0 - that reads as "empty",
    // the exact misreport this census exists to prevent.
    for (std::size_t k = 0; k < total; ++k) {
        if (!resolved[k]) {
            files[todo[k]].flags.isLocked = true;
            failures.fetch_add(1);
        }
    }

    extentReadFailures_ = failures.load();
    if (cb) report(files.size());
}

Result<std::string> RealNtfsBackend::ReadVolumeAt(HANDLE hVol, std::uint64_t offset,
                                                 std::size_t length) const {
    std::string out;
    out.resize(length);
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(hVol, pos, nullptr, FILE_BEGIN)) {
        return Result<std::string>::Failure(
            MakeError(ErrorCode::E_ENUM_IO, volumeId_, 0, ::GetLastError(), "volume: seek failed"));
    }
    DWORD read = 0;
    if (!::ReadFile(hVol, out.data(), static_cast<DWORD>(length), &read, nullptr) ||
        read != length) {
        return Result<std::string>::Failure(
            MakeError(ErrorCode::E_ENUM_IO, volumeId_, 0, ::GetLastError(), "volume: read failed"));
    }
    return Result<std::string>::Success(std::move(out));
}

Result<std::string> RealNtfsBackend::ReadMftRange(HANDLE hVol, const std::vector<Extent>& runs,
                                                  std::uint32_t bytesPerCluster,
                                                  std::uint64_t offset, std::size_t length) const {
    // The mapping from stream offsets to physical ones lives in the portable MFT
    // module, where a fragmented table is covered by a test. This function is only
    // the Win32 read.
    auto readAt = [this, hVol](std::uint64_t volumeOffset, std::size_t want) {
        return ReadVolumeAt(hVol, volumeOffset, want);
    };
    return mft::ReadStreamRange(runs, bytesPerCluster, offset, length, readAt);
}

Result<std::vector<FileEntry>> RealNtfsBackend::EnumerateViaMft(std::uint64_t maxFiles) {
    auto geomRes = Geometry();
    if (!geomRes) return Result<std::vector<FileEntry>>::Failure(geomRes.Error());
    const VolumeGeometry geom = geomRes.Value();
    if (geom.mftValidDataLength == 0 || geom.bytesPerFileRecordSegment == 0) {
        return Result<std::vector<FileEntry>>::Failure(
            MakeError(ErrorCode::E_ENUM_IO, volumeId_, 0, 0, "mft: geometry does not describe the table"));
    }

    UniqueHandle hVol = OpenVolumeHandle(FILE_READ_ATTRIBUTES | FILE_READ_DATA);
    if (!hVol.Valid()) {
        return Result<std::vector<FileEntry>>::Failure(
            MakeError(ErrorCode::E_ENUM_IO, volumeId_, 0, ::GetLastError()));
    }

    // Preferred: read the table through a handle on $MFT itself.
    //
    // Raw sector reads go straight to the disk and therefore bypass the NTFS
    // metadata cache, so a record the filesystem updated seconds ago - every
    // record a just-completed move touched - can still read back with its old
    // run list. The first real move exposed exactly that: two moves reported as
    // completed and verified, and an analysis that still showed the old layout,
    // so the next run planned the same two moves again.
    //
    // A handle could not be opened before (FileIdType 0 is rejected); the full
    // reference works, and reads through it are cache-coherent.
    UniqueHandle hMft = OpenFileByReference(kMftFileId, FILE_READ_DATA | FILE_READ_ATTRIBUTES);

    // Bootstrap: $MFT cannot be opened as "file 0".
    //
    // OpenFileById with a FileIdType descriptor of 0 is rejected with
    // ERROR_INVALID_PARAMETER (err=87) - that is exactly what the first run on a
    // real volume reported ("OpenFileById failed id=0 ref=0 tries=1 access=129
    // err=87"). Record 0 sits at MftStartLcn by definition, so it is read
    // straight off the volume and its own $DATA run list says where the rest of
    // the table is. This also removes the last OpenFileById from the fast path.
    const std::uint32_t bytesPerCluster = geom.bytesPerCluster;
    std::vector<Extent> runs;
    const mft::RecordGeometry recordGeom{geom.bytesPerFileRecordSegment, bytesPerCluster};
    {
        auto firstRecord =
            ReadVolumeAt(hVol.Get(), static_cast<std::uint64_t>(geom.mftStartLcn) * bytesPerCluster,
                         geom.bytesPerFileRecordSegment);
        if (firstRecord) {
            auto parsed = mft::ParseRecord(firstRecord.Value(), 0, recordGeom);
            if (parsed && !parsed.Value().extents.empty()) {
                runs = parsed.Value().extents;
                if (runs.front().startLcn != geom.mftStartLcn) {
                    // Only informational: the record is authoritative, the
                    // geometry field is not always filled in on every driver.
                    mediaDetector_.NoteProbe("MftStartLcnMismatch geom=" +
                                             std::to_string(geom.mftStartLcn) +
                                             " record=" + std::to_string(runs.front().startLcn));
                }
            } else {
                mediaDetector_.NoteProbe("MftRecord0Unparsed, cannot bootstrap the table");
            }
        } else {
            mediaDetector_.NoteProbe("MftRecord0Unreadable: " + firstRecord.Error().diagnosticDetail);
        }
    }
    if (runs.empty()) {
        return Result<std::vector<FileEntry>>::Failure(MakeError(
            ErrorCode::E_ENUM_IO, volumeId_, 0, 0,
            "mft: cannot locate the $MFT run list from record 0"));
    }


    const std::function<void(std::uint64_t)> progress = scanProgressCb_;
    const std::vector<Extent> mftRuns = runs;
    const HANDLE volHandle = hVol.Get();
    const HANDLE mftHandle = hMft.Valid() ? hMft.Get() : nullptr;

    auto reader = [this, volHandle, mftHandle, mftRuns, bytesPerCluster](std::uint64_t offset,
                                                                        std::size_t length) {
        if (mftHandle != nullptr) return ReadVolumeAt(mftHandle, offset, length);
        return ReadMftRange(volHandle, mftRuns, bytesPerCluster, offset, length);
    };

    auto scanRes = mft::ScanMft(geom.mftValidDataLength, recordGeom, reader, maxFiles, progress);
    if (!scanRes) {
        mediaDetector_.NoteProbe(std::string("MftScanFailed: ") +
                                 std::string(scanRes.Error().diagnosticDetail));
        return Result<std::vector<FileEntry>>::Failure(scanRes.Error());
    }
    mft::ScanResult scan = std::move(scanRes.Value());

    PathComposer composer;
    composer.AddDirectory(PathComposer::kRootDirectoryId, PathComposer::kRootDirectoryId, ".");

    std::vector<FileEntry> entries;
    entries.reserve(scan.records.size());
    for (const auto& rec : scan.records) {
        FileEntry fe = mft::EntryFromRecord(rec);
        const FileId parent = static_cast<FileId>(rec.parentRef & 0x0000FFFFFFFFFFFFULL);
        if (fe.flags.isDirectory) {
            composer.AddDirectory(fe.id, parent, fe.name);
        } else {
            composer.AddFile(fe.id, parent, fe.name);
        }
        // The full 64-bit reference (sequence + index) is what OpenFileById wants,
        // and the record just handed it over for free.
        refCache_[fe.id] = (rec.sequence << 48) | static_cast<std::uint64_t>(fe.id);
        entries.push_back(std::move(fe));
    }

    // Files whose attributes are split across records: the run list decoded from
    // the base record may be incomplete, so those are confirmed the old way. On a
    // normal volume this is a handful of files.
    if (!scan.needsHandleFallback.empty()) {
        std::uint64_t failures = 0;
        for (const FileId id : scan.needsHandleFallback) {
            auto it = std::find_if(entries.begin(), entries.end(),
                                   [id](const FileEntry& e) { return e.id == id; });
            if (it == entries.end()) continue;
            auto hf = OpenFileByReference(id, FILE_READ_ATTRIBUTES | FILE_READ_DATA);
            if (!hf.Valid()) {
                it->flags.isLocked = true;
                ++failures;
                continue;
            }
            LARGE_INTEGER fsize{};
            if (::GetFileSizeEx(hf.Get(), &fsize)) {
                it->sizeBytes = static_cast<std::uint64_t>(fsize.QuadPart);
            }
            auto extRes = ReadRetrievalPointers(hf.Get());
            if (!extRes) {
                it->flags.isLocked = true;
                ++failures;
                continue;
            }
            it->extents = std::move(extRes.Value());
            it->flags.resident = it->extents.empty();
            it->fragmentCount = it->extents.empty() ? 0
                                                    : static_cast<std::uint32_t>(it->extents.size());
            it->allocatedClusters = 0;
            for (const auto& e : it->extents) it->allocatedClusters += e.clusterCount;
        }
        if (failures > 0) {
            mediaDetector_.NoteProbe("MftFallbackFailed count=" + std::to_string(failures));
        }
    }

    const std::string rootPrefix = Win32Platform::WideToUtf8(VolumeRootPath());
    for (auto& fe : entries) {
        std::string composed = composer.PathOf(fe.id, rootPrefix);
        if (composed.empty()) composed = "\\" + fe.name;
        fe.path = std::move(composed);
        pathCache_[fe.id] = fe.path;
        flagsCache_[fe.id] = fe.flags;
    }

    if (scanProgressCb_) scanProgressCb_(entries.size());
    mediaDetector_.NoteProbe(hMft.Valid()
                                 ? "MftScan via $MFT handle (cache-coherent)"
                                 : "MftScan via raw volume reads (may lag behind the cache)");
    mediaDetector_.NoteProbe("MftScan records=" + std::to_string(scan.stats.recordsSeen) +
                             " used=" + std::to_string(scan.stats.recordsUsed) +
                             " extension=" + std::to_string(scan.stats.extensionRecords) +
                             " attrlist=" + std::to_string(scan.stats.withAttributeList) +
                             " malformed=" + std::to_string(scan.stats.malformed));
    return Result<std::vector<FileEntry>>::Success(std::move(entries));
}

Result<std::vector<FileEntry>> RealNtfsBackend::Enumerate(std::uint64_t maxFiles) {
    // The fast path replaces ~160,000 random reads with one sequential read of
    // $MFT. It is the default, but it is never trusted blindly: any failure falls
    // back to the handle path rather than failing the analysis.
    if (scanEngine_ != ScanEngine::Handles) {
        auto viaMft = EnumerateViaMft(maxFiles);
        if (viaMft) return viaMft;
        if (scanEngine_ == ScanEngine::Mft) return viaMft; // explicitly requested: report it
        mediaDetector_.NoteProbe("MftScanUnavailable, falling back to the handle scan");
    }

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
    PathComposer composer;
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
            refCache_[fe.id] = rec.fileReferenceNumber;
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
                // The path is composed from the USN parent chain after enumeration
                // (PathComposer). GetFinalPathNameByHandleW cost ~14 ms per file on a
                // real HDD - 36 minutes for 160k files - and produced nothing at all
                // for the files that could not be opened.
                composer.AddFile(fe.id,
                                 static_cast<FileId>(rec.parentFileReferenceNumber &
                                                     0x0000FFFFFFFFFFFFULL),
                                 fe.name);
            } else {
                composer.AddDirectory(fe.id,
                                      static_cast<FileId>(rec.parentFileReferenceNumber &
                                                          0x0000FFFFFFFFFFFFULL),
                                      fe.name);
            }

            entries.push_back(std::move(fe));

        }
    }

    // Phase 2: the expensive half. One open plus one ioctl per file, spread over
    // several threads.
    ResolveLayouts(entries);

    // Phase 3: compose every path from data already in memory.
    const std::string rootPrefix = Win32Platform::WideToUtf8(VolumeRootPath());
    for (auto& fe : entries) {
        std::string composed = composer.PathOf(fe.id, rootPrefix);
        if (composed.empty()) {
            // An ancestor that was not in the journal (deleted before it began)
            // leaves no usable path; keep something the operator can read.
            composed = "\\" + fe.name;
        }
        fe.path = std::move(composed);
        pathCache_[fe.id] = fe.path;
        flagsCache_[fe.id] = fe.flags;
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
    // FILE_READ_DATA, not FILE_READ_ATTRIBUTES. FSCTL_GET_RETRIEVAL_POINTERS is
    // issued on this handle and refuses to run without it, so an attribute-only
    // handle made every post-move verification fail with E_VERIFY_READBACK_FAILED
    // on a volume where the move itself had succeeded. This is the same trap that
    // was already fixed once in the enumeration path; the verification path kept a
    // copy of it.
    UniqueHandle hf = OpenFileByReference(fileId, FILE_READ_ATTRIBUTES | FILE_READ_DATA);
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

    // FSCTL_MOVE_FILE needs the file itself opened for reading and writing data.
    // An attributes-only handle is rejected by the volume before any cluster is
    // touched (the first real-volume attempt failed here), and the move path had
    // inherited the read-only access mask from the analysis path.
    UniqueHandle hFile = OpenFileByReference(fileId, FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES,
                                             /*protectClusters=*/true);
    if (!hFile.Valid()) {
        const DWORD err = ::GetLastError();
        mediaDetector_.NoteProbe("MoveOpenFailed id=" + std::to_string(fileId) +
                                 " err=" + std::to_string(err));
        return {false, 0, err, static_cast<std::uint32_t>(MapWin32ToMoveError(err)), false, SkipReason::AccessDenied};
    }

    // The caller names the VCN and the destination; the source LCN is only known
    // to the volume. It is read here so a failed move can be reported in terms of
    // both ends (src -> dst) instead of a VCN that means nothing to an operator.
    Lcn sourceLcnForProbe = kInvalidLcn;
    if (auto extRes = ReadRetrievalPointers(hFile.Get())) {
        for (const auto& e : extRes.Value()) {
            const Vcn end = e.startVcn + e.clusterCount;
            if (startVcn >= e.startVcn && startVcn < end) {
                sourceLcnForProbe = e.startLcn + (startVcn - e.startVcn);
                break;
            }
        }
    }

    if (sourceLcnForProbe != kInvalidLcn && sourceLcnForProbe == targetLcn) {
        // The extent is already where the plan wants it. Issuing the ioctl would
        // "succeed" while changing nothing, and the caller would count it as a
        // completed move - which is how a stale plan can look productive forever.
        return {false, 0, 0, static_cast<std::uint32_t>(ErrorCode::E_MOVE_INVALID_PARAM), false,
                SkipReason::None};
    }

    MOVE_FILE_DATA mfd{};
    mfd.FileHandle = hFile.Get();
    mfd.StartingVcn.QuadPart = static_cast<LONGLONG>(startVcn);
    mfd.StartingLcn.QuadPart = static_cast<LONGLONG>(targetLcn);
    mfd.ClusterCount = clusters;

    DWORD bytesRet = 0;
    if (!::DeviceIoControl(hVol.Get(), FSCTL_MOVE_FILE, &mfd, sizeof(mfd), nullptr, 0, &bytesRet, nullptr)) {
        const DWORD err = ::GetLastError();
        // Bounded: the engine retries, so an unbounded probe log would grow with
        // the number of attempts rather than the number of files.
        {
            // Named, not `lock`: the enclosing MoveExtent already holds a
            // `lock_guard lock(moveMutex_)`, and MSVC rejects the shadowing with
            // C4456, which is an error in this build (the first attempt died here
            // after passing the mingw type check).
            std::lock_guard<std::mutex> probeLock(probeMutex_);
            if (moveProbeFailures_ < 3) {
                mediaDetector_.NoteProbe("MoveFileFailed id=" + std::to_string(fileId) +
                                         " vcn=" + std::to_string(startVcn) +
                                         " src=" + std::to_string(sourceLcnForProbe) +
                                         " dst=" + std::to_string(targetLcn) +
                                         " clusters=" + std::to_string(clusters) +
                                         " err=" + std::to_string(err));
            }
            ++moveProbeFailures_;
        }
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
    } else {
        // A report of "MFT: 0 fragments" is a claim. When the layout could not be
        // read that claim is false, and Windows' own analysis usually reports a
        // non-zero count for the same volume, so say which one this is.
        mediaDetector_.NoteProbe(std::string("MftExtentsUnavailable err=") +
                                 std::to_string(extRes.Error().win32Error) +
                                 " code=" + std::string(ErrorCodeName(extRes.Error().code)));
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
    UniqueHandle hf = OpenFileByReference(fileId, FILE_READ_ATTRIBUTES | FILE_READ_DATA);
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
