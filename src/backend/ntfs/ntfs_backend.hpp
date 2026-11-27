#pragma once

#include "backend/ntfs/media_detector.hpp"
#include "interfaces/i_filesystem_adapter.hpp"
#include "interfaces/i_volume_provider.hpp"
#include "platform/win32_platform.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

namespace auralite {

class RealNtfsBackend final : public IVolumeProvider, public IFileSystemAdapter {
public:
    explicit RealNtfsBackend(VolumeId volumeId);

    // IVolumeProvider
    VolumeId Id() const override { return volumeId_; }
    Result<VolumeGeometry> Geometry() override;
    MediaClass Media() override;
    MediaEvidence Evidence() override;
    Result<PreflightResult> Preflight(bool isElevated, double fullDefragThresholdPct) override;
    Result<BitmapSnapshot> ReadBitmap(Lcn start, ClusterCount count) override;
    bool IsSimulation() const noexcept override { return false; }

    // IFileSystemAdapter
    CapabilityBits Capabilities() const override;
    Result<std::vector<FileEntry>> Enumerate(std::uint64_t maxFiles) override;
    Result<std::vector<Extent>> Extents(FileId fileId) override;
    Result<FileFlags> Flags(FileId fileId) override;
    MoveResult MoveExtent(FileId fileId, Vcn startVcn, Lcn targetLcn, std::uint32_t clusters) override;
    Result<MftInfo> Mft() override;
    Result<MftZoneInfo> Zone() override;
    Result<std::uint64_t> ComputeWindowChecksum(FileId fileId) override;
    void SetScanProgressCallback(std::function<void(std::uint64_t)> callback) override;

private:
#ifdef _WIN32
    UniqueHandle OpenVolumeHandle(DWORD desiredAccess) const;
    // Opens the volume with the access every FSCTL in this backend needs.
    UniqueHandle OpenVolumeForFsctl() const;
    // OpenFileById needs a volume handle. Opening it per file cost ~13 ms per file
    // on a real HDD (74 files/s, ~36 minutes for 160k files); it is opened once and
    // reused. Invalidated on ERROR_INVALID_HANDLE so a remounted volume recovers.
    HANDLE FsctlVolumeHandle() const;
    void InvalidateFsctlVolumeHandle() const;
    // Counted so a volume whose extents could not be read is reported as such
    // instead of as "0 fragmented files".
    mutable std::uint64_t extentReadFailures_{0};
    // NoteProbe/openFailures_ are written from the layout workers; the probe log
    // is a plain vector and would otherwise be corrupted by concurrent pushes.
    mutable std::mutex probeMutex_;
    // protectClusters applies FSCTL_MARK_HANDLE/MARK_HANDLE_PROTECT_CLUSTERS. Only
    // a handle that is about to be moved needs it: applying it during read-only
    // analysis is an unrequested side effect on every file (P9-T02 F-03).
    // volOverride lets a worker resolve files through its own volume handle
    // instead of the shared cache, so a thread pool never touches the cached
    // handle that another thread may be invalidating.
    UniqueHandle OpenFileByReference(FileId fileId, DWORD desiredAccess,
                                     bool protectClusters = false,
                                     HANDLE volOverride = nullptr) const;
    // Measures one file (size + extents). Split out of the enumeration loop so it
    // can run on several threads; it only writes to the FileEntry it is given.
    void ResolveFileLayout(FileEntry& fe, HANDLE hVol,
                           std::atomic<std::uint64_t>& failures) const;
    // Second phase of Enumerate: layouts for every non-directory file, in
    // parallel. See the definition for why.
    void ResolveLayouts(std::vector<FileEntry>& files);
    // The $MFT fast path: one sequential read of the file table instead of one
    // random read per file. Returns a failure (never a partial list) when the
    // table cannot be read, so Enumerate can fall back to the handle path.
    Result<std::vector<FileEntry>> EnumerateViaMft(std::uint64_t maxFiles);
    // Reads `length` bytes at `offset` of the $MFT data stream, following its run
    // list. The stream is not contiguous in general.
    Result<std::string> ReadVolumeAt(HANDLE hVol, std::uint64_t offset, std::size_t length) const;
    Result<std::string> ReadMftRange(HANDLE hVol, const std::vector<Extent>& runs,
                                     std::uint32_t bytesPerCluster,
                                     std::uint64_t offset, std::size_t length) const;
    // Full 64-bit reference (record index + sequence) as it came out of the USN
    // record. FileId only keeps the 48-bit index, and OpenFileById matches the
    // pair, so the index alone is not enough.
    mutable std::uint64_t openFailures_{0};
    void NoteOpenFailure(const std::string& text) const;
    std::wstring VolumeDevicePath() const;
    std::wstring VolumeRootPath() const;
    Result<std::vector<Extent>> ReadRetrievalPointers(HANDLE fileHandle) const;
#endif

    VolumeId volumeId_;
    mutable RealMediaDetector mediaDetector_;
    mutable std::mutex moveMutex_; // Single serialised move queue per volume (§3.5)
    std::unordered_map<FileId, std::string> pathCache_;
    std::unordered_map<FileId, FileFlags> flagsCache_;
    std::unordered_map<FileId, std::uint64_t> refCache_;
    std::function<void(std::uint64_t)> scanProgressCb_;
    // AURALITE_SCAN_ENGINE = auto (default) | mft | handles. Both paths must
    // produce the same census, which is how the fast path is validated on a real
    // volume: run with each and compare the Extents line.
    enum class ScanEngine { Auto = 0, Mft, Handles };
    ScanEngine scanEngine_{ScanEngine::Auto};
    mutable UniqueHandle cachedVolHandle_;
    mutable std::mutex volHandleMutex_;
    VolumeGeometry cachedGeom_{};
    bool hasCachedGeom_{false};
};

class NtfsVolumeProxy final : public IVolumeProvider {
public:
    explicit NtfsVolumeProxy(std::shared_ptr<RealNtfsBackend> b) : b_(std::move(b)) {}
    VolumeId Id() const override { return b_->Id(); }
    Result<VolumeGeometry> Geometry() override { return b_->Geometry(); }
    MediaClass Media() override { return b_->Media(); }
    MediaEvidence Evidence() override { return b_->Evidence(); }
    Result<PreflightResult> Preflight(bool el, double th) override { return b_->Preflight(el, th); }
    Result<BitmapSnapshot> ReadBitmap(Lcn s, ClusterCount c) override { return b_->ReadBitmap(s, c); }
    bool IsSimulation() const noexcept override { return false; }

private:
    std::shared_ptr<RealNtfsBackend> b_;
};

class NtfsFsProxy final : public IFileSystemAdapter {
public:
    explicit NtfsFsProxy(std::shared_ptr<RealNtfsBackend> b) : b_(std::move(b)) {}
    CapabilityBits Capabilities() const override { return b_->Capabilities(); }
    Result<std::vector<FileEntry>> Enumerate(std::uint64_t m) override { return b_->Enumerate(m); }
    Result<std::vector<Extent>> Extents(FileId id) override { return b_->Extents(id); }
    Result<FileFlags> Flags(FileId id) override { return b_->Flags(id); }
    MoveResult MoveExtent(FileId id, Vcn v, Lcn l, std::uint32_t c) override { return b_->MoveExtent(id, v, l, c); }
    Result<MftInfo> Mft() override { return b_->Mft(); }
    Result<MftZoneInfo> Zone() override { return b_->Zone(); }
    Result<std::uint64_t> ComputeWindowChecksum(FileId id) override { return b_->ComputeWindowChecksum(id); }
    // Without this forwarder the callback is dropped by the proxy and the
    // enumeration stays silent - exactly the failure this hook exists to remove.
    void SetScanProgressCallback(std::function<void(std::uint64_t)> cb) override {
        b_->SetScanProgressCallback(std::move(cb));
    }

private:
    std::shared_ptr<RealNtfsBackend> b_;
};

} // namespace auralite
