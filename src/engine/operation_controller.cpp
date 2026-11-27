#include "engine/operation_controller.hpp"
#include "core/error_catalog.hpp"

#include <algorithm>

namespace auralite {

OperationController::OperationController(PolicyGate& policy, StructuredLogger& logger)
    : policy_(policy), logger_(logger) {}

bool OperationController::IsActionAllowed(DefragMode mode, MediaClass media, bool isElevated, bool isDemo) const noexcept {
    const auto st = State();
    if (st == OperationState::Analyzing || st == OperationState::Defragmenting) {
        return false;
    }
    if (mode == DefragMode::Analyze) {
        return true;
    }
    if (IsDefragBlockedMedia(media)) {
        return false;
    }
    if (!isDemo && !isElevated) {
        return false;
    }
    return true;
}

void OperationController::DismissError() noexcept {
    if (State() == OperationState::Error) {
        state_.store(OperationState::Idle, std::memory_order_release);
    }
}

AnalysisSummary OperationController::ComputeSummary(const VolumeGeometry& geom,
                                                      const BitmapSnapshot& bmp,
                                                      const std::vector<FileEntry>& files,
                                                      const PolicyGate& policy) {
    AnalysisSummary sum{};
    sum.totalClusters = geom.totalClusters;
    sum.freeClusters = geom.freeClusters;
    sum.usedClusters = geom.totalClusters > geom.freeClusters ? (geom.totalClusters - geom.freeClusters) : 0;

    ClusterCount fragmentedClusters = 0;
    for (const auto& f : files) {
        sum.totalFiles += 1;
        if (f.flags.resident || f.allocatedClusters == 0) {
            sum.residentFiles += 1;
        }
        if (f.flags.isMetadata || f.flags.isPageOrHiber || f.flags.isLocked) {
            sum.unmovableFiles += 1;
        }
        if (policy.IsFileExcluded(f)) {
            sum.excludedFiles += 1;
        }
        sum.totalFragments += f.fragmentCount;
        if (f.fragmentCount > 1) {
            sum.fragmentedFiles += 1;
            fragmentedClusters += f.allocatedClusters;
        }
    }

    if (sum.usedClusters > 0) {
        sum.volumeFragmentationPct = (100.0 * static_cast<double>(fragmentedClusters)) /
                                     static_cast<double>(sum.usedClusters);
    }
    if (sum.totalFiles > 0) {
        sum.fileFragmentationPct = (100.0 * static_cast<double>(sum.fragmentedFiles)) /
                                   static_cast<double>(sum.totalFiles);
    }

    ClusterCount curFree = 0;
    ClusterCount maxFree = 0;
    for (ClusterCount i = 0; i < bmp.totalClusters; ++i) {
        if (!bmp.IsAllocated(bmp.startLcn + i)) {
            ++curFree;
            if (curFree > maxFree) maxFree = curFree;
        } else {
            curFree = 0;
        }
    }
    sum.largestFreeRunClusters = maxFree;
    return sum;
}

Result<AnalysisSnapshot> OperationController::Analyze(IVolumeProvider& volume,
                                                      IFileSystemAdapter& fs,
                                                      IProgressSink& sink,
                                                      CancelToken& token,
                                                      std::uint64_t maxFiles) {
    const auto prevState = State();
    if (prevState == OperationState::Analyzing || prevState == OperationState::Defragmenting) {
        return Result<AnalysisSnapshot>::Failure(MakeError(ErrorCode::E_INTERNAL_INVALID_STATE, volume.Id()));
    }

    state_.store(OperationState::Analyzing, std::memory_order_release);
    const auto restoreState = [&](OperationState next) {
        state_.store(next, std::memory_order_release);
    };

    auto geomRes = volume.Geometry();
    if (!geomRes) {
        restoreState(OperationState::Error);
        return Result<AnalysisSnapshot>::Failure(geomRes.Error());
    }
    const auto& geom = geomRes.Value();

    if (token.IsCancelled()) {
        restoreState(OperationState::Idle);
        return Result<AnalysisSnapshot>::Failure(MakeError(ErrorCode::E_INTERNAL_CANCELLED, volume.Id()));
    }

    auto pfRes = volume.Preflight(true, policy_.Config().fullDefragThresholdPct);
    if (!pfRes) {
        restoreState(OperationState::Error);
        return Result<AnalysisSnapshot>::Failure(pfRes.Error());
    }

    auto bmpRes = volume.ReadBitmap(0, geom.totalClusters);
    if (!bmpRes) {
        restoreState(OperationState::Error);
        return Result<AnalysisSnapshot>::Failure(bmpRes.Error());
    }

    if (token.IsCancelled()) {
        restoreState(OperationState::Idle);
        return Result<AnalysisSnapshot>::Failure(MakeError(ErrorCode::E_INTERNAL_CANCELLED, volume.Id()));
    }

    // Enumeration is the long phase on a real volume: one FSCTL per file, minutes
    // for 160k files. Report file counts while it runs, and detach the callback
    // afterwards so a later phase cannot write into a dead sink.
    {
        ProgressSnapshot p{};
        p.mode = DefragMode::Analyze;
        p.volumeId = volume.Id();
        p.state = OperationState::Analyzing;
        p.totalSteps = 0; // total unknown: the count is what enumeration discovers
        p.completedSteps = 0;
        p.isSimulation = volume.IsSimulation();
        sink.OnSnapshot(p);

        fs.SetScanProgressCallback([&sink, &volume](std::uint64_t scanned) {
            ProgressSnapshot progress{};
            progress.mode = DefragMode::Analyze;
            progress.volumeId = volume.Id();
            progress.state = OperationState::Analyzing;
            progress.totalSteps = 0;
            progress.completedSteps = scanned;
            progress.isSimulation = volume.IsSimulation();
            sink.OnSnapshot(progress);
        });
    }

    auto filesRes = fs.Enumerate(maxFiles);
    fs.SetScanProgressCallback(nullptr);
    if (!filesRes) {
        restoreState(OperationState::Error);
        return Result<AnalysisSnapshot>::Failure(filesRes.Error());
    }

    auto mftRes = fs.Mft();
    auto zoneRes = fs.Zone();

    AnalysisSnapshot snap{};
    snap.volumeId = volume.Id();
    snap.geometry = geom;
    snap.mediaClass = volume.Media();
    snap.mediaEvidence = volume.Evidence();
    snap.capabilities = fs.Capabilities();
    snap.preflight = pfRes.Value();
    snap.bitmap = std::move(bmpRes.Value());
    snap.files = std::move(filesRes.Value());
    if (mftRes) snap.mftInfo = mftRes.Value();
    if (zoneRes) snap.mftZone = zoneRes.Value();
    snap.snapshotGeneration = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    snap.isSimulation = volume.IsSimulation();

    for (auto& f : snap.files) {
        if (policy_.IsFileExcluded(f)) {
            f.flags.isExcluded = true;
            f.state = FileState::Excluded;
        } else if (f.flags.isMetadata || f.flags.isPageOrHiber || f.flags.isLocked) {
            f.state = FileState::Unmovable;
        } else if (f.fragmentCount > 1) {
            f.state = FileState::Fragmented;
        } else {
            f.state = FileState::Ready;
        }
    }

    snap.summary = ComputeSummary(snap.geometry, snap.bitmap, snap.files, policy_);

    ProgressSnapshot p{};
    p.mode = DefragMode::Analyze;
    p.volumeId = snap.volumeId;
    p.percentComplete = 100.0;
    p.isSimulation = snap.isSimulation;
    if (snap.isSimulation) {
        restoreState(OperationState::Demo);
        p.state = OperationState::Demo;
        p.statusKey = "IDS_DEMO_BANNER";
    } else if (IsDefragBlockedMedia(snap.mediaClass)) {
        restoreState(OperationState::SsdAnalysisOnly);
        p.state = OperationState::SsdAnalysisOnly;
        p.statusKey = "IDS_SSD_ANALYZE_ONLY";
    } else {
        restoreState(OperationState::Idle);
        p.state = OperationState::Idle;
        p.statusKey = "IDS_STATE_IDLE";
    }
    sink.OnSnapshot(p);

    return Result<AnalysisSnapshot>::Success(std::move(snap));
}

Result<ExecutionOutcome> OperationController::RunOperation(IVolumeProvider& volume,
                                                           IFileSystemAdapter& fs,
                                                           const PlanOptions& options,
                                                           IProgressSink& sink,
                                                           IJournal& journal,
                                                           CancelToken& token,
                                                           bool isElevated,
                                                           AnalysisSnapshot* outBeforeSnapshot,
                                                           AnalysisSnapshot* outAfterSnapshot) {
    auto beforeRes = Analyze(volume, fs, sink, token, options.maxScanFiles);
    if (!beforeRes) return Result<ExecutionOutcome>::Failure(beforeRes.Error());
    const AnalysisSnapshot beforeSnap = beforeRes.Value();
    if (outBeforeSnapshot) *outBeforeSnapshot = beforeSnap;

    if (options.mode == DefragMode::Analyze) {
        return Result<ExecutionOutcome>::Success(ExecutionOutcome{});
    }

    // §7.5 Invariant: no transition into Defragmenting without passing Preflight, Spinning media, and elevation (or simulation)
    auto gate = policy_.CanWriteToVolume(beforeSnap.preflight, options.mode, isElevated, volume.IsSimulation());
    if (!gate) {
        if (gate.Error().code == ErrorCode::E_PREFLIGHT_SSD_BLOCKED) {
            state_.store(OperationState::SsdAnalysisOnly, std::memory_order_release);
        } else if (gate.Error().code == ErrorCode::E_PREFLIGHT_NOT_ELEVATED) {
            state_.store(OperationState::NotElevated, std::memory_order_release);
        } else {
            state_.store(OperationState::Error, std::memory_order_release);
        }
        return Result<ExecutionOutcome>::Failure(gate.Error());
    }

    DefragPlanner planner;
    auto planRes = planner.Plan(beforeSnap, options, policy_);
    if (!planRes) {
        state_.store(OperationState::Error, std::memory_order_release);
        return Result<ExecutionOutcome>::Failure(planRes.Error());
    }

    const std::vector<SkippedPlanItem> plannedSkips = planRes.Value().skipped;
    state_.store(OperationState::Defragmenting, std::memory_order_release);
    OnlineMode executor(volume, fs, policy_, logger_, isElevated, policy_.Config().verifyChecksumsSample);
    auto execRes = executor.ExecuteWithOutcome(planRes.Value(), sink, journal, token, nullptr);
    if (!execRes) {
        state_.store(execRes.Error().code == ErrorCode::E_INTERNAL_CANCELLED ? OperationState::Idle : OperationState::Error,
                     std::memory_order_release);
        return Result<ExecutionOutcome>::Failure(execRes.Error());
    }

    execRes.Value().planSkips = plannedSkips;
    if (outAfterSnapshot && !options.dryRun) {
        state_.store(OperationState::Idle, std::memory_order_release);
        auto afterRes = Analyze(volume, fs, sink, token, options.maxScanFiles);
        if (!afterRes) {
            return Result<ExecutionOutcome>::Failure(afterRes.Error());
        }
        *outAfterSnapshot = std::move(afterRes.Value());
    } else if (outAfterSnapshot) {
        *outAfterSnapshot = beforeSnap;
    }

    state_.store(volume.IsSimulation() ? OperationState::Demo : OperationState::Idle, std::memory_order_release);
    return execRes;
}

Result<ExecutionOutcome> OperationController::ResumeOperation(IVolumeProvider& volume,
                                                              IFileSystemAdapter& fs,
                                                              const DefragPlan& originalPlan,
                                                              IProgressSink& sink,
                                                              IJournal& journal,
                                                              CancelToken& token,
                                                              bool isElevated) {
    auto recRes = journal.Recover();
    if (!recRes) {
        state_.store(OperationState::Error, std::memory_order_release);
        return Result<ExecutionOutcome>::Failure(recRes.Error());
    }

    auto cpRes = journal.LastCheckpoint();
    if (!cpRes) {
        state_.store(OperationState::Error, std::memory_order_release);
        return Result<ExecutionOutcome>::Failure(cpRes.Error());
    }
    if (cpRes.Value().volumeSerial != originalPlan.volumeSerialNumber) {
        state_.store(OperationState::Error, std::memory_order_release);
        return Result<ExecutionOutcome>::Failure(MakeError(ErrorCode::E_JOURNAL_SERIAL_MISMATCH, volume.Id()));
    }
    if (cpRes.Value().planHash != originalPlan.planHash) {
        state_.store(OperationState::Error, std::memory_order_release);
        return Result<ExecutionOutcome>::Failure(MakeError(ErrorCode::E_JOURNAL_PLAN_HASH_MISMATCH, volume.Id()));
    }

    state_.store(OperationState::Defragmenting, std::memory_order_release);
    OnlineMode executor(volume, fs, policy_, logger_, isElevated, policy_.Config().verifyChecksumsSample);
    auto execRes = executor.ExecuteWithOutcome(originalPlan, sink, journal, token, &recRes.Value());
    if (!execRes) {
        state_.store(execRes.Error().code == ErrorCode::E_INTERNAL_CANCELLED ? OperationState::Idle : OperationState::Error,
                     std::memory_order_release);
        return execRes;
    }
    state_.store(volume.IsSimulation() ? OperationState::Demo : OperationState::Idle, std::memory_order_release);
    return execRes;
}

} // namespace auralite
