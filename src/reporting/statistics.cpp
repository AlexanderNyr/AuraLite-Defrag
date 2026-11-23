#include "reporting/statistics.hpp"

#include <algorithm>

namespace auralite {

BeforeAfterReport StatisticsEngine::ComputeBeforeAfter(const AnalysisSnapshot& before,
                                                       const AnalysisSnapshot& after) noexcept {
    BeforeAfterReport r{};
    r.volumeFragBeforePct = before.summary.volumeFragmentationPct;
    r.volumeFragAfterPct = after.summary.volumeFragmentationPct;
    r.fileFragBeforePct = before.summary.fileFragmentationPct;
    r.fileFragAfterPct = after.summary.fileFragmentationPct;
    r.fragmentedFilesBefore = before.summary.fragmentedFiles;
    r.fragmentedFilesAfter = after.summary.fragmentedFiles;
    r.largestFreeRunBefore = before.summary.largestFreeRunClusters;
    r.largestFreeRunAfter = after.summary.largestFreeRunClusters;
    return r;
}

FragmentHistogram StatisticsEngine::ComputeHistogram(const AnalysisSnapshot& snapshot) noexcept {
    FragmentHistogram h{};
    for (const auto& f : snapshot.files) {
        h.totalFiles += 1;
        if (f.flags.resident || f.fragmentCount == 0) {
            h.residentCount += 1;
            continue;
        }
        const std::uint32_t fr = f.fragmentCount;
        if (fr == 1) h.bins[0] += 1;
        else if (fr <= 4) h.bins[1] += 1;
        else if (fr <= 16) h.bins[2] += 1;
        else if (fr <= 64) h.bins[3] += 1;
        else h.bins[4] += 1;
    }
    return h;
}

std::vector<TopFragmentedEntry> StatisticsEngine::ComputeTopN(const AnalysisSnapshot& snapshot,
                                                              std::size_t n) {
    std::vector<TopFragmentedEntry> entries;
    entries.reserve(snapshot.files.size());
    for (const auto& f : snapshot.files) {
        if (f.fragmentCount > 1) {
            entries.push_back({f.id, f.name, f.path, f.sizeBytes, f.fragmentCount, f.state});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const TopFragmentedEntry& a, const TopFragmentedEntry& b) {
        if (a.fragmentCount != b.fragmentCount) return a.fragmentCount > b.fragmentCount;
        if (a.sizeBytes != b.sizeBytes) return a.sizeBytes > b.sizeBytes;
        return a.id < b.id;
    });
    if (entries.size() > n) {
        entries.resize(n);
    }
    return entries;
}

void StatisticsEngine::RecordThroughputSample(std::vector<ThroughputPoint>& series,
                                              const ProgressSnapshot& progress,
                                              std::size_t maxPoints) {
    if (maxPoints == 0) return;
    if (series.size() >= maxPoints) {
        series.erase(series.begin());
    }
    series.push_back({progress.elapsedMs, progress.throughputMBps, progress.clustersMoved, progress.etaSeconds});
}

} // namespace auralite
