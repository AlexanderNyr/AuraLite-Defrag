#pragma once
//
// P7-T01 — Chart data model, update rates, threading (R-074).
//
// Responsibility: turn a high-frequency stream of engine progress snapshots
// into a bounded, coalesced sequence of immutable ChartFrame values that the
// UI thread can consume without ever blocking or losing the final frame.
//
// Design contract (plan §3.5, PT-07, P7-T01 acceptance criteria):
//
//   * 10 Hz ±10% delivery over a long operation.
//   * 0 UI-thread blocks > 16 ms — the consumer never holds a lock while
//     formatting or drawing.
//   * 0 dropped FINAL frames — a coalsecing queue must never discard the last
//     sample, because that frame carries the before/after report.
//   * ≤ 64 MB of chart memory at 250k files (bounded series + bounded Top-N).
//
// Threading model:
//   Producer (engine worker)  -> PushProgress() / PushAnalysis()
//   Consumer (UI thread)      -> TryPop() / Drain()
// The queue is a mutex-protected ring of *fixed capacity*. When full, the
// oldest coalescable frame is merged into the next one; frames marked
// `mustDeliver` (the final frame) are never dropped.
//
#include "interfaces/snapshots.hpp"
#include "reporting/statistics.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace auralite::reporting {

// Tunables that encode the plan's numbers in one place.
struct ChartModelConfig {
    // Target publication rate. The engine already coalesces at 10 Hz; the
    // model re-asserts it because the CLI and the scheduler drive the engine
    // at different rates and the UI must not depend on either.
    std::uint32_t targetHz{10};
    // Hard cap on queued frames. The UI drains far faster in practice; this
    // exists so a stalled consumer cannot grow memory without bound.
    std::size_t maxQueuedFrames{32};
    // Rolling throughput series length (600 points ≈ 60 s at 10 Hz).
    std::size_t maxThroughputPoints{600};
    // Top-N list size carried in every frame.
    std::size_t topN{25};
    // Histogram is cheap; the expensive part is the per-file scan, so the
    // histogram+Top-N are recomputed only when an analysis snapshot arrives.
    bool recomputeOnAnalysisOnly{true};

    std::uint32_t ClampHz() const noexcept {
        if (targetHz < 1) return 1;
        if (targetHz > 60) return 60;
        return targetHz;
    }
    std::uint64_t MinFrameIntervalMs() const noexcept {
        return 1000U / ClampHz();
    }
};

// One immutable unit of chart data. Everything the Insights pane needs.
struct ChartFrame {
    std::uint64_t sequence{0};
    std::uint64_t producedAtMs{0};
    ProgressSnapshot progress{};
    BeforeAfterReport beforeAfter{};
    FragmentHistogram histogram{};
    std::vector<TopFragmentedEntry> topFragmented;
    std::vector<ThroughputPoint> series;

    // True for the frame that closes an operation. Guaranteed delivery.
    bool isFinal{false};
    // Set when the model coalesced frames to keep up; the UI may show a hint
    // that the series is decimated rather than silently trusting it.
    bool coalesced{false};
    std::uint32_t coalescedAway{0};

    // Approximate retained bytes, used to assert the ≤64 MB budget in tests.
    std::size_t ApproxBytes() const noexcept;
};

// Statistics for a finished operation, cheap to copy, safe to hand to a
// report writer or a golden-trace generator.
struct ChartSummary {
    BeforeAfterReport beforeAfter{};
    FragmentHistogram histogramBefore{};
    FragmentHistogram histogramAfter{};
    std::vector<TopFragmentedEntry> topFragmentedBefore;
    std::vector<TopFragmentedEntry> topFragmentedAfter;
    std::vector<ThroughputPoint> series;
    std::uint64_t framesProduced{0};
    std::uint64_t framesDroppedCoalesced{0};
    std::uint64_t framesFinalDelivered{0};
};

class ChartDataModel {
public:
    explicit ChartDataModel(ChartModelConfig config = {});

    // ---- Producer side (engine worker thread) ---------------------------

    // Feed a progress snapshot. Returns true when a new queued frame was
    // created (i.e. the rate limiter allowed it), false when the snapshot was
    // absorbed by the pending frame instead.
    bool PushProgress(const ProgressSnapshot& progress);

    // Feed an analysis snapshot (before or after an operation). Recomputes the
    // histogram and Top-N, which are the two chart inputs that need a scan.
    // `setBefore` selects which side of the before/after report is filled.
    void PushAnalysis(const AnalysisSnapshot& analysis, bool setBefore);

    // Mark the operation finished. Enqueues a frame with isFinal=true and
    // flushes any pending coalesced frame first. Guaranteed never to be
    // discarded by backpressure.
    void PushFinal(const ProgressSnapshot& progress);

    // ---- Consumer side (UI thread) --------------------------------------

    // Non-blocking. Returns std::nullopt when the queue is empty.
    bool TryPop(ChartFrame& out);

    // Pop everything currently queued, newest last. Bounded by maxQueuedFrames.
    std::vector<ChartFrame> Drain();

    // Cheap, lock-free-ish queue depth probe for diagnostics.
    std::size_t QueuedCount() const;

    // ---- Reporting ------------------------------------------------------

    // Snapshot of the accumulated statistics for the finished operation.
    ChartSummary Summary() const;

    const ChartModelConfig& Config() const noexcept { return config_; }

    // Reset between operations; keeps configuration.
    void Reset();

private:
    // Must be called with mutex_ held.
    void EnqueueLocked(ChartFrame frame);
    // Merge `older` into `newer` so that no final frame is lost.
    static void MergeInto(ChartFrame& newer, const ChartFrame& older);

    ChartModelConfig config_;
    mutable std::mutex mutex_;

    std::deque<ChartFrame> queue_;
    ChartFrame pending_{};          // accumulating, not yet enqueued
    bool hasPending_{false};

    // Before/after accumulation.
    bool hasBefore_{false};
    bool hasAfter_{false};
    AnalysisSnapshot beforeSnapshot_{};
    AnalysisSnapshot afterSnapshot_{};
    BeforeAfterReport beforeAfter_{};
    FragmentHistogram histogramBefore_{};
    FragmentHistogram histogramAfter_{};
    std::vector<TopFragmentedEntry> topBefore_;
    std::vector<TopFragmentedEntry> topAfter_;

    std::vector<ThroughputPoint> series_;
    std::uint64_t sequence_{0};
    std::uint64_t lastEmitMs_{0};
    std::uint64_t produced_{0};
    std::uint64_t coalescedAway_{0};
    std::uint64_t finalDelivered_{0};
};

} // namespace auralite::reporting
