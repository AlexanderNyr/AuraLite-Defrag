#pragma once

#include "core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace auralite {

enum class LogLevel : std::uint8_t {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

struct LogEvent {
    std::string tsUtc;
    LogLevel level{LogLevel::Info};
    std::string opId;
    std::string volumeId;
    ErrorCode errorCode{ErrorCode::Ok};
    ErrorStage stage{ErrorStage::None};
    std::uint64_t durationMs{0};
    std::uint64_t clusters{0};
    std::string messageKey;
    std::vector<std::string> arguments;
};

std::string HashPathForPrivacy(std::string_view path);
std::string SanitizeLogField(std::string_view input);

class StructuredLogger {
public:
    StructuredLogger() = default;
    explicit StructuredLogger(std::filesystem::path logFilePath,
                              std::size_t maxFileBytes = 10U * 1024U * 1024U,
                              std::size_t maxRotations = 5U);

    void SetLogFile(std::filesystem::path logFilePath,
                    std::size_t maxFileBytes = 10U * 1024U * 1024U,
                    std::size_t maxRotations = 5U);
    void SetMinLevel(LogLevel level) noexcept;
    void SetVerbosePaths(bool verbose) noexcept;
    bool VerbosePaths() const noexcept;

    void Log(LogEvent event);
    std::vector<LogEvent> RecentEvents(std::size_t maxCount = 200) const;

    static std::string FormatEventJsonLine(const LogEvent& ev);
    static std::string CurrentUtcIso8601();

private:
    void RotateIfNeededLocked(std::size_t incomingBytes);

    mutable std::mutex mutex_;
    std::filesystem::path logFilePath_;
    std::size_t maxFileBytes_{10U * 1024U * 1024U};
    std::size_t maxRotations_{5U};
    LogLevel minLevel_{LogLevel::Info};
    bool verbosePaths_{false};
    std::vector<LogEvent> memoryRing_;
};

} // namespace auralite
