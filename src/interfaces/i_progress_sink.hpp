#pragma once

#include "core/logger.hpp"
#include "interfaces/snapshots.hpp"

namespace auralite {

class IProgressSink {
public:
    virtual ~IProgressSink() = default;

    virtual void OnSnapshot(const ProgressSnapshot& snapshot) = 0;
    virtual void OnEvent(const LogEvent& event) = 0;
};

} // namespace auralite
