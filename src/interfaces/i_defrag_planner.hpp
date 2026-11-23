#pragma once

#include "core/result.hpp"
#include "interfaces/snapshots.hpp"

namespace auralite {

class PolicyGate;

class IDefragPlanner {
public:
    virtual ~IDefragPlanner() = default;

    virtual Result<DefragPlan> Plan(const AnalysisSnapshot& snapshot,
                                    const PlanOptions& options,
                                    const PolicyGate& policy) = 0;

    virtual Result<DefragPlan> Replan(const AnalysisSnapshot& snapshot,
                                      const DefragPlan& originalPlan,
                                      const Checkpoint& checkpoint,
                                      const PolicyGate& policy) = 0;
};

} // namespace auralite
