#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace auralite {

enum class ErrorCode : std::uint32_t {
    Ok = 0,

    // Pre-flight errors
    E_PREFLIGHT_DIRTY = 1001,
    E_PREFLIGHT_READ_ONLY = 1002,
    E_PREFLIGHT_LOCKED_BITLOCKER = 1003,
    E_PREFLIGHT_UNSUPPORTED_FS = 1004,
    E_PREFLIGHT_SSD_BLOCKED = 1005,
    E_PREFLIGHT_INSUFFICIENT_SPACE = 1006,
    E_PREFLIGHT_NOT_ELEVATED = 1007,
    E_PREFLIGHT_VOLUME_INACCESSIBLE = 1008,

    // Enumeration errors
    E_ENUM_IO = 2001,
    E_ENUM_ACCESS_DENIED = 2002,
    E_ENUM_MALFORMED_RECORD = 2003,
    E_ENUM_PATH_ESCAPE = 2004,

    // Bitmap errors
    E_BITMAP_IO = 3001,
    E_BITMAP_INVALID_RANGE = 3002,
    E_BITMAP_STALE = 3003,

    // Move errors
    E_MOVE_STALE_BITMAP = 4001,
    E_MOVE_ACCESS_DENIED = 4002,
    E_MOVE_INVALID_PARAM = 4003,
    E_MOVE_UNSUPPORTED_STREAM = 4004,
    E_MOVE_INTERRUPTED = 4005,
    E_MOVE_IO = 4006,
    E_MOVE_LOCKED = 4007,
    E_MOVE_FILE_VANISHED = 4008,
    E_MOVE_PARTIAL = 4009,
    E_MOVE_MFT_UNAVAILABLE = 4010,

    // Verification errors (always abort operation + seal journal)
    E_VERIFY_EXTENT_MISMATCH = 5001,
    E_VERIFY_CHECKSUM_MISMATCH = 5002,
    E_VERIFY_READBACK_FAILED = 5003,

    // Journal errors (always abort operation + seal journal)
    E_JOURNAL_CORRUPT_HEADER = 6001,
    E_JOURNAL_CRC_MISMATCH = 6002,
    E_JOURNAL_IO = 6003,
    E_JOURNAL_PLAN_HASH_MISMATCH = 6004,
    E_JOURNAL_SERIAL_MISMATCH = 6005,
    E_JOURNAL_RECOVERED = 6006,

    // Config / scenario / input errors
    E_CONFIG_PARSE = 7001,
    E_CONFIG_SCHEMA = 7002,
    E_CONFIG_SIZE_LIMIT = 7003,
    E_CONFIG_DEPTH_LIMIT = 7004,
    E_CONFIG_PATH_TRAVERSAL = 7005,
    E_CONFIG_IO = 7006,

    // Export errors
    E_EXPORT_IO = 8001,
    E_EXPORT_WIC = 8002,
    E_EXPORT_INVALID_PATH = 8003,

    // Internal / platform errors
    E_INTERNAL_INVALID_STATE = 9001,
    E_INTERNAL_HANDLE = 9002,
    E_INTERNAL_CANCELLED = 9003,
    E_INTERNAL_VOLUME_REMOVED = 9004,
    E_INTERNAL_POLICY_VIOLATION = 9005,
};

enum class ErrorSeverity : std::uint8_t {
    Info = 0,
    Warning = 1,
    RecoverableError = 2,
    FatalAbort = 3,
};

enum class ErrorStage : std::uint8_t {
    None = 0,
    Preflight,
    Enumeration,
    Bitmap,
    Planning,
    Move,
    Verification,
    Journal,
    Config,
    Export,
    Internal,
};

struct ErrorInfo {
    ErrorCode code{ErrorCode::Ok};
    std::uint32_t win32Error{0};
    std::int32_t hresult{0};
    ErrorStage stage{ErrorStage::None};
    ErrorSeverity severity{ErrorSeverity::Info};
    std::string volumeId;
    std::uint64_t fileId{0};
    bool retryable{false};
    std::uint8_t maxRetries{0};
    bool abortOperation{false};
    std::string userMessageKey;
    std::string diagnosticDetail; // Never contains raw user paths in logs
};

template <typename T>
class Result {
public:
    static Result<T> Success(T value) {
        Result<T> r;
        r.value_.emplace(std::move(value));
        return r;
    }

    static Result<T> Failure(ErrorInfo err) {
        Result<T> r;
        r.error_ = std::move(err);
        return r;
    }

    bool Ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return Ok(); }

    const T& Value() const& { return *value_; }
    T& Value() & { return *value_; }
    T&& Value() && { return std::move(*value_); }

    const ErrorInfo& Error() const noexcept { return error_; }

private:
    std::optional<T> value_;
    ErrorInfo error_{};
};

template <>
class Result<void> {
public:
    static Result<void> Success() {
        Result<void> r;
        r.ok_ = true;
        return r;
    }

    static Result<void> Failure(ErrorInfo err) {
        Result<void> r;
        r.ok_ = false;
        r.error_ = std::move(err);
        return r;
    }

    bool Ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    const ErrorInfo& Error() const noexcept { return error_; }

private:
    bool ok_{false};
    ErrorInfo error_{};
};

} // namespace auralite
