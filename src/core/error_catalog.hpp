#pragma once

#include "core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace auralite {

struct ErrorDescriptor {
    ErrorCode code;
    const char* name;
    ErrorStage stage;
    ErrorSeverity severity;
    bool retryable;
    std::uint8_t maxRetries;
    bool abortOperation;
    const char* userMessageKey;
};

const ErrorDescriptor& DescribeError(ErrorCode code) noexcept;

ErrorInfo MakeError(ErrorCode code,
                    std::string volumeId = {},
                    std::uint64_t fileId = 0,
                    std::uint32_t win32Error = 0,
                    std::string diagnosticDetail = {});

ErrorCode MapWin32ToMoveError(std::uint32_t win32Error) noexcept;
std::string_view ErrorCodeName(ErrorCode code) noexcept;
int MapErrorToCliExitCode(ErrorCode code) noexcept;

} // namespace auralite
