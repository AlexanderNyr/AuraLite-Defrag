#pragma once

#include "engine/policy_gate.hpp"
#include "interfaces/i_defrag_planner.hpp"

namespace auralite {

class DefragPlanner final : public IDefragPlanner {
public:
    Result<DefragPlan> Plan(const AnalysisSnapshot& snapshot,
                            const PlanOptions& options,
                            const PolicyGate& policy) override;

    Result<DefragPlan> Replan(const AnalysisSnapshot& snapshot,
                              const DefragPlan& originalPlan,
                              const Checkpoint& checkpoint,
                              const PolicyGate& policy) override;

    static std::uint64_t ComputePlanHash(const DefragPlan& plan) noexcept;
};

} // namespace auralite
