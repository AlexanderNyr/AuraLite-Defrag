#pragma once

#include "core/logger.hpp"
#include "engine/online_mode.hpp"
#include "engine/planner.hpp"
#include "engine/policy_gate.hpp"
#include "interfaces/i_filesystem_adapter.hpp"
#include "interfaces/i_journal.hpp"
#include "interfaces/i_progress_sink.hpp"
#include "interfaces/i_volume_provider.hpp"
#include "interfaces/snapshots.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace auralite {

class OperationController {
public:
    OperationController(PolicyGate& policy, StructuredLogger& logger);

    OperationState State() const noexcept { return state_.load(std::memory_order_acquire); }
    bool IsActionAllowed(DefragMode mode, MediaClass media, bool isElevated, bool isDemo) const noexcept;

    Result<AnalysisSnapshot> Analyze(IVolumeProvider& volume,
                                     IFileSystemAdapter& fs,
                                     IProgressSink& sink,
                                     CancelToken& token,
                                     std::uint64_t maxFiles = 1000000);

    Result<ExecutionOutcome> RunOperation(IVolumeProvider& volume,
                                          IFileSystemAdapter& fs,
                                          const PlanOptions& options,
                                          IProgressSink& sink,
                                          IJournal& journal,
                                          CancelToken& token,
                                          bool isElevated,
                                          AnalysisSnapshot* outBeforeSnapshot = nullptr,
                                          AnalysisSnapshot* outAfterSnapshot = nullptr);

    Result<ExecutionOutcome> ResumeOperation(IVolumeProvider& volume,
                                             IFileSystemAdapter& fs,
                                             const DefragPlan& originalPlan,
                                             IProgressSink& sink,
                                             IJournal& journal,
                                             CancelToken& token,
                                             bool isElevated);

    void SetNotElevated() noexcept { state_.store(OperationState::NotElevated, std::memory_order_release); }
    void DismissError() noexcept;

private:
    static AnalysisSummary ComputeSummary(const VolumeGeometry& geom,
                                          const BitmapSnapshot& bmp,
                                          const std::vector<FileEntry>& files,
                                          const PolicyGate& policy);

    PolicyGate& policy_;
    StructuredLogger& logger_;
    std::atomic<OperationState> state_{OperationState::Idle};
    std::atomic<std::uint64_t> generation_{0};
};

} // namespace auralite
