#pragma once
//
// ExecutionOutcome is the value type an execution mode returns to its caller.
// It lives in interfaces/ (L0) rather than engine/ (L4) because it is part of
// the IExecutionMode contract: `ExecuteWithOutcome` returns it, the operation
// controller stores it, and the L5 differential oracle consumes it to attach
// engine-reported counters to a normalised run. Keeping it in engine/ forced
// every consumer to reach down into L4, which the layering audit correctly
// flagged.
//
#include "interfaces/snapshots.hpp"

#include <cstdint>
#include <vector>

namespace auralite {

struct ExecutionOutcome {
    std::uint64_t stepsCompleted{0};
    std::uint64_t stepsSkipped{0};
    std::uint64_t stepsVerified{0};
    ClusterCount clustersMoved{0};
    std::uint64_t bytesMoved{0};
    std::vector<SkippedPlanItem> runtimeSkips;
};

} // namespace auralite
