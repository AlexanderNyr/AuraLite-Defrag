#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace auralite {

using VolumeId = std::string;
using FileId = std::uint64_t;
using Lcn = std::uint64_t;
using Vcn = std::uint64_t;
using ClusterCount = std::uint64_t;

inline constexpr Lcn kInvalidLcn = ~static_cast<Lcn>(0);
inline constexpr FileId kInvalidFileId = ~static_cast<FileId>(0);
inline constexpr FileId kMftFileId = 0;
inline constexpr FileId kMftMirrFileId = 1;

enum class MediaClass : std::uint8_t {
    Spinning = 0,
    SolidState = 1,
    Unknown = 2, // Policy-equivalent to SolidState (fail-closed)
};

inline bool IsDefragBlockedMedia(MediaClass mc) noexcept {
    return mc != MediaClass::Spinning;
}

struct MediaEvidence {
    bool seekPenaltyValid{false};
    bool incursSeekPenalty{false};
    bool trimValid{false};
    bool trimEnabled{false};
    std::uint32_t busType{0};
    bool removableMedia{false};
    bool isVirtualDisk{false};
    std::string vendorId;
    std::string productId;
    std::vector<std::string> probeLog;
};

struct Extent {
    Vcn startVcn{0};
    Lcn startLcn{0};
    ClusterCount clusterCount{0};
};

// Runs that follow each other in both VCN and LCN are one extent with a longer
// run, whatever the filesystem chose to split them into on disk.
//
// This is not cosmetics. FSCTL_MOVE_FILE moves one chunk at a time, and NTFS
// leaves the resulting run list as a chain of adjacent entries: a file moved to
// a single free run comes back as VCN 0 + 7568 @ 38431076 followed by
// VCN 7568 + 2147 @ 38438644 - 38431076 + 7568 == 38438644, one unbroken stretch
// of disk. Counting entries instead of discontinuities made that file look
// fragmented forever: every run re-planned the same two moves, moved it to
// another contiguous run, and the census never changed.
//
// Sparse runs (VCNs with no LCN) are never merged across, because the LCN
// adjacency test fails there by construction.
inline void MergeAdjacentExtents(std::vector<Extent>& extents) {
    if (extents.size() < 2) return;
    std::vector<Extent> merged;
    merged.reserve(extents.size());
    for (const auto& e : extents) {
        if (e.clusterCount == 0) continue;
        if (!merged.empty()) {
            Extent& prev = merged.back();
            if (prev.startVcn + prev.clusterCount == e.startVcn &&
                prev.startLcn + prev.clusterCount == e.startLcn) {
                prev.clusterCount += e.clusterCount;
                continue;
            }
        }
        merged.push_back(e);
    }
    extents.swap(merged);
}

struct FileFlags {
    bool compressed{false};
    bool sparse{false};
    bool encrypted{false};
    bool reparsePoint{false};
    bool resident{false};
    bool hasAds{false};
    bool isMetadata{false};
    bool isPageOrHiber{false};
    bool isLocked{false};
    bool isDirectory{false};
    bool isExcluded{false};
    bool inMftZone{false};
};

enum class FileCategory : std::uint8_t {
    Other = 0,
    Document,
    Archive,
    Media,
    Executable,
    System,
    Temp,
    Database,
    Image,
    DiskImage,
};

enum class FileState : std::uint8_t {
    Ready = 0,
    Fragmented,
    Unmovable,
    Excluded,
    InProgress,
    Skipped,
    Defragmented,
};

enum class SkipReason : std::uint8_t {
    None = 0,
    MetadataFile,
    PageOrSystemFile,
    LockedOrInUse,
    AccessDenied,
    ExcludedByRule,
    UnsupportedStream,
    InsufficientContiguousSpace,
    StaleBitmapConflict,
    ReadOnlyOrPreFlightBlocked,
    IoError,
    MftSafeguardAbort,
    FileVanished,
};

struct FileEntry {
    FileId id{0};
    std::string name;
    std::string path; // UTF-8 internal canonical representation; converted to UTF-16 \\?\ at Win32 boundary
    std::uint64_t sizeBytes{0};
    ClusterCount allocatedClusters{0};
    FileFlags flags{};
    FileCategory category{FileCategory::Other};
    FileState state{FileState::Ready};
    SkipReason skipReason{SkipReason::None};
    std::vector<Extent> extents;
    std::uint32_t fragmentCount{0};
    std::uint64_t contentChecksum{0}; // Optional deterministic/verification hash
};

// Mirrors verified D-06 NTFS_VOLUME_DATA_BUFFER fields + volume identity
struct VolumeGeometry {
    std::uint64_t volumeSerialNumber{0};
    std::uint64_t numberSectors{0};
    ClusterCount totalClusters{0};
    ClusterCount freeClusters{0};
    ClusterCount totalReserved{0};
    std::uint32_t bytesPerSector{512};
    std::uint32_t bytesPerCluster{4096};
    std::uint32_t bytesPerFileRecordSegment{1024};
    std::uint32_t clustersPerFileRecordSegment{0};
    std::uint64_t mftValidDataLength{0};
    Lcn mftStartLcn{0};
    Lcn mft2StartLcn{0};
    Lcn mftZoneStart{0};
    Lcn mftZoneEnd{0};
    std::uint16_t majorVersion{3};
    std::uint16_t minorVersion{1};
    std::string label;
    std::string fsName{"NTFS"};
    std::string driveLetter;
    std::string volumeGuidPath;
};

struct CapabilityBits {
    bool supportsMftAnalysis{true};
    bool supportsOnlineMetadataMove{false};
    bool supportsDirectMftMove{false};
    bool supportsSparseAndCompressed{true};
    std::uint32_t moveGranularityClusters{1};
    std::uint32_t sparseMoveGranularityClusters{16};
    std::uint32_t maxMoveClustersPerCall{65536};
    bool bootTimeAvailable{false}; // Explicitly false in v1 (R-014)
};

enum class MftTier : std::uint8_t {
    Tier1Direct = 1,
    Tier2Indirect = 2,
    Tier3ReportOnly = 3,
    Tier4BootTimeDeferred = 4,
};

struct MftInfo {
    Lcn mftStartLcn{0};
    Lcn mft2StartLcn{0};
    std::uint64_t mftValidDataLength{0};
    std::uint32_t fragmentCount{0};
    std::vector<Extent> mftExtents;
    std::uint64_t totalRecords{0};
    std::uint64_t inUseRecords{0};
    std::uint64_t freeRecords{0};
    bool mftMirrMovable{false};
};

struct MftZoneInfo {
    Lcn zoneStartLcn{0};
    Lcn zoneEndLcn{0};
    ClusterCount totalZoneClusters{0};
    ClusterCount freeZoneClusters{0};
    ClusterCount occupiedByMftClusters{0};
    ClusterCount occupiedByUserClusters{0};
    std::uint32_t reservedSettingLevel{1}; // 1..4 (200 MB units per D-14)
};

enum class PreflightStatus : std::uint8_t {
    Clean = 0,
    QuickOnlyLowSpace,
    InsufficientFreeSpace,
    DirtyVolume,
    ReadOnlyVolume,
    BitLockerLocked,
    UnsupportedFileSystem,
    SolidStateBlocked,
    NotElevated,
    VolumeInaccessible,
};

struct PreflightResult {
    bool passed{false};
    bool quickOnly{false};
    PreflightStatus status{PreflightStatus::VolumeInaccessible};
    double freePercent{0.0};
    MediaClass mediaClass{MediaClass::Unknown};
    MediaEvidence evidence{};
    std::string messageKey;
    std::string detail;
};

// 9 legend categories in §7.2 (ordered by display priority lowest -> highest)
enum class ClusterCategory : std::uint8_t {
    Free = 0,
    UsedContiguous = 1,
    Fragmented = 2,
    UnmovableSystem = 3,
    MftZone = 4,
    Mft = 5,
    Excluded = 6,
    SelectedFile = 7,
    BeingProcessed = 8,
};

inline constexpr std::size_t kClusterCategoryCount = 9;

struct BitmapSnapshot {
    Lcn startLcn{0};
    ClusterCount totalClusters{0};
    std::uint64_t generation{0};
    // 1 bit per cluster, LSB-first per byte (matching FSCTL_GET_VOLUME_BITMAP)
    std::vector<std::uint8_t> bits;

    bool IsAllocated(Lcn lcn) const noexcept {
        if (lcn < startLcn) return false;
        const std::uint64_t idx = lcn - startLcn;
        if (idx >= totalClusters) return false;
        const std::size_t byteIdx = static_cast<std::size_t>(idx >> 3);
        if (byteIdx >= bits.size()) return false;
        return ((bits[byteIdx] >> (idx & 7U)) & 1U) != 0;
    }

    void SetAllocated(Lcn lcn, bool allocated) {
        if (lcn < startLcn) return;
        const std::uint64_t idx = lcn - startLcn;
        if (idx >= totalClusters) return;
        const std::size_t byteIdx = static_cast<std::size_t>(idx >> 3);
        if (byteIdx >= bits.size()) return;
        const std::uint8_t mask = static_cast<std::uint8_t>(1U << (idx & 7U));
        if (allocated) {
            bits[byteIdx] |= mask;
        } else {
            bits[byteIdx] &= static_cast<std::uint8_t>(~mask);
        }
    }
};

enum class DefragMode : std::uint8_t {
    Analyze = 0,
    FullDefrag,
    QuickDefrag,
    DefragFreeSpace,
    MoveLargeToEnd,
    SingleFileOrFolder,
    MftOptimize,
    MftZoneEvacuate,
};

struct MoveResult {
    bool success{false};
    std::uint32_t clustersMoved{0};
    std::uint32_t win32Error{0};
    std::uint32_t errorCodeValue{0}; // Maps to ErrorCode
    bool partial{false};
    SkipReason suggestedSkip{SkipReason::None};
};

} // namespace auralite
