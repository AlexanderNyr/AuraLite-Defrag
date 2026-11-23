#pragma once

#include "core/result.hpp"
#include "core/types.hpp"
#include "interfaces/i_journal.hpp"
#include "interfaces/i_progress_sink.hpp"
#include "interfaces/snapshots.hpp"

#include <string>

namespace auralite {

enum class ExecutionModeId : std::uint8_t {
    Online = 0,
    BootTimeDeferred = 1,
};

class IExecutionMode {
public:
    virtual ~IExecutionMode() = default;

    virtual ExecutionModeId Id() const noexcept = 0;
    virtual CapabilityBits Caps() const = 0;
    virtual Result<void> Execute(const DefragPlan& plan,
                                 IProgressSink& sink,
                                 IJournal& journal,
                                 CancelToken& token) = 0;
};

} // namespace auralite
