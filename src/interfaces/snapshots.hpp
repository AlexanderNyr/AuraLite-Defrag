#pragma once

#include "core/logger.hpp"
#include "core/types.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace auralite {

enum class OperationState : std::uint8_t {
    Idle = 0,
    Analyzing,
    Defragmenting,
    Paused,
    Error,
    SsdAnalysisOnly,
    Demo,
    NotElevated,
};

struct AnalysisSummary {
    ClusterCount totalClusters{0};
    ClusterCount freeClusters{0};
    ClusterCount usedClusters{0};
    std::uint64_t totalFiles{0};
    std::uint64_t fragmentedFiles{0};
    std::uint64_t totalFragments{0};
    std::uint64_t residentFiles{0};
    std::uint64_t unmovableFiles{0};
    std::uint64_t excludedFiles{0};
    double volumeFragmentationPct{0.0};
    double fileFragmentationPct{0.0};
    ClusterCount largestFreeRunClusters{0};
};

struct AnalysisSnapshot {
    VolumeId volumeId;
    VolumeGeometry geometry{};
    MediaClass mediaClass{MediaClass::Unknown};
    MediaEvidence mediaEvidence{};
    CapabilityBits capabilities{};
    PreflightResult preflight{};
    BitmapSnapshot bitmap{};
    std::vector<FileEntry> files;
    MftInfo mftInfo{};
    MftZoneInfo mftZone{};
    AnalysisSummary summary{};
    std::uint64_t snapshotGeneration{0};
    bool isSimulation{false};
};

struct PlannedMove {
    std::uint64_t stepIndex{0};
    FileId fileId{0};
    std::string fileName;
    Vcn startVcn{0};
    Lcn sourceLcn{0};
    Lcn targetLcn{0};
    ClusterCount clusterCount{0};
    bool isMftMove{false};
    std::uint64_t expectedChecksum{0};
};

struct SkippedPlanItem {
    FileId fileId{0};
    std::string fileName;
    SkipReason reason{SkipReason::None};
};

struct PlanOptions {
    DefragMode mode{DefragMode::FullDefrag};
    bool dryRun{false};
    double fullDefragThresholdPct{15.0};
    std::uint64_t largeFileThresholdBytes{100ULL * 1024ULL * 1024ULL};
    std::uint32_t quickMinFragments{2};
    std::uint64_t quickMinSizeBytes{64ULL * 1024ULL};
    std::vector<FileId> targetFileIds;
    std::string targetFolderPrefix;
    MftTier mftTier{MftTier::Tier3ReportOnly};
    bool mftConfirmed{false};
    // Upper bound on the file count the scan may read. 1,000,000 is the historical
    // default; a smaller value exists for timing a sample of a large volume.
    std::uint64_t maxScanFiles{1000000};
};

struct DefragPlan {
    std::string planId;
    VolumeId volumeId;
    std::uint64_t volumeSerialNumber{0};
    std::uint64_t planHash{0};
    DefragMode mode{DefragMode::FullDefrag};
    bool dryRun{false};
    std::vector<PlannedMove> moves;
    std::vector<SkippedPlanItem> skipped;
    ClusterCount totalClustersToMove{0};
    std::uint64_t estimatedBytesToMove{0};
};

struct ProgressSnapshot {
    OperationState state{OperationState::Idle};
    DefragMode mode{DefragMode::Analyze};
    VolumeId volumeId;
    std::uint64_t totalSteps{0};
    std::uint64_t completedSteps{0};
    ClusterCount totalClusters{0};
    ClusterCount clustersMoved{0};
    std::uint64_t bytesMoved{0};
    double percentComplete{0.0};
    double throughputMBps{0.0};
    double etaSeconds{0.0};
    std::uint64_t elapsedMs{0};
    FileId activeFileId{kInvalidFileId};
    Lcn activeTargetLcn{kInvalidLcn};
    std::string statusKey{"IDS_STATE_IDLE"};
    std::uint64_t skippedCount{0};
    std::uint64_t verifiedCount{0};
    bool isSimulation{false};
};

enum class JournalRecordType : std::uint16_t {
    PlanBegin = 1,
    MoveIntent = 2,
    MoveDone = 3,
    Skip = 4,
    Checkpoint = 5,
    Tail = 6,
};

struct JournalRecord {
    std::uint32_t sequence{0};
    std::uint64_t opId{0};
    JournalRecordType type{JournalRecordType::PlanBegin};
    std::uint64_t stepIndex{0};
    FileId fileId{0};
    Vcn startVcn{0};
    Lcn sourceLcn{0};
    Lcn targetLcn{0};
    ClusterCount clusterCount{0};
    std::uint64_t planHash{0};
    std::uint64_t volumeSerial{0};
    std::uint32_t extraCode{0}; // e.g., SkipReason or status
};

struct Checkpoint {
    std::uint32_t generation{0};
    std::uint64_t opId{0};
    std::uint64_t lastSequence{0};
    std::uint64_t lastCompletedStep{0};
    std::uint64_t byteOffset{0};
    std::uint64_t planHash{0};
    std::uint64_t volumeSerial{0};
    bool sealed{false};
};

class CancelToken {
public:
    void RequestCancel() noexcept { cancelled_.store(true, std::memory_order_release); }
    void RequestPause() noexcept { paused_.store(true, std::memory_order_release); }
    void Resume() noexcept { paused_.store(false, std::memory_order_release); }
    void Reset() noexcept {
        cancelled_.store(false, std::memory_order_release);
        paused_.store(false, std::memory_order_release);
    }

    bool IsCancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }
    bool IsPaused() const noexcept { return paused_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> paused_{false};
};

} // namespace auralite
