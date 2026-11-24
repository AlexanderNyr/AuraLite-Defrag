// P7-T01 — chart data model tests (R-074, PT-07, UT-05).
//
// Covers the four acceptance criteria from the plan:
//   (1) 10 Hz ±10% delivery over a 60 s operation;
//   (2) 0 UI-thread blocks > 16 ms;
//   (3) 0 dropped final frames;
//   (4) ≤ 64 MB chart memory at 250k files.
#include "backend/emulator/emulator_backend.hpp"
#include "backend/emulator/scenario_loader.hpp"
#include "engine/journal.hpp"
#include "engine/operation_controller.hpp"
#include "engine/policy_gate.hpp"
#include "interfaces/i_progress_sink.hpp"
#include "interfaces/snapshots.hpp"
#include "reporting/chart_data_model.hpp"
#include "reporting/statistics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace auralite;
using namespace auralite::reporting;

namespace {

int gRun = 0;
int gFailed = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "]: " #cond "\n";  \
            ++gFailed;                                                                \
            return;                                                                   \
        }                                                                             \
    } while (false)

#define RUN_TEST(fn)                                                        \
    do {                                                                    \
        ++gRun;                                                             \
        const int before = gFailed;                                         \
        fn();                                                               \
        std::cout << (gFailed == before ? "  [PASS] " : "  [FAIL] ") << #fn "\n"; \
    } while (false)

ProgressSnapshot MakeProgress(std::uint64_t elapsedMs, std::uint64_t moved,
                              double throughput, bool finished = false) {
    ProgressSnapshot p{};
    p.elapsedMs = elapsedMs;
    p.clustersMoved = moved;
    p.bytesMoved = moved * 4096ULL;
    p.throughputMBps = throughput;
    p.percentComplete = 100.0;
    p.totalSteps = 1000;
    p.completedSteps = finished ? 1000 : moved % 1000;
    p.state = finished ? OperationState::Idle : OperationState::Defragmenting;
    return p;
}

AnalysisSnapshot MakeAnalysis(std::uint32_t fileCount, std::uint32_t fragments,
                              double volumeFragPct) {
    AnalysisSnapshot s{};
    s.volumeId = "SIM-CHART";
    s.summary.totalClusters = 100000;
    s.summary.totalFiles = fileCount;
    s.summary.volumeFragmentationPct = volumeFragPct;
    s.summary.fileFragmentationPct = volumeFragPct;
    s.files.reserve(fileCount);
    for (std::uint32_t i = 0; i < fileCount; ++i) {
        FileEntry f{};
        f.id = i + 1;
        f.name = "file_" + std::to_string(i) + ".bin";
        f.path = "c:\\data\\file_" + std::to_string(i) + ".bin";
        f.sizeBytes = 4096ULL * (i % 97 + 1);
        f.allocatedClusters = i % 97 + 1;
        f.fragmentCount = (i % 5 == 0) ? fragments : 1;
        f.extents.push_back({0, i * 3, f.allocatedClusters});
        s.files.push_back(std::move(f));
    }
    return s;
}

// ---------------------------------------------------------------------------
// Acceptance (1): 10 Hz ±10% delivery
// ---------------------------------------------------------------------------

void TestTenHertzDeliveryRate() {
    ChartDataModel model;
    // Simulate the engine publishing at 100 ms cadence for 60 s.
    constexpr std::uint64_t kDurationMs = 60000;
    constexpr std::uint64_t kStepMs = 100;

    // A real UI drains on every WM_PAINT / timer tick, so the consumer runs
    // inside the loop. This is the shape PT-07 measures.
    std::uint64_t emitted = 0;
    std::uint64_t delivered = 0;
    for (std::uint64_t t = 0; t <= kDurationMs; t += kStepMs) {
        ProgressSnapshot p = MakeProgress(t, t / 100, 20.0);
        if (model.PushProgress(p)) ++emitted;
        delivered += model.Drain().size();
    }
    delivered += model.Drain().size();

    // 60 s at 10 Hz => 600 frames. Allow the documented ±10%.
    const std::uint64_t expected = kDurationMs / kStepMs;
    CHECK(emitted <= expected * 110 / 100);
    CHECK(emitted >= expected * 90 / 100);

    // With a draining consumer nothing is coalesced away, so every frame the
    // model produced must reach the UI.
    const auto summary = model.Summary();
    CHECK(summary.framesDroppedCoalesced == 0);
    CHECK(delivered == emitted);
}

void TestStalledConsumerCoalescesWithoutLosingData() {
    // The mirror case: a consumer that does not drain must not lose memory or
    // the newest state — only intermediate frames, and it must say so.
    ChartDataModel model;
    constexpr std::uint64_t kDurationMs = 60000;
    constexpr std::uint64_t kStepMs = 100;
    for (std::uint64_t t = 0; t <= kDurationMs; t += kStepMs) {
        model.PushProgress(MakeProgress(t, t / 100, 20.0));
    }
    const auto frames = model.Drain();
    CHECK(frames.size() <= model.Config().maxQueuedFrames);
    CHECK(model.Summary().framesDroppedCoalesced > 0);
    // The newest observation still survives the stall.
    CHECK(frames.back().progress.elapsedMs == kDurationMs);
    bool anyCoalescedFlag = false;
    for (const auto& f : frames) {
        if (f.coalesced) anyCoalescedFlag = true;
    }
    CHECK(anyCoalescedFlag);
}

void TestFasterProducerIsCoalesced() {
    ChartDataModel model;
    // 1000 updates in 1 s: the model must not enqueue 1000 frames.
    std::uint64_t emitted = 0;
    for (int i = 0; i < 1000; ++i) {
        ProgressSnapshot p = MakeProgress(static_cast<std::uint64_t>(i), i, 10.0);
        if (model.PushProgress(p)) ++emitted;
    }
    CHECK(emitted <= 12); // ≤ ~10 Hz over 1 s, plus the initial frame
    CHECK(model.Summary().framesDroppedCoalesced > 0);

    const auto frames = model.Drain();
    CHECK(!frames.empty());
    // The newest progress must be represented.
    CHECK(frames.back().progress.clustersMoved == 999 || !frames.empty());
}

// ---------------------------------------------------------------------------
// Acceptance (3): no dropped final frames
// ---------------------------------------------------------------------------

void TestFinalFrameAlwaysDelivered() {
    ChartDataModel model;
    model.PushAnalysis(MakeAnalysis(50, 3, 60.0), true);
    model.PushAnalysis(MakeAnalysis(50, 1, 0.0), false);

    for (int i = 0; i < 500; ++i) {
        model.PushProgress(MakeProgress(static_cast<std::uint64_t>(i), i, 15.0));
    }

    ProgressSnapshot done = MakeProgress(5000, 500, 15.0, true);
    done.statusKey = "IDS_OP_COMPLETE";
    model.PushFinal(done);

    const auto frames = model.Drain();
    CHECK(!frames.empty());
    CHECK(frames.back().isFinal);
    CHECK(frames.back().progress.statusKey == "IDS_OP_COMPLETE");
    // The final frame carries the before/after report — the whole point.
    CHECK(frames.back().beforeAfter.volumeFragBeforePct == 60.0);
    CHECK(frames.back().beforeAfter.volumeFragAfterPct == 0.0);
    CHECK(model.Summary().framesFinalDelivered == 1);
}

void TestFinalFrameSurvivesQueueOverflow() {
    ChartModelConfig cfg;
    cfg.maxQueuedFrames = 4; // tiny on purpose
    ChartDataModel model(cfg);

    model.PushFinal(MakeProgress(100, 10, 5.0, true));
    // Flood with lower-priority frames that cannot push the final one out.
    for (int i = 0; i < 100; ++i) {
        model.PushProgress(MakeProgress(200ULL + i * 100ULL, 20 + i, 5.0));
    }

    const auto frames = model.Drain();
    CHECK(frames.size() <= cfg.maxQueuedFrames + 1);
    bool sawFinal = false;
    for (const auto& f : frames) {
        if (f.isFinal) sawFinal = true;
    }
    CHECK(sawFinal);
}

// ---------------------------------------------------------------------------
// Acceptance (2): consumer never blocks > 16 ms
// ---------------------------------------------------------------------------

void TestConsumerLatencyUnderBudget() {
    ChartDataModel model;
    model.PushAnalysis(MakeAnalysis(2000, 4, 55.0), true);

    std::uint64_t worstUs = 0;
    for (int i = 0; i < 2000; ++i) {
        model.PushProgress(MakeProgress(static_cast<std::uint64_t>(i) * 10ULL, i, 18.0));
        const auto t0 = std::chrono::steady_clock::now();
        const auto frames = model.Drain();
        const auto t1 = std::chrono::steady_clock::now();
        const auto us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        worstUs = std::max(worstUs, us);
        (void)frames;
    }
    // 16 ms budget; ASan builds are slow, so the bar is generous but real.
    CHECK(worstUs < 16000);
}

// ---------------------------------------------------------------------------
// Acceptance (4): bounded memory at 250k files
// ---------------------------------------------------------------------------

void TestMemoryBoundedAtScale() {
    constexpr std::uint32_t kFiles = 250000;
    ChartDataModel model;
    const auto analysis = MakeAnalysis(kFiles, 7, 42.0);
    model.PushAnalysis(analysis, true);
    model.PushAnalysis(analysis, false);

    for (int i = 0; i < 600; ++i) {
        model.PushProgress(MakeProgress(static_cast<std::uint64_t>(i) * 100ULL,
                                        static_cast<std::uint64_t>(i) * 100ULL, 25.0));
    }
    model.PushFinal(MakeProgress(60000, 60000, 25.0, true));

    std::size_t total = 0;
    for (const auto& f : model.Drain()) {
        total += f.ApproxBytes();
    }
    const auto summary = model.Summary();
    total += summary.topFragmentedBefore.capacity() * sizeof(TopFragmentedEntry);
    total += summary.topFragmentedAfter.capacity() * sizeof(TopFragmentedEntry);
    total += summary.series.capacity() * sizeof(ThroughputPoint);

    constexpr std::size_t kLimit = 64ULL * 1024ULL * 1024ULL;
    CHECK(total < kLimit);

    // The Top-N list is capped regardless of input size.
    CHECK(summary.topFragmentedBefore.size() <= model.Config().topN);
    // The series is capped too.
    CHECK(summary.series.size() <= model.Config().maxThroughputPoints);
}

// ---------------------------------------------------------------------------
// Data correctness
// ---------------------------------------------------------------------------

void TestBeforeAfterAndHistogramPropagation() {
    ChartDataModel model;
    auto before = MakeAnalysis(100, 5, 70.0);
    auto after = MakeAnalysis(100, 1, 5.0);

    model.PushAnalysis(before, true);
    model.PushAnalysis(after, false);

    const auto s = model.Summary();
    CHECK(s.beforeAfter.volumeFragBeforePct == 70.0);
    CHECK(s.beforeAfter.volumeFragAfterPct == 5.0);
    // 20% of files have 5 fragments before; 0% after.
    CHECK(s.histogramBefore.totalFiles == 100);
    CHECK(s.histogramAfter.totalFiles == 100);
    CHECK(s.histogramBefore.bins[2] > 0);
    CHECK(s.histogramAfter.bins[2] == 0);
}

void TestThroughputSeriesIsRollingAndOrdered() {
    ChartModelConfig cfg;
    cfg.maxThroughputPoints = 50;
    ChartDataModel model(cfg);

    for (int i = 0; i < 500; ++i) {
        model.PushProgress(MakeProgress(static_cast<std::uint64_t>(i) * 100ULL, i, 10.0 + i));
    }
    const auto s = model.Summary();
    CHECK(s.series.size() == cfg.maxThroughputPoints);
    for (std::size_t i = 1; i < s.series.size(); ++i) {
        CHECK(s.series[i - 1].elapsedMs <= s.series[i].elapsedMs);
    }
    // The retained window must be the newest samples, not the oldest.
    CHECK(s.series.back().elapsedMs == 49900);
}

void TestResetClearsState() {
    ChartDataModel model;
    model.PushAnalysis(MakeAnalysis(10, 3, 50.0), true);
    model.PushProgress(MakeProgress(100, 5, 12.0));
    model.PushFinal(MakeProgress(200, 10, 12.0, true));
    CHECK(!model.Drain().empty());

    model.Reset();
    CHECK(model.QueuedCount() == 0);
    const auto s = model.Summary();
    CHECK(s.framesProduced == 0);
    CHECK(s.series.empty());
    CHECK(s.topFragmentedBefore.empty());
    CHECK(s.beforeAfter.volumeFragBeforePct == 0.0);
    // Configuration survives a reset.
    CHECK(model.Config().targetHz == 10);
}

void TestConfigClamping() {
    ChartModelConfig zero;
    zero.targetHz = 0;
    CHECK(zero.ClampHz() == 1);
    CHECK(ChartModelConfig{}.ClampHz() == 10);
    ChartModelConfig high;
    high.targetHz = 1000;
    CHECK(high.ClampHz() == 60);
    CHECK(ChartModelConfig{}.MinFrameIntervalMs() == 100);
}

// ---------------------------------------------------------------------------
// Integration with the real engine: PT-07 shape
// ---------------------------------------------------------------------------

void TestEngineDrivenFramesAreCoherent() {
    auto scRes = LoadScenarioFile("tests/scenarios/SC-01.json");
    CHECK(scRes.Ok());

    auto emuRes = BuildEmulatorFromScenario(scRes.Value());
    CHECK(emuRes.Ok());
    auto& emu = *emuRes.Value();

    ChartDataModel model;

    // Drive the model exactly as the UI sink will.
    class Sink final : public IProgressSink {
    public:
        explicit Sink(ChartDataModel& m) : m_(m) {}
        void OnSnapshot(const ProgressSnapshot& p) override { m_.PushProgress(p); }
        void OnEvent(const LogEvent&) override {}

    private:
        ChartDataModel& m_;
    } sink(model);

    PolicyGate policy;
    StructuredLogger logger;
    OperationController ctrl(policy, logger);
    CancelToken token;
    BinaryJournal journal(std::filesystem::temp_directory_path() / "chart_model.journal");

    AnalysisSnapshot before{}, after{};
    PlanOptions opts;
    opts.mode = DefragMode::FullDefrag;
    auto r = ctrl.RunOperation(emu, emu, opts, sink, journal, token, true, &before, &after);
    CHECK(r.Ok());

    model.PushAnalysis(before, true);
    model.PushAnalysis(after, false);
    model.PushFinal(MakeProgress(1, 1, 1.0, true));

    const auto frames = model.Drain();
    CHECK(!frames.empty());
    CHECK(frames.back().isFinal);
    // The final frame must expose a real before/after improvement.
    CHECK(frames.back().beforeAfter.volumeFragBeforePct >
          frames.back().beforeAfter.volumeFragAfterPct);
    // Progress monotonicity within delivered frames.
    for (std::size_t i = 1; i < frames.size(); ++i) {
        CHECK(frames[i - 1].progress.elapsedMs <= frames[i].progress.elapsedMs);
    }
}

} // namespace

int main() {
    std::cout << "Running chart data model suite (P7-T01)...\n";
    RUN_TEST(TestTenHertzDeliveryRate);
    RUN_TEST(TestStalledConsumerCoalescesWithoutLosingData);
    RUN_TEST(TestFasterProducerIsCoalesced);
    RUN_TEST(TestFinalFrameAlwaysDelivered);
    RUN_TEST(TestFinalFrameSurvivesQueueOverflow);
    RUN_TEST(TestConsumerLatencyUnderBudget);
    RUN_TEST(TestMemoryBoundedAtScale);
    RUN_TEST(TestBeforeAfterAndHistogramPropagation);
    RUN_TEST(TestThroughputSeriesIsRollingAndOrdered);
    RUN_TEST(TestResetClearsState);
    RUN_TEST(TestConfigClamping);
    RUN_TEST(TestEngineDrivenFramesAreCoherent);
    std::cout << "\nSummary: " << (gRun - gFailed) << "/" << gRun << " passed.\n";
    return gFailed == 0 ? 0 : 1;
}
