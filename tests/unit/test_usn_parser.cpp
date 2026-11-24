// P9-T01 / P3-T05 — USN record parsing unit tests.
//
// The record walk previously lived inline in RealNtfsBackend::Enumerate()
// behind `#ifdef _WIN32`, so no Linux test could reach it. These tests are the
// reason it was extracted: every bounds check is now asserted against a real
// byte buffer instead of being trusted by inspection.
#include "backend/usn/usn_parser.hpp"
#include "core/json.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace auralite;

namespace {

int gRun = 0;
int gFailed = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "]: " #cond "\n";  \
            ++gFailed;                                                                \
            return;                                                                   \
        }                                                                             \
    } while (false)

#define RUN_TEST(fn)                                                        \
    do {                                                                    \
        ++gRun;                                                             \
        const int before = gFailed;                                         \
        fn();                                                               \
        std::cout << (gFailed == before ? "  [PASS] " : "  [FAIL] ") << #fn "\n"; \
    } while (false)

// Build a well-formed record with an ASCII name.
void AppendRecord(std::string& buf, std::uint64_t frn, const std::string& name,
                  std::uint32_t attrs = 0x80, std::uint16_t major = 2) {
    const std::uint16_t nameLen = static_cast<std::uint16_t>(name.size() * 2);
    const std::uint32_t recLen = static_cast<std::uint32_t>(kUsnRecordV2FixedSize + nameLen);
    const std::size_t base = buf.size();
    buf.append(recLen, '\0');
    char* p = buf.data() + base;
    std::memcpy(p + 0, &recLen, 4);
    const std::uint16_t minor = 0;
    std::memcpy(p + 4, &major, 2);
    std::memcpy(p + 6, &minor, 2);
    std::memcpy(p + 8, &frn, 8);
    std::memcpy(p + 16, &frn, 8);
    std::memcpy(p + 52, &attrs, 4);
    std::memcpy(p + 56, &nameLen, 2);
    const std::uint16_t nameOff = static_cast<std::uint16_t>(kUsnRecordV2FixedSize);
    std::memcpy(p + 58, &nameOff, 2);
    for (std::size_t i = 0; i < name.size(); ++i) {
        p[kUsnRecordV2FixedSize + i * 2] = name[i];
    }
}

std::string MakeBuffer(std::initializer_list<std::string> names) {
    std::string buf(kUsnHeaderSize, '\0');
    std::uint64_t frn = 100;
    for (const auto& n : names) AppendRecord(buf, frn++, n);
    return buf;
}

// ---------------------------------------------------------------------------

void TestParsesWellFormedBuffer() {
    const auto buf = MakeBuffer({"alpha.txt", "beta.dat", "gamma"});
    auto r = ParseUsnBuffer(buf, 100);
    CHECK(r.Ok());
    const auto& out = r.Value();
    CHECK(out.records.size() == 3);
    CHECK(out.records[0].name == "alpha.txt");
    CHECK(out.records[1].name == "beta.dat");
    CHECK(out.records[2].name == "gamma");
    CHECK(out.records[0].fileReferenceNumber == 100);
    CHECK(out.records[2].fileReferenceNumber == 102);
    CHECK(out.malformedRecords == 0);
    CHECK(out.bytesConsumed == buf.size());
}

void TestShortBufferIsNotAnError() {
    // The OS returns fewer than 8 bytes when enumeration is exhausted. That is
    // a normal termination, not a corrupt volume.
    for (std::size_t n : {0UL, 1UL, 4UL, 7UL, 8UL}) {
        std::string buf(n, '\0');
        auto r = ParseUsnBuffer(buf, 100);
        CHECK(r.Ok());
        CHECK(r.Value().records.empty());
    }
}

void TestHugeRecordLengthDoesNotOverrun() {
    // RecordLength near UINT32_MAX. offset + RecordLength must be computed in
    // 64-bit; in 32-bit it wraps and the bounds check passes on a 68-byte buffer.
    std::string buf(kUsnHeaderSize, '\0');
    buf.append(kUsnRecordV2FixedSize, '\0');
    const std::uint32_t recLen = 0xFFFFFFF0u;
    std::memcpy(buf.data() + kUsnHeaderSize, &recLen, 4);
    const std::uint16_t major = 2;
    std::memcpy(buf.data() + kUsnHeaderSize + 4, &major, 2);

    auto r = ParseUsnBuffer(buf, 100);
    CHECK(r.Ok());
    CHECK(r.Value().records.empty()); // stopped, did not read past the end
}

void TestZeroRecordLengthStopsParsing() {
    // A zero length would loop forever; it must terminate the walk.
    std::string buf(kUsnHeaderSize, '\0');
    buf.append(kUsnRecordV2FixedSize, '\0');
    auto r = ParseUsnBuffer(buf, 100);
    CHECK(r.Ok());
    CHECK(r.Value().records.empty());
}

void TestNameOffsetOutOfRecordIsSkipped() {
    std::string buf = MakeBuffer({"good.txt"});
    // Corrupt the first record's FileNameOffset to point past its own end.
    char* p = buf.data() + kUsnHeaderSize;
    const std::uint16_t badOffset = 0xFFF0;
    std::memcpy(p + 58, &badOffset, 2);

    auto r = ParseUsnBuffer(buf, 100);
    CHECK(r.Ok());
    CHECK(r.Value().records.empty());
    CHECK(r.Value().malformedRecords == 1);

    // A second, well-formed record after the corrupt one must still be parsed.
    AppendRecord(buf, 999, "after.txt");
    // Repair is not needed: the corrupt record's RecordLength is intact, so the
    // walk can advance past it.
    auto r2 = ParseUsnBuffer(buf, 100);
    CHECK(r2.Ok());
    CHECK(r2.Value().records.size() == 1);
    CHECK(r2.Value().records[0].name == "after.txt");
}

void TestNameLengthRunningPastRecordIsSkipped() {
    std::string buf = MakeBuffer({"x"});
    char* p = buf.data() + kUsnHeaderSize;
    const std::uint16_t hugeLen = 0x8000; // way past the record
    std::memcpy(p + 56, &hugeLen, 2);

    auto r = ParseUsnBuffer(buf, 100);
    CHECK(r.Ok());
    CHECK(r.Value().records.empty());
    CHECK(r.Value().malformedRecords == 1);
}

void TestOddNameLengthIsRejected() {
    std::string buf = MakeBuffer({"xy"});
    char* p = buf.data() + kUsnHeaderSize;
    const std::uint16_t oddLen = 3; // not a whole UTF-16 unit
    std::memcpy(p + 56, &oddLen, 2);

    auto r = ParseUsnBuffer(buf, 100);
    CHECK(r.Ok());
    CHECK(r.Value().records.empty());
}

void TestUnknownMajorVersionIsSkippedButWalkContinues() {
    std::string buf(kUsnHeaderSize, '\0');
    AppendRecord(buf, 1, "v3record.txt", 0x80, 3); // V3-shaped but V2-sized
    AppendRecord(buf, 2, "v2record.txt", 0x80, 2);

    auto r = ParseUsnBuffer(buf, 100);
    CHECK(r.Ok());
    CHECK(r.Value().records.size() == 1);
    CHECK(r.Value().records[0].name == "v2record.txt");
    CHECK(r.Value().malformedRecords == 1);
}

void TestMaxRecordsBoundsOutput() {
    std::string buf(kUsnHeaderSize, '\0');
    for (int i = 0; i < 1000; ++i) AppendRecord(buf, static_cast<std::uint64_t>(i), "n");
    auto r = ParseUsnBuffer(buf, 10);
    CHECK(r.Ok());
    CHECK(r.Value().records.size() == 10);
}

void TestAttributesAreDecoded() {
    std::string buf(kUsnHeaderSize, '\0');
    const std::uint32_t attrs = kFileAttributeDirectory | kFileAttributeCompressed;
    AppendRecord(buf, 7, "dir", attrs);
    auto r = ParseUsnBuffer(buf, 10);
    CHECK(r.Ok());
    CHECK(r.Value().records.size() == 1);
    CHECK((r.Value().records[0].fileAttributes & kFileAttributeDirectory) != 0);
    CHECK((r.Value().records[0].fileAttributes & kFileAttributeCompressed) != 0);
    CHECK((r.Value().records[0].fileAttributes & kFileAttributeSparseFile) == 0);
}

void TestUnicodeNamesSurvive() {
    std::string buf(kUsnHeaderSize, '\0');
    // "日本.txt" in UTF-16LE
    const wchar_t* wide = L"\u65E5\u672C.txt";
    std::string name;
    for (const wchar_t* w = wide; *w; ++w) {
        const std::uint16_t unit = static_cast<std::uint16_t>(*w);
        name.push_back(static_cast<char>(unit & 0xFF));
        name.push_back(static_cast<char>(unit >> 8));
    }
    const std::uint32_t recLen = static_cast<std::uint32_t>(kUsnRecordV2FixedSize + name.size());
    const std::size_t base = buf.size();
    buf.append(recLen, '\0');
    char* p = buf.data() + base;
    std::memcpy(p + 0, &recLen, 4);
    const std::uint16_t major = 2;
    std::memcpy(p + 4, &major, 2);
    const std::uint16_t nameLen = static_cast<std::uint16_t>(name.size());
    std::memcpy(p + 56, &nameLen, 2);
    const std::uint16_t nameOff = static_cast<std::uint16_t>(kUsnRecordV2FixedSize);
    std::memcpy(p + 58, &nameOff, 2);
    std::memcpy(p + kUsnRecordV2FixedSize, name.data(), name.size());

    auto r = ParseUsnBuffer(buf, 10);
    CHECK(r.Ok());
    CHECK(r.Value().records.size() == 1);
    CHECK(r.Value().records[0].name == "\xE6\x97\xA5\xE6\x9C\xAC.txt");
    CHECK(IsValidUtf8(r.Value().records[0].name));
}

// --- UTF-16 decoder -------------------------------------------------------

void TestUtf16Decoder() {
    auto ascii = Utf16LeToUtf8(std::string("A\x00" "B\x00", 4));
    CHECK(ascii.Ok() && ascii.Value() == "AB");

    // Surrogate pair for U+1F3AF (🎯)
    auto emoji = Utf16LeToUtf8(std::string("\x3C\xD8\xAF\xDF", 4));
    CHECK(emoji.Ok());
    CHECK(emoji.Value() == "\xF0\x9F\x8E\xAF");

    // Lone high surrogate
    CHECK(!Utf16LeToUtf8(std::string("\x00\xD8", 2)).Ok());
    // Lone low surrogate
    CHECK(!Utf16LeToUtf8(std::string("\x00\xDC", 2)).Ok());
    // High surrogate at end of buffer
    CHECK(!Utf16LeToUtf8(std::string("\x41\x00\x00\xD8", 4)).Ok());
    // Odd byte count
    CHECK(!Utf16LeToUtf8(std::string("\x41\x00\x42", 3)).Ok());
    // Embedded NUL is not a legal file-name character
    CHECK(!Utf16LeToUtf8(std::string("\x00\x00", 2)).Ok());
    // Control characters are rejected
    CHECK(!Utf16LeToUtf8(std::string("\x01\x00", 2)).Ok());
    // Empty is fine
    CHECK(Utf16LeToUtf8("").Ok());
}

void TestDecoderOutputIsAlwaysValidUtf8() {
    // Exhaustive over all single UTF-16 units: whatever decodes must be UTF-8.
    for (std::uint32_t unit = 0; unit <= 0xFFFF; ++unit) {
        std::string in(2, '\0');
        in[0] = static_cast<char>(unit & 0xFF);
        in[1] = static_cast<char>((unit >> 8) & 0xFF);
        auto r = Utf16LeToUtf8(in);
        if (r.Ok()) {
            if (!IsValidUtf8(r.Value())) {
                std::cerr << "  invalid UTF-8 produced for unit 0x" << std::hex << unit << "\n";
                ++gFailed;
                return;
            }
        }
    }
    CHECK(gFailed == 0);
}

void TestTruncatedBufferStopsCleanly() {
    // Cut the buffer in the middle of a record: the record must be abandoned,
    // not read past the end.
    auto buf = MakeBuffer({"full.txt", "second.txt"});
    for (std::size_t cut = kUsnHeaderSize; cut < buf.size(); ++cut) {
        const std::string truncated = buf.substr(0, cut);
        auto r = ParseUsnBuffer(truncated, 100);
        CHECK(r.Ok());
        CHECK(r.Value().bytesConsumed <= truncated.size());
    }
}

} // namespace

int main() {
    std::cout << "Running USN parser suite (P9-T01 / P3-T05)...\n";
    RUN_TEST(TestParsesWellFormedBuffer);
    RUN_TEST(TestShortBufferIsNotAnError);
    RUN_TEST(TestHugeRecordLengthDoesNotOverrun);
    RUN_TEST(TestZeroRecordLengthStopsParsing);
    RUN_TEST(TestNameOffsetOutOfRecordIsSkipped);
    RUN_TEST(TestNameLengthRunningPastRecordIsSkipped);
    RUN_TEST(TestOddNameLengthIsRejected);
    RUN_TEST(TestUnknownMajorVersionIsSkippedButWalkContinues);
    RUN_TEST(TestMaxRecordsBoundsOutput);
    RUN_TEST(TestAttributesAreDecoded);
    RUN_TEST(TestUnicodeNamesSurvive);
    RUN_TEST(TestUtf16Decoder);
    RUN_TEST(TestDecoderOutputIsAlwaysValidUtf8);
    RUN_TEST(TestTruncatedBufferStopsCleanly);
    std::cout << "\nSummary: " << (gRun - gFailed) << "/" << gRun << " passed.\n";
    return gFailed == 0 ? 0 : 1;
}
