#pragma once

#include "interfaces/i_filesystem_adapter.hpp"
#include "interfaces/i_media_detector.hpp"
#include "interfaces/i_volume_provider.hpp"

#include <cstdint>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

namespace auralite {

enum class FaultType : std::uint8_t {
    None = 0,
    IoError,
    PartialFailure,
    InterruptionMidMove,
    AccessDenied,
    LockedInUse,
    UnsupportedStream,
    FileVanished,
};

struct ScheduledFault {
    FaultType type{FaultType::None};
    std::uint64_t atOperation{0};
    FileId targetFileId{kInvalidFileId}; // Optional filter; kInvalidFileId = match by operationIndex
    std::uint32_t win32Error{5};
    bool triggered{false};
};

struct EmulatorConfig {
    VolumeId volumeId{"SIM-VOL-0"};
    VolumeGeometry geometry{};
    MediaClass mediaClass{MediaClass::Spinning};
    MediaEvidence mediaEvidence{};
    CapabilityBits capabilities{};
    bool dirtyVolume{false};
    bool readOnlyVolume{false};
    bool bitLockerLocked{false};
    bool isSimulationMode{true};
    std::uint64_t seed{1};
};

class EmulatorMediaDetector final : public IMediaDetector {
public:
    explicit EmulatorMediaDetector(MediaClass mc, MediaEvidence ev)
        : mediaClass_(mc), evidence_(std::move(ev)) {}

    MediaClass Classify(const VolumeId&) override { return mediaClass_; }
    MediaEvidence Detail() const override { return evidence_; }
    void SetMediaClass(MediaClass mc) noexcept { mediaClass_ = mc; }

private:
    MediaClass mediaClass_;
    MediaEvidence evidence_;
};

class EmulatorBackend final : public IVolumeProvider, public IFileSystemAdapter {
public:
    explicit EmulatorBackend(EmulatorConfig config);

    // IVolumeProvider
    VolumeId Id() const override { return config_.volumeId; }
    Result<VolumeGeometry> Geometry() override;
    MediaClass Media() override { return mediaDetector_.Classify(config_.volumeId); }
    MediaEvidence Evidence() override { return mediaDetector_.Detail(); }
    Result<PreflightResult> Preflight(bool isElevated, double fullDefragThresholdPct) override;
    Result<BitmapSnapshot> ReadBitmap(Lcn start, ClusterCount count) override;
    bool IsSimulation() const noexcept override { return config_.isSimulationMode; }

    // IFileSystemAdapter
    CapabilityBits Capabilities() const override { return config_.capabilities; }
    Result<std::vector<FileEntry>> Enumerate(std::uint64_t maxFiles) override;
    void SetScanProgressCallback(std::function<void(std::uint64_t)> callback) override {
        scanProgressCb_ = std::move(callback);
    }
    Result<std::vector<Extent>> Extents(FileId fileId) override;
    Result<FileFlags> Flags(FileId fileId) override;
    MoveResult MoveExtent(FileId fileId, Vcn startVcn, Lcn targetLcn, std::uint32_t clusters) override;
    Result<MftInfo> Mft() override;
    Result<MftZoneInfo> Zone() override;
    Result<std::uint64_t> ComputeWindowChecksum(FileId fileId) override;

    // Setup & testing helpers
    void AddFile(FileEntry entry);
    void RemoveFile(FileId fileId);
    void SetScheduledFaults(std::vector<ScheduledFault> faults);
    void SetMediaClass(MediaClass mc) noexcept { mediaDetector_.SetMediaClass(mc); }
    void SetDirty(bool dirty) noexcept { config_.dirtyVolume = dirty; }
    void SetReadOnly(bool ro) noexcept { config_.readOnlyVolume = ro; }
    void SetBitLockerLocked(bool locked) noexcept { config_.bitLockerLocked = locked; }
    void SetMftInfo(MftInfo info) { mftInfo_ = std::move(info); }
    void SetReservedZoneLevel(std::uint32_t lvl) noexcept { reservedZoneLevel_ = lvl; }

    // Simulates MFT growth (for Tier 2 S-01 validation)
    Result<std::uint32_t> SimulateMftGrowth(ClusterCount additionalClusters);

    // Invariant verifier for property/unit tests
    bool VerifyModelInvariants(std::string* outReason = nullptr) const;
    std::uint64_t MoveCallCount() const noexcept { return moveCallCount_; }

private:
    static std::uint32_t CountFragments(const std::vector<Extent>& extents) noexcept;
    static void MergeContiguousExtents(std::vector<Extent>& extents);
    bool RelocateFileExtents(FileEntry& file, Vcn startVcn, Lcn targetLcn, ClusterCount count);

    EmulatorConfig config_;
    EmulatorMediaDetector mediaDetector_;
    BitmapSnapshot bitmap_;
    std::vector<FileEntry> files_;
    std::unordered_map<FileId, std::size_t> fileIndex_;
    std::vector<ScheduledFault> faults_;
    MftInfo mftInfo_{};
    std::uint32_t reservedZoneLevel_{1};
    std::uint64_t moveCallCount_{0};
    std::function<void(std::uint64_t)> scanProgressCb_;
};

// Deterministic 64-bit SplitMix64 PRNG
class DeterministicRng {
public:
    explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}
    std::uint64_t NextU64() noexcept;
    std::uint64_t Range(std::uint64_t lo, std::uint64_t hi) noexcept;

private:
    std::uint64_t state_;
};

} // namespace auralite
