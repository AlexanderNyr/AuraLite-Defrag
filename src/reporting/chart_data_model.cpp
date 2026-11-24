#include "reporting/chart_data_model.hpp"

#include <algorithm>
#include <utility>

namespace auralite::reporting {

namespace {

// Rough retained-size accounting for the ≤64 MB budget assertion.
std::size_t SeriesBytes(const std::vector<ThroughputPoint>& s) noexcept {
    return s.capacity() * sizeof(ThroughputPoint);
}

std::size_t TopBytes(const std::vector<TopFragmentedEntry>& t) noexcept {
    std::size_t n = t.capacity() * sizeof(TopFragmentedEntry);
    for (const auto& e : t) {
        n += e.name.capacity() + e.path.capacity();
    }
    return n;
}

} // namespace

std::size_t ChartFrame::ApproxBytes() const noexcept {
    return sizeof(ChartFrame) - sizeof(series) - sizeof(topFragmented) +
           SeriesBytes(series) + TopBytes(topFragmented);
}

ChartDataModel::ChartDataModel(ChartModelConfig config) : config_(config) {
    if (config_.maxQueuedFrames == 0) config_.maxQueuedFrames = 1;
    if (config_.maxThroughputPoints == 0) config_.maxThroughputPoints = 1;
}

// ---------------------------------------------------------------------------
// Producer side
// ---------------------------------------------------------------------------

bool ChartDataModel::PushProgress(const ProgressSnapshot& progress) {
    std::lock_guard lock(mutex_);

    // Rolling throughput series is always updated, even when the frame that
    // carries it is coalesced away: the series is the chart's memory.
    StatisticsEngine::RecordThroughputSample(series_, progress, config_.maxThroughputPoints);

    // Rate limiting is a property of *time*, not of whether something is
    // already pending. Deriving it from hasPending_ silently disabled the
    // limiter whenever the previous frame had just been emitted.
    //
    // The very first snapshot must not be rate-limited: there is no previous
    // emission to pace against, and swallowing it would delay the UI's first
    // paint of a running operation.
    const bool isFirstFrame = (produced_ == 0 && !hasPending_ && lastEmitMs_ == 0);
    const bool rateLimited =
        !isFirstFrame && progress.elapsedMs < lastEmitMs_ + config_.MinFrameIntervalMs();

    if (!hasPending_) {
        pending_ = ChartFrame{};
    }
    pending_.progress = progress;
    pending_.series = series_;
    pending_.beforeAfter = beforeAfter_;
    pending_.histogram = hasAfter_ ? histogramAfter_ : histogramBefore_;
    pending_.topFragmented = hasAfter_ ? topAfter_ : topBefore_;
    hasPending_ = true;

    if (rateLimited) {
        // Absorbed into the pending frame; this is coalescing, not loss.
        ++coalescedAway_;
        pending_.coalesced = true;
        ++pending_.coalescedAway;
        return false;
    }

    ChartFrame frame = pending_;
    frame.sequence = ++sequence_;
    // Monotonic timestamps: a caller may replay a snapshot with a stale
    // elapsed value; the delivered sequence must still be non-decreasing.
    frame.producedAtMs = progress.elapsedMs < lastEmitMs_ ? lastEmitMs_ : progress.elapsedMs;
    hasPending_ = false;
    lastEmitMs_ = frame.producedAtMs;
    ++produced_;
    EnqueueLocked(std::move(frame));
    return true;
}

void ChartDataModel::PushAnalysis(const AnalysisSnapshot& analysis, bool setBefore) {
    std::lock_guard lock(mutex_);

    const auto hist = StatisticsEngine::ComputeHistogram(analysis);
    const auto top = StatisticsEngine::ComputeTopN(analysis, config_.topN);

    if (setBefore) {
        beforeSnapshot_ = analysis;
        hasBefore_ = true;
        histogramBefore_ = hist;
        topBefore_ = top;
    } else {
        afterSnapshot_ = analysis;
        hasAfter_ = true;
        histogramAfter_ = hist;
        topAfter_ = top;
    }

    if (hasBefore_ && hasAfter_) {
        beforeAfter_ = StatisticsEngine::ComputeBeforeAfter(beforeSnapshot_, afterSnapshot_);
    }

    if (!hasPending_) {
        pending_ = ChartFrame{};
    }
    pending_.beforeAfter = beforeAfter_;
    pending_.histogram = hasAfter_ ? histogramAfter_ : histogramBefore_;
    pending_.topFragmented = hasAfter_ ? topAfter_ : topBefore_;
    pending_.series = series_;
    hasPending_ = true;

    ChartFrame frame = pending_;
    frame.sequence = ++sequence_;
    frame.producedAtMs = pending_.progress.elapsedMs < lastEmitMs_ ? lastEmitMs_
                                                                  : pending_.progress.elapsedMs;
    hasPending_ = false;
    lastEmitMs_ = frame.producedAtMs;
    ++produced_;
    EnqueueLocked(std::move(frame));
}

void ChartDataModel::PushFinal(const ProgressSnapshot& progress) {
    std::lock_guard lock(mutex_);

    StatisticsEngine::RecordThroughputSample(series_, progress, config_.maxThroughputPoints);

    // Fold any accumulated-but-unemitted state into the final frame so that
    // the before/after report can never be lost to rate limiting.
    if (!hasPending_) {
        pending_ = ChartFrame{};
    }
    pending_.progress = progress;
    pending_.series = series_;
    pending_.beforeAfter = beforeAfter_;
    pending_.histogram = hasAfter_ ? histogramAfter_ : histogramBefore_;
    pending_.topFragmented = hasAfter_ ? topAfter_ : topBefore_;

    ChartFrame frame = pending_;
    frame.sequence = ++sequence_;
    frame.producedAtMs = progress.elapsedMs < lastEmitMs_ ? lastEmitMs_ : progress.elapsedMs;
    frame.isFinal = true;
    hasPending_ = false;
    lastEmitMs_ = frame.producedAtMs;
    ++produced_;
    ++finalDelivered_;
    EnqueueLocked(std::move(frame));
}

// ---------------------------------------------------------------------------
// Consumer side
// ---------------------------------------------------------------------------

bool ChartDataModel::TryPop(ChartFrame& out) {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

std::vector<ChartFrame> ChartDataModel::Drain() {
    std::lock_guard lock(mutex_);
    std::vector<ChartFrame> out;
    out.reserve(queue_.size());
    while (!queue_.empty()) {
        out.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    return out;
}

std::size_t ChartDataModel::QueuedCount() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

ChartSummary ChartDataModel::Summary() const {
    std::lock_guard lock(mutex_);
    ChartSummary s;
    s.beforeAfter = beforeAfter_;
    s.histogramBefore = histogramBefore_;
    s.histogramAfter = histogramAfter_;
    s.topFragmentedBefore = topBefore_;
    s.topFragmentedAfter = topAfter_;
    s.series = series_;
    s.framesProduced = produced_;
    s.framesDroppedCoalesced = coalescedAway_;
    s.framesFinalDelivered = finalDelivered_;
    return s;
}

void ChartDataModel::Reset() {
    std::lock_guard lock(mutex_);
    queue_.clear();
    pending_ = ChartFrame{};
    hasPending_ = false;
    hasBefore_ = false;
    hasAfter_ = false;
    beforeSnapshot_ = AnalysisSnapshot{};
    afterSnapshot_ = AnalysisSnapshot{};
    beforeAfter_ = BeforeAfterReport{};
    histogramBefore_ = FragmentHistogram{};
    histogramAfter_ = FragmentHistogram{};
    topBefore_.clear();
    topAfter_.clear();
    series_.clear();
    sequence_ = 0;
    lastEmitMs_ = 0;
    produced_ = 0;
    coalescedAway_ = 0;
    finalDelivered_ = 0;
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

void ChartDataModel::MergeInto(ChartFrame& newer, const ChartFrame& older) {
    const ChartFrame& latest = newer.progress.elapsedMs >= older.progress.elapsedMs ? newer : older;
    newer.progress = latest.progress;
    // The series already contains the history; keep the longer one.
    if (newer.series.size() < older.series.size()) newer.series = older.series;
    newer.isFinal = newer.isFinal || older.isFinal;
    newer.coalesced = true;
    newer.coalescedAway += older.coalescedAway + 1;
}

void ChartDataModel::EnqueueLocked(ChartFrame frame) {
    if (queue_.size() >= config_.maxQueuedFrames) {
        // Account for the loss here as well as on the frame itself: Summary()
        // must never under-report coalescing, or a stalled UI would look
        // healthy while quietly losing intermediate frames.
        ++coalescedAway_;
        // Backpressure policy: coalesce from the front, never drop a final
        // frame. Merging preserves the newest progress and the union of the
        // series, so a UI that stalls for a moment resumes with correct data.
        if (!queue_.empty()) {
            ChartFrame victim = std::move(queue_.front());
            queue_.pop_front();
            if (!frame.isFinal && victim.isFinal) {
                // The frame being dropped is the more important one: promote it.
                MergeInto(victim, frame);
                frame = std::move(victim);
            } else {
                MergeInto(frame, victim);
            }
        }
    }
    queue_.push_back(std::move(frame));
}

} // namespace auralite::reporting
