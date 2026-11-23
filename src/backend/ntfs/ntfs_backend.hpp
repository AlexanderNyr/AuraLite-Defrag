#pragma once

#include "backend/ntfs/media_detector.hpp"
#include "interfaces/i_filesystem_adapter.hpp"
#include "interfaces/i_volume_provider.hpp"
#include "platform/win32_platform.hpp"

#include <memory>
#include <mutex>
#include <string>
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

private:
#ifdef _WIN32
    UniqueHandle OpenVolumeHandle(DWORD desiredAccess) const;
    UniqueHandle OpenFileByReference(FileId fileId, DWORD desiredAccess) const;
    std::wstring VolumeDevicePath() const;
    std::wstring VolumeRootPath() const;
    Result<std::vector<Extent>> ReadRetrievalPointers(HANDLE fileHandle) const;
#endif

    VolumeId volumeId_;
    RealMediaDetector mediaDetector_;
    mutable std::mutex moveMutex_; // Single serialised move queue per volume (§3.5)
    std::unordered_map<FileId, std::string> pathCache_;
    std::unordered_map<FileId, FileFlags> flagsCache_;
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

private:
    std::shared_ptr<RealNtfsBackend> b_;
};

} // namespace auralite
