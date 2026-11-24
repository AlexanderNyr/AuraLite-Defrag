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
    std::vector<TopFragmentedEntry> out;
    if (n == 0) return out;

    // P7-T06 acceptance (3): recompute <= 20 ms at 250k files. The previous
    // implementation materialised a TopFragmentedEntry (two std::string copies)
    // for every fragmented file and then fully sorted them, which cost ~480 ms
    // at 250k files. Ranking only needs the sort keys, so rank lightweight
    // references first and copy strings for the winners alone.
    std::vector<const FileEntry*> candidates;
    candidates.reserve(snapshot.files.size());
    for (const auto& f : snapshot.files) {
        if (f.fragmentCount > 1) candidates.push_back(&f);
    }
    if (candidates.empty()) return out;

    const auto better = [](const FileEntry* a, const FileEntry* b) noexcept {
        if (a->fragmentCount != b->fragmentCount) return a->fragmentCount > b->fragmentCount;
        if (a->sizeBytes != b->sizeBytes) return a->sizeBytes > b->sizeBytes;
        return a->id < b->id;
    };

    const std::size_t take = candidates.size() < n ? candidates.size() : n;
    if (take < candidates.size()) {
        // partial_sort: O(m log n) instead of O(m log m).
        std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(take),
                          candidates.end(), better);
    } else {
        std::sort(candidates.begin(), candidates.end(), better);
    }

    out.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
        const FileEntry& f = *candidates[i];
        out.push_back({f.id, f.name, f.path, f.sizeBytes, f.fragmentCount, f.state});
    }
    return out;
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
