#pragma once

#include "core/logger.hpp"
#include "engine/policy_gate.hpp"
#include "interfaces/execution_outcome.hpp"
#include "interfaces/i_execution_mode.hpp"
#include "interfaces/i_filesystem_adapter.hpp"
#include "interfaces/i_volume_provider.hpp"

#include <cstdint>
#include <vector>

namespace auralite {

class OnlineMode final : public IExecutionMode {
public:
    OnlineMode(IVolumeProvider& volume,
               IFileSystemAdapter& fs,
               const PolicyGate& policy,
               StructuredLogger& logger,
               bool isElevated = true,
               bool verifyChecksumsSample = true);

    ExecutionModeId Id() const noexcept override { return ExecutionModeId::Online; }
    CapabilityBits Caps() const override;

    Result<void> Execute(const DefragPlan& plan,
                         IProgressSink& sink,
                         IJournal& journal,
                         CancelToken& token) override;

    Result<ExecutionOutcome> ExecuteWithOutcome(const DefragPlan& plan,
                                                IProgressSink& sink,
                                                IJournal& journal,
                                                CancelToken& token,
                                                const std::vector<JournalRecord>* recoveredRecords = nullptr);

private:
    bool VerifyMovedExtent(FileId fileId,
                           Vcn startVcn,
                           Lcn expectedTargetLcn,
                           ClusterCount count,
                           std::uint64_t expectedChecksum,
                           bool checkChecksum,
                           ErrorCode& outVerifyError);

    IVolumeProvider& volume_;
    IFileSystemAdapter& fs_;
    const PolicyGate& policy_;
    StructuredLogger& logger_;
    bool isElevated_{true};
    bool verifyChecksumsSample_{true};
};

} // namespace auralite
