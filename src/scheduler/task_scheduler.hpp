#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <string>

namespace auralite {

enum class ScheduleCadence : std::uint8_t {
    Daily = 0,
    Weekly = 1,
    OnIdle = 2,
};

struct ScheduledTaskSpec {
    std::string taskName{"AuraLite-Defrag-Scheduled"};
    VolumeId volumeId;
    DefragMode mode{DefragMode::QuickDefrag};
    ScheduleCadence cadence{ScheduleCadence::Weekly};
    std::string startBoundaryIso{"2026-12-01T03:00:00"};
    std::string cliExecutablePath;
    MediaClass mediaClass{MediaClass::Unknown};
};

class TaskSchedulerManager {
public:
    // Enforces SSD/Unknown refusal before even touching Task Scheduler COM (P8-T01 / P8-T09)
    static Result<void> RegisterTask(const ScheduledTaskSpec& spec);
    static Result<void> RemoveTask(const std::string& taskName);
};

} // namespace auralite
