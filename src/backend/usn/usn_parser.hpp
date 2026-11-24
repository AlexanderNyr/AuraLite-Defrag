#pragma once
//
// P9-T01 / P3-T05 — platform-independent USN record parsing.
//
// The record walk used to live inline inside RealNtfsBackend::Enumerate() behind
// `#ifdef _WIN32`. That made it the single largest piece of untrusted-input
// handling in the product and simultaneously unreachable by any test, because a
// Linux CI run cannot compile the Win32 backend at all. Nothing about the format
// is Win32-specific: FSCTL_ENUM_USN_DATA returns a byte buffer whose layout is
// fixed by usnrecordv2.h, and walking it is pure byte arithmetic.
//
// Moving it here gives three things:
//   * it can be fuzzed with arbitrary bytes on any platform (P9-T01);
//   * the bounds checks are unit-tested rather than asserted by inspection;
//   * the Win32 backend shrinks to device I/O plus a call to this parser.
//
// Every field in an untrusted buffer is attacker-controlled: RecordLength,
// FileNameOffset and FileNameLength are all checked before use, because a
// malformed buffer is exactly what a corrupted or hostile volume looks like.
//
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace auralite {

// sizeof(USN) — the 8-byte header that precedes the first record.
inline constexpr std::size_t kUsnHeaderSize = 8;
// USN_RECORD_V2 has a fixed 60-byte prefix before the variable-length name.
inline constexpr std::size_t kUsnRecordV2FixedSize = 60;
// USN_RECORD_V3 uses a 128-bit file reference, so the fixed part is larger.
inline constexpr std::size_t kUsnRecordV3FixedSize = 76;

inline constexpr std::uint16_t kUsnMajorVersionV2 = 2;
inline constexpr std::uint16_t kUsnMajorVersionV3 = 3;

// Attribute bits mirrored from winnt.h so this header stays independent of the
// Windows SDK. Values are ABI-fixed by the OS.
inline constexpr std::uint32_t kFileAttributeDirectory = 0x00000010U;
inline constexpr std::uint32_t kFileAttributeCompressed = 0x00000800U;
inline constexpr std::uint32_t kFileAttributeSparseFile = 0x00000200U;
inline constexpr std::uint32_t kFileAttributeEncrypted = 0x00004000U;
inline constexpr std::uint32_t kFileAttributeReparsePoint = 0x00000400U;

struct UsnRecord {
    std::uint16_t majorVersion{0};
    std::uint16_t minorVersion{0};
    std::uint64_t fileReferenceNumber{0};
    std::uint64_t parentFileReferenceNumber{0};
    std::int64_t usn{0};
    std::uint32_t reason{0};
    std::uint32_t fileAttributes{0};
    std::uint32_t recordLength{0};
    std::string name; // UTF-8; empty when the buffer carried no usable name
};

struct UsnParseOutcome {
    std::vector<UsnRecord> records;
    std::uint64_t nextStartFileReferenceNumber{0};
    std::size_t bytesConsumed{0};
    // Records skipped because their header was inconsistent. A non-zero count is
    // not an error: the OS is allowed to grow the buffer between calls, and the
    // original code tolerated that too. It is reported so a caller can notice a
    // volume that is producing garbage at a suspicious rate.
    std::size_t malformedRecords{0};
};

// Parse one FSCTL_ENUM_USN_DATA buffer.
//
// Behaviour contract, all enforced by tests:
//   * never reads outside `buffer`, whatever the embedded lengths claim;
//   * stops at the first record whose RecordLength cannot be trusted;
//   * rejects (skips) records with a bad version, a name that overruns the
//     record, an odd FileNameLength, or invalid UTF-16;
//   * `maxRecords` bounds the output so a 1 MiB buffer full of minimal records
//     cannot allocate without limit;
//   * a buffer shorter than the 8-byte header yields an empty result, not an
//     error — that is a legal "no more data" from the OS.
Result<UsnParseOutcome> ParseUsnBuffer(std::string_view buffer, std::size_t maxRecords);

// Decode UTF-16LE to UTF-8, rejecting unpaired surrogates and overlong forms.
// Exposed separately because it is a fuzz target in its own right: this code
// processes file names taken from a volume that may be corrupt.
Result<std::string> Utf16LeToUtf8(std::string_view bytes);

} // namespace auralite
