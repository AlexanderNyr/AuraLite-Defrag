// P7-T05 / P7-T06 — release-build timing harness.
//
// The unit tests run under ASan/UBSan, which inflates timings ~10x. The plan's
// budgets (histogram <= 50 ms, Top-N <= 20 ms at 250k files) are stated for a
// release build, so they are measured here, in an ordinary -O2 build.
#include "reporting/statistics.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace auralite;

namespace {

AnalysisSnapshot MakeSnapshot(std::uint32_t fileCount, std::uint32_t seed) {
    AnalysisSnapshot s{};
    s.volumeId = "BENCH";
    s.summary.totalClusters = 1'000'000;
    s.summary.totalFiles = fileCount;
    s.files.reserve(fileCount);
    for (std::uint32_t i = 0; i < fileCount; ++i) {
        FileEntry f{};
        f.id = i + 1;
        f.name = "f" + std::to_string(i) + ".bin";
        f.path = "c:\\data\\f" + std::to_string(i) + ".bin";
        f.sizeBytes = 4096ULL * (((i * 7U + seed) % 500U) + 1U);
        f.allocatedClusters = ((i * 7U + seed) % 500U) + 1U;
        const std::uint32_t b = (i * 13U + seed) % 9U;
        if (b == 0) { f.fragmentCount = 0; f.flags.resident = true; }
        else if (b <= 2) f.fragmentCount = 1;
        else if (b <= 4) f.fragmentCount = 2 + (i % 3);
        else if (b <= 6) f.fragmentCount = 6 + (i % 10);
        else if (b <= 7) f.fragmentCount = 20 + (i % 40);
        else f.fragmentCount = 70 + (i % 90);
        f.extents.push_back({0, static_cast<Lcn>(i) * 4ULL, f.allocatedClusters});
        s.files.push_back(std::move(f));
    }
    return s;
}

template <typename Fn>
double BestMs(Fn&& fn, int reps = 7) {
    double best = 1e18;
    for (int r = 0; r < reps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms < best) best = ms;
    }
    return best;
}

} // namespace

int main() {
    constexpr std::uint32_t kFiles = 250'000;
    const auto snap = MakeSnapshot(kFiles, 5);

    const double histMs = BestMs([&] {
        const auto h = StatisticsEngine::ComputeHistogram(snap);
        volatile std::uint64_t sink = h.totalFiles;
        (void)sink;
    });
    const double top25Ms = BestMs([&] {
        const auto t = StatisticsEngine::ComputeTopN(snap, 25);
        volatile std::size_t sink = t.size();
        (void)sink;
    });
    const double top100Ms = BestMs([&] {
        const auto t = StatisticsEngine::ComputeTopN(snap, 100);
        volatile std::size_t sink = t.size();
        (void)sink;
    });

    std::printf("Reporting benchmarks, %u files (release -O2)\n", kFiles);
    std::printf("  histogram (P7-T05)  %8.2f ms   budget 50.00 ms   %s\n", histMs,
                histMs <= 50.0 ? "PASS" : "FAIL");
    std::printf("  Top-N 25  (P7-T06)  %8.2f ms   budget 20.00 ms   %s\n", top25Ms,
                top25Ms <= 20.0 ? "PASS" : "FAIL");
    std::printf("  Top-N 100 (P7-T06)  %8.2f ms   budget 20.00 ms   %s\n", top100Ms,
                top100Ms <= 20.0 ? "PASS" : "FAIL");

    const bool ok = histMs <= 50.0 && top25Ms <= 20.0 && top100Ms <= 20.0;
    return ok ? 0 : 1;
}
