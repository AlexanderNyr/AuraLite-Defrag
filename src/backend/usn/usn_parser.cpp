#include "backend/usn/usn_parser.hpp"
#include "core/error_catalog.hpp"

#include <cstring>

namespace auralite {

namespace {

std::uint16_t ReadU16(const char* p) noexcept {
    std::uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

std::uint32_t ReadU32(const char* p) noexcept {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

std::uint64_t ReadU64(const char* p) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// Reject 0x0000 and every non-character the Unicode standard forbids in a file
// name; a name containing them is a sign of a corrupt buffer, and Windows itself
// refuses such names at the filesystem boundary.
bool IsPlausibleFileNameChar(std::uint32_t cp) noexcept {
    if (cp == 0) return false;
    if (cp < 0x20) return false;
    if (cp == 0x7F) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false; // unpaired surrogate
    return true;
}

std::size_t AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
        return 1;
    }
    if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return 3;
    }
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    return 4;
}

} // namespace

Result<std::string> Utf16LeToUtf8(std::string_view bytes) {
    if ((bytes.size() % 2) != 0) {
        return Result<std::string>::Failure(MakeError(
            ErrorCode::E_ENUM_IO, {}, 0, 0, "UTF-16LE input has an odd byte count"));
    }

    std::string out;
    out.reserve(bytes.size()); // UTF-8 never exceeds 3 bytes per UTF-16 unit

    std::size_t i = 0;
    while (i + 1 < bytes.size() || (i + 1 == bytes.size())) {
        if (i + 2 > bytes.size()) break;
        const std::uint32_t unit =
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
        i += 2;

        std::uint32_t cp = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            // High surrogate: must be followed by a low surrogate.
            if (i + 2 > bytes.size()) {
                return Result<std::string>::Failure(MakeError(
                    ErrorCode::E_ENUM_IO, {}, 0, 0, "UTF-16LE: truncated surrogate pair"));
            }
            const std::uint32_t low =
                static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
            if (low < 0xDC00 || low > 0xDFFF) {
                return Result<std::string>::Failure(MakeError(
                    ErrorCode::E_ENUM_IO, {}, 0, 0, "UTF-16LE: unpaired high surrogate"));
            }
            i += 2;
            cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return Result<std::string>::Failure(MakeError(
                ErrorCode::E_ENUM_IO, {}, 0, 0, "UTF-16LE: unpaired low surrogate"));
        }

        if (!IsPlausibleFileNameChar(cp)) {
            return Result<std::string>::Failure(MakeError(
                ErrorCode::E_ENUM_IO, {}, 0, 0, "UTF-16LE: implausible file-name character"));
        }
        AppendUtf8(out, cp);
    }
    return Result<std::string>::Success(std::move(out));
}

Result<UsnParseOutcome> ParseUsnBuffer(std::string_view buffer, std::size_t maxRecords) {
    UsnParseOutcome out;

    // A buffer shorter than the 8-byte USN header means the OS had nothing more
    // to give; that is a normal end-of-enumeration, not a malformed volume.
    if (buffer.size() < kUsnHeaderSize) {
        return Result<UsnParseOutcome>::Success(std::move(out));
    }

    out.nextStartFileReferenceNumber = ReadU64(buffer.data());

    std::size_t offset = kUsnHeaderSize;
    while (offset + kUsnRecordV2FixedSize <= buffer.size()) {
        if (out.records.size() >= maxRecords) break;

        const char* rec = buffer.data() + offset;
        const std::uint32_t recordLength = ReadU32(rec);
        const std::uint16_t majorVersion = ReadU16(rec + 4);
        const std::uint16_t minorVersion = ReadU16(rec + 6);
        const std::uint64_t frn = ReadU64(rec + 8);
        const std::uint64_t parentFrn = ReadU64(rec + 16);
        const std::uint32_t reason = ReadU32(rec + 40);
        const std::uint32_t fileAttributes = ReadU32(rec + 52);
        const std::uint16_t fileNameLength = ReadU16(rec + 56);
        const std::uint16_t fileNameOffset = ReadU16(rec + 58);

        // The classic bounds failure: adding two 32-bit values wraps. Compute in
        // 64-bit so a RecordLength near UINT32_MAX can never look in-bounds.
        const std::uint64_t recordEnd = static_cast<std::uint64_t>(offset) + recordLength;
        if (recordLength == 0 || recordEnd > buffer.size()) {
            // Cannot trust this record or anything after it.
            break;
        }

        if (majorVersion != kUsnMajorVersionV2) {
            // V3 and later use a wider file reference; skipping them is the
            // documented behaviour, and the record length still advances.
            ++out.malformedRecords;
            offset = static_cast<std::size_t>(recordEnd);
            continue;
        }

        const std::uint64_t nameStart = static_cast<std::uint64_t>(offset) + fileNameOffset;
        const std::uint64_t nameEnd = nameStart + fileNameLength;
        if (fileNameOffset < kUsnRecordV2FixedSize || nameEnd > recordEnd ||
            (fileNameLength % 2) != 0) {
            ++out.malformedRecords;
            offset = static_cast<std::size_t>(recordEnd);
            continue;
        }

        UsnRecord r;
        r.majorVersion = majorVersion;
        r.minorVersion = minorVersion;
        r.fileReferenceNumber = frn;
        r.parentFileReferenceNumber = parentFrn;
        r.usn = static_cast<std::int64_t>(ReadU64(rec + 24));
        r.reason = reason;
        r.fileAttributes = fileAttributes;
        r.recordLength = recordLength;

        const std::string_view rawName(buffer.data() + nameStart, fileNameLength);
        auto utf8 = Utf16LeToUtf8(rawName);
        if (utf8.Ok()) {
            r.name = std::move(utf8.Value());
        } else {
            // A name we cannot decode is a skipped record, not a failed volume:
            // the caller still wants every other file in the buffer.
            ++out.malformedRecords;
            r.name.clear();
        }

        out.records.push_back(std::move(r));
        offset = static_cast<std::size_t>(recordEnd);
    }

    out.bytesConsumed = offset;
    return Result<UsnParseOutcome>::Success(std::move(out));
}

} // namespace auralite
