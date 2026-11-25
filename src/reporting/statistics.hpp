#pragma once

#include "interfaces/snapshots.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace auralite {

struct BeforeAfterReport {
    double volumeFragBeforePct{0.0};
    double volumeFragAfterPct{0.0};
    double fileFragBeforePct{0.0};
    double fileFragAfterPct{0.0};
    std::uint64_t fragmentedFilesBefore{0};
    std::uint64_t fragmentedFilesAfter{0};
    ClusterCount largestFreeRunBefore{0};
    ClusterCount largestFreeRunAfter{0};
};

struct FragmentHistogram {
    // Bins: [0]="1", [1]="2-4", [2]="5-16", [3]="17-64", [4]="65+"
    std::array<std::uint64_t, 5> bins{};
    std::uint64_t residentCount{0};
    std::uint64_t totalFiles{0};
};

// What the backend actually saw per file. Only two of these are problems: a
// resident file legitimately has no extents because its data lives inside the MFT
// record, while a file that could not be opened or whose layout could not be read
// is a hole in the report. Conflating them produced a warning about 55,917
// "unreadable" files on a healthy volume.
struct ExtentCensus {
    std::uint64_t singleExtent{0};
    std::uint64_t multiExtent{0};
    std::uint64_t zeroLength{0};
    std::uint64_t resident{0};
    std::uint64_t unreadable{0};
    std::uint64_t locked{0};
    std::uint32_t maxExtents{0};

    // True when part of the volume was not measured, so the percentages are partial.
    bool Blind() const noexcept { return unreadable > 0 || locked > 0; }

    std::string Text() const;
};

struct TopFragmentedEntry {
    FileId id{0};
    std::string name;
    std::string path;
    std::uint64_t sizeBytes{0};
    std::uint32_t fragmentCount{0};
    FileState state{FileState::Ready};
};

struct ThroughputPoint {
    std::uint64_t elapsedMs{0};
    double throughputMBps{0.0};
    ClusterCount clustersMoved{0};
    double etaSeconds{0.0};
};

struct ChartFrame {
    BeforeAfterReport beforeAfter{};
    FragmentHistogram histogram{};
    std::vector<TopFragmentedEntry> topFragmented;
    std::vector<ThroughputPoint> series; // Rolling up to 600 points
};

class StatisticsEngine {
public:
    static BeforeAfterReport ComputeBeforeAfter(const AnalysisSnapshot& before,
                                                const AnalysisSnapshot& after) noexcept;

    static FragmentHistogram ComputeHistogram(const AnalysisSnapshot& snapshot) noexcept;

    static std::vector<TopFragmentedEntry> ComputeTopN(const AnalysisSnapshot& snapshot,
                                                       std::size_t n = 25);

    static ExtentCensus Census(const std::vector<FileEntry>& files);

    static void RecordThroughputSample(std::vector<ThroughputPoint>& series,
                                       const ProgressSnapshot& progress,
                                       std::size_t maxPoints = 600);
};

} // namespace auralite
