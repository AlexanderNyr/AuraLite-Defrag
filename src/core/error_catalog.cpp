#include "core/error_catalog.hpp"

#include <array>

namespace auralite {

namespace {

constexpr auto kErrorTable = std::to_array<ErrorDescriptor>({
    {ErrorCode::Ok, "OK", ErrorStage::None, ErrorSeverity::Info, false, 0, false, "IDS_STATE_IDLE"},

    // Pre-flight
    {ErrorCode::E_PREFLIGHT_DIRTY, "E_PREFLIGHT_DIRTY", ErrorStage::Preflight, ErrorSeverity::FatalAbort, false, 0, true, "IDS_DIRTY_VOLUME"},
    {ErrorCode::E_PREFLIGHT_READ_ONLY, "E_PREFLIGHT_READ_ONLY", ErrorStage::Preflight, ErrorSeverity::FatalAbort, false, 0, true, "IDS_PREFLIGHT_READ_ONLY"},
    {ErrorCode::E_PREFLIGHT_LOCKED_BITLOCKER, "E_PREFLIGHT_LOCKED_BITLOCKER", ErrorStage::Preflight, ErrorSeverity::FatalAbort, false, 0, true, "IDS_PREFLIGHT_BITLOCKER"},
    {ErrorCode::E_PREFLIGHT_UNSUPPORTED_FS, "E_PREFLIGHT_UNSUPPORTED_FS", ErrorStage::Preflight, ErrorSeverity::FatalAbort, false, 0, true, "IDS_PREFLIGHT_UNSUPPORTED_FS"},
    {ErrorCode::E_PREFLIGHT_SSD_BLOCKED, "E_PREFLIGHT_SSD_BLOCKED", ErrorStage::Preflight, ErrorSeverity::FatalAbort, false, 0, true, "IDS_SSD_BLOCKED_BODY"},
    {ErrorCode::E_PREFLIGHT_INSUFFICIENT_SPACE, "E_PREFLIGHT_INSUFFICIENT_SPACE", ErrorStage::Preflight, ErrorSeverity::FatalAbort, false, 0, true, "IDS_PREFLIGHT_FAILED_GENERIC"},
    {ErrorCode::E_PREFLIGHT_NOT_ELEVATED, "E_PREFLIGHT_NOT_ELEVATED", ErrorStage::Preflight, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ELEVATION_REQUIRED"},
    {ErrorCode::E_PREFLIGHT_VOLUME_INACCESSIBLE, "E_PREFLIGHT_VOLUME_INACCESSIBLE", ErrorStage::Preflight, ErrorSeverity::FatalAbort, false, 0, true, "IDS_PREFLIGHT_FAILED_GENERIC"},

    // Enumeration
    {ErrorCode::E_ENUM_IO, "E_ENUM_IO", ErrorStage::Enumeration, ErrorSeverity::RecoverableError, true, 1, true, "IDS_ERR_ANALYSIS"},
    {ErrorCode::E_ENUM_ACCESS_DENIED, "E_ENUM_ACCESS_DENIED", ErrorStage::Enumeration, ErrorSeverity::Warning, false, 0, false, "IDS_ERR_ACCESS"},
    {ErrorCode::E_ENUM_MALFORMED_RECORD, "E_ENUM_MALFORMED_RECORD", ErrorStage::Enumeration, ErrorSeverity::Warning, false, 0, false, "IDS_ERR_ANALYSIS"},
    {ErrorCode::E_ENUM_PATH_ESCAPE, "E_ENUM_PATH_ESCAPE", ErrorStage::Enumeration, ErrorSeverity::Warning, false, 0, false, "IDS_ERR_PATH"},

    // Bitmap
    {ErrorCode::E_BITMAP_IO, "E_BITMAP_IO", ErrorStage::Bitmap, ErrorSeverity::RecoverableError, true, 3, true, "IDS_ERR_ANALYSIS"},
    {ErrorCode::E_BITMAP_INVALID_RANGE, "E_BITMAP_INVALID_RANGE", ErrorStage::Bitmap, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_ANALYSIS"},
    {ErrorCode::E_BITMAP_STALE, "E_BITMAP_STALE", ErrorStage::Bitmap, ErrorSeverity::Warning, true, 3, false, "IDS_ERR_STALE_BITMAP"},

    // Move
    {ErrorCode::E_MOVE_STALE_BITMAP, "E_MOVE_STALE_BITMAP", ErrorStage::Move, ErrorSeverity::Warning, true, 3, false, "IDS_ERR_STALE_BITMAP"},
    {ErrorCode::E_MOVE_ACCESS_DENIED, "E_MOVE_ACCESS_DENIED", ErrorStage::Move, ErrorSeverity::Warning, true, 3, false, "IDS_ERR_ACCESS"},
    {ErrorCode::E_MOVE_INVALID_PARAM, "E_MOVE_INVALID_PARAM", ErrorStage::Move, ErrorSeverity::RecoverableError, false, 0, false, "IDS_ERR_MOVE_GENERIC"},
    {ErrorCode::E_MOVE_UNSUPPORTED_STREAM, "E_MOVE_UNSUPPORTED_STREAM", ErrorStage::Move, ErrorSeverity::Warning, false, 0, false, "IDS_ERR_UNSUPPORTED_STREAM"},
    {ErrorCode::E_MOVE_INTERRUPTED, "E_MOVE_INTERRUPTED", ErrorStage::Move, ErrorSeverity::RecoverableError, false, 0, true, "IDS_OP_STOPPED"},
    {ErrorCode::E_MOVE_IO, "E_MOVE_IO", ErrorStage::Move, ErrorSeverity::RecoverableError, true, 3, false, "IDS_ERR_MOVE_GENERIC"},
    {ErrorCode::E_MOVE_LOCKED, "E_MOVE_LOCKED", ErrorStage::Move, ErrorSeverity::Warning, true, 3, false, "IDS_ERR_ACCESS"},
    {ErrorCode::E_MOVE_FILE_VANISHED, "E_MOVE_FILE_VANISHED", ErrorStage::Move, ErrorSeverity::Warning, false, 0, false, "IDS_ERR_FILE_VANISHED"},
    {ErrorCode::E_MOVE_PARTIAL, "E_MOVE_PARTIAL", ErrorStage::Move, ErrorSeverity::Warning, false, 0, false, "IDS_ERR_MOVE_GENERIC"},
    {ErrorCode::E_MOVE_MFT_UNAVAILABLE, "E_MOVE_MFT_UNAVAILABLE", ErrorStage::Move, ErrorSeverity::Warning, false, 0, false, "IDS_MFT_TIER1_FAILED"},

    // Verification
    {ErrorCode::E_VERIFY_EXTENT_MISMATCH, "E_VERIFY_EXTENT_MISMATCH", ErrorStage::Verification, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_VERIFY"},
    {ErrorCode::E_VERIFY_CHECKSUM_MISMATCH, "E_VERIFY_CHECKSUM_MISMATCH", ErrorStage::Verification, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_VERIFY"},
    {ErrorCode::E_VERIFY_READBACK_FAILED, "E_VERIFY_READBACK_FAILED", ErrorStage::Verification, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_VERIFY"},

    // Journal
    {ErrorCode::E_JOURNAL_CORRUPT_HEADER, "E_JOURNAL_CORRUPT_HEADER", ErrorStage::Journal, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_JOURNAL"},
    {ErrorCode::E_JOURNAL_CRC_MISMATCH, "E_JOURNAL_CRC_MISMATCH", ErrorStage::Journal, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_JOURNAL"},
    {ErrorCode::E_JOURNAL_IO, "E_JOURNAL_IO", ErrorStage::Journal, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_JOURNAL"},
    {ErrorCode::E_JOURNAL_PLAN_HASH_MISMATCH, "E_JOURNAL_PLAN_HASH_MISMATCH", ErrorStage::Journal, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_JOURNAL"},
    {ErrorCode::E_JOURNAL_SERIAL_MISMATCH, "E_JOURNAL_SERIAL_MISMATCH", ErrorStage::Journal, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_JOURNAL"},
    {ErrorCode::E_JOURNAL_RECOVERED, "E_JOURNAL_RECOVERED", ErrorStage::Journal, ErrorSeverity::Info, false, 0, false, "IDS_JOURNAL_RECOVERY"},

    // Config
    {ErrorCode::E_CONFIG_PARSE, "E_CONFIG_PARSE", ErrorStage::Config, ErrorSeverity::RecoverableError, false, 0, true, "IDS_ERR_CONFIG"},
    {ErrorCode::E_CONFIG_SCHEMA, "E_CONFIG_SCHEMA", ErrorStage::Config, ErrorSeverity::RecoverableError, false, 0, true, "IDS_ERR_CONFIG"},
});

constexpr auto kExtraErrorTable = std::to_array<ErrorDescriptor>({
    {ErrorCode::E_CONFIG_SIZE_LIMIT, "E_CONFIG_SIZE_LIMIT", ErrorStage::Config, ErrorSeverity::RecoverableError, false, 0, true, "IDS_ERR_CONFIG"},
    {ErrorCode::E_CONFIG_DEPTH_LIMIT, "E_CONFIG_DEPTH_LIMIT", ErrorStage::Config, ErrorSeverity::RecoverableError, false, 0, true, "IDS_ERR_CONFIG"},
    {ErrorCode::E_CONFIG_PATH_TRAVERSAL, "E_CONFIG_PATH_TRAVERSAL", ErrorStage::Config, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_PATH"},
    {ErrorCode::E_CONFIG_IO, "E_CONFIG_IO", ErrorStage::Config, ErrorSeverity::RecoverableError, false, 0, true, "IDS_ERR_CONFIG"},

    // Export
    {ErrorCode::E_EXPORT_IO, "E_EXPORT_IO", ErrorStage::Export, ErrorSeverity::RecoverableError, false, 0, false, "IDS_ERR_EXPORT"},
    {ErrorCode::E_EXPORT_WIC, "E_EXPORT_WIC", ErrorStage::Export, ErrorSeverity::RecoverableError, false, 0, false, "IDS_ERR_EXPORT"},
    {ErrorCode::E_EXPORT_INVALID_PATH, "E_EXPORT_INVALID_PATH", ErrorStage::Export, ErrorSeverity::RecoverableError, false, 0, false, "IDS_ERR_PATH"},

    // Internal
    {ErrorCode::E_INTERNAL_INVALID_STATE, "E_INTERNAL_INVALID_STATE", ErrorStage::Internal, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_INTERNAL"},
    {ErrorCode::E_INTERNAL_HANDLE, "E_INTERNAL_HANDLE", ErrorStage::Internal, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_INTERNAL"},
    {ErrorCode::E_INTERNAL_CANCELLED, "E_INTERNAL_CANCELLED", ErrorStage::Internal, ErrorSeverity::Info, false, 0, true, "IDS_OP_CANCELLED"},
});

constexpr ErrorDescriptor kVolumeRemovedDesc{
    ErrorCode::E_INTERNAL_VOLUME_REMOVED, "E_INTERNAL_VOLUME_REMOVED", ErrorStage::Internal, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_VOLUME_REMOVED"
};
constexpr ErrorDescriptor kPolicyViolationDesc{
    ErrorCode::E_INTERNAL_POLICY_VIOLATION, "E_INTERNAL_POLICY_VIOLATION", ErrorStage::Internal, ErrorSeverity::FatalAbort, false, 0, true, "IDS_SSD_BLOCKED_BODY"
};
constexpr ErrorDescriptor kFallbackDesc{
    ErrorCode::E_INTERNAL_INVALID_STATE, "E_UNKNOWN", ErrorStage::Internal, ErrorSeverity::FatalAbort, false, 0, true, "IDS_ERR_INTERNAL"
};

} // namespace

const ErrorDescriptor& DescribeError(ErrorCode code) noexcept {
    for (const auto& entry : kErrorTable) {
        if (entry.code == code) return entry;
    }
    for (const auto& entry : kExtraErrorTable) {
        if (entry.code == code) return entry;
    }
    if (code == ErrorCode::E_INTERNAL_VOLUME_REMOVED) return kVolumeRemovedDesc;
    if (code == ErrorCode::E_INTERNAL_POLICY_VIOLATION) return kPolicyViolationDesc;
    return kFallbackDesc;
}

ErrorInfo MakeError(ErrorCode code,
                    std::string volumeId,
                    std::uint64_t fileId,
                    std::uint32_t win32Error,
                    std::string diagnosticDetail) {
    const auto& d = DescribeError(code);
    ErrorInfo info{};
    info.code = code;
    info.win32Error = win32Error;
    info.hresult = win32Error != 0 ? static_cast<std::int32_t>(0x80070000U | (win32Error & 0xFFFFU)) : 0;
    info.stage = d.stage;
    info.severity = d.severity;
    info.volumeId = std::move(volumeId);
    info.fileId = fileId;
    info.retryable = d.retryable;
    info.maxRetries = d.maxRetries;
    info.abortOperation = d.abortOperation;
    info.userMessageKey = d.userMessageKey;
    info.diagnosticDetail = std::move(diagnosticDetail);
    return info;
}

ErrorCode MapWin32ToMoveError(std::uint32_t win32Error) noexcept {
    // Standard Win32 error codes from FSCTL_MOVE_FILE / file I/O
    switch (win32Error) {
        case 2:   // ERROR_FILE_NOT_FOUND
        case 3:   // ERROR_PATH_NOT_FOUND
            return ErrorCode::E_MOVE_FILE_VANISHED;
        case 5:   // ERROR_ACCESS_DENIED
            return ErrorCode::E_MOVE_ACCESS_DENIED;
        case 32:  // ERROR_SHARING_VIOLATION
        case 33:  // ERROR_LOCK_VIOLATION
            return ErrorCode::E_MOVE_LOCKED;
        case 87:  // ERROR_INVALID_PARAMETER
            return ErrorCode::E_MOVE_INVALID_PARAM;
        case 50:  // ERROR_NOT_SUPPORTED
        case 1:   // ERROR_INVALID_FUNCTION
            return ErrorCode::E_MOVE_UNSUPPORTED_STREAM;
        case 112: // ERROR_DISK_FULL
        case 183: // ERROR_ALREADY_EXISTS (cluster collision / stale bitmap)
            return ErrorCode::E_MOVE_STALE_BITMAP;
        case 21:  // ERROR_NOT_READY
        case 1167:// ERROR_DEVICE_NOT_CONNECTED
            return ErrorCode::E_INTERNAL_VOLUME_REMOVED;
        default:
            return ErrorCode::E_MOVE_IO;
    }
}

std::string_view ErrorCodeName(ErrorCode code) noexcept {
    return DescribeError(code).name;
}

int MapErrorToCliExitCode(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:
            return 0;
        case ErrorCode::E_PREFLIGHT_SSD_BLOCKED:
        case ErrorCode::E_INTERNAL_POLICY_VIOLATION:
            return 12; // Documented SSD / policy refusal exit code
        case ErrorCode::E_PREFLIGHT_NOT_ELEVATED:
            return 13;
        case ErrorCode::E_PREFLIGHT_DIRTY:
        case ErrorCode::E_PREFLIGHT_READ_ONLY:
        case ErrorCode::E_PREFLIGHT_LOCKED_BITLOCKER:
        case ErrorCode::E_PREFLIGHT_UNSUPPORTED_FS:
        case ErrorCode::E_PREFLIGHT_INSUFFICIENT_SPACE:
        case ErrorCode::E_PREFLIGHT_VOLUME_INACCESSIBLE:
            return 11;
        case ErrorCode::E_VERIFY_EXTENT_MISMATCH:
        case ErrorCode::E_VERIFY_CHECKSUM_MISMATCH:
        case ErrorCode::E_VERIFY_READBACK_FAILED:
            return 20;
        case ErrorCode::E_JOURNAL_CORRUPT_HEADER:
        case ErrorCode::E_JOURNAL_CRC_MISMATCH:
        case ErrorCode::E_JOURNAL_IO:
        case ErrorCode::E_JOURNAL_PLAN_HASH_MISMATCH:
        case ErrorCode::E_JOURNAL_SERIAL_MISMATCH:
            return 21;
        case ErrorCode::E_CONFIG_PARSE:
        case ErrorCode::E_CONFIG_SCHEMA:
        case ErrorCode::E_CONFIG_SIZE_LIMIT:
        case ErrorCode::E_CONFIG_DEPTH_LIMIT:
        case ErrorCode::E_CONFIG_PATH_TRAVERSAL:
        case ErrorCode::E_CONFIG_IO:
            return 2;
        case ErrorCode::E_INTERNAL_CANCELLED:
            return 3;
        default:
            return 1;
    }
}

} // namespace auralite
