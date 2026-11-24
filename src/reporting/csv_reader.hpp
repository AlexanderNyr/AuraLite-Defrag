#pragma once
//
// P7-T08 — CSV round-trip reader and injection-safe schema.
//
// The exporter (csv_exporter.cpp) is the writer half. This is the reader half,
// required by acceptance criterion (1): "250k rows round-trip with 0 lost or
// duplicated records". Without a parser the criterion cannot be checked at all,
// and a quoting bug in the writer stays invisible until a customer's spreadsheet
// silently mangles a path.
//
// Scope: only the three exports this project produces (file list, statistics,
// skip report). This is deliberately NOT a general-purpose CSV parser; it is a
// verifier for our own writer and a fixture harness for SEC-02.
//
#include "core/result.hpp"
#include "core/types.hpp"
#include "interfaces/snapshots.hpp"
#include "reporting/statistics.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace auralite {

// One record from the file-list export.
struct CsvFileRow {
    std::uint32_t schemaVersion{0};
    FileId id{0};
    std::string name;
    std::string path;
    std::uint64_t sizeBytes{0};
    ClusterCount allocatedClusters{0};
    std::uint32_t fragmentCount{0};
    FileState state{FileState::Ready};
    bool inMftZone{false};
};

struct CsvStatisticsRow {
    std::uint32_t schemaVersion{0};
    std::string volumeId;
    std::uint64_t totalClusters{0};
    std::uint64_t freeClusters{0};
    std::uint64_t totalFiles{0};
    std::uint64_t fragmentedFiles{0};
    double volumeFragmentationPct{0.0};
    double fileFragmentationPct{0.0};
    std::uint64_t mftFragments{0};
    std::uint64_t mftZoneTotalClusters{0};
    std::uint64_t mftZoneFreeClusters{0};
    std::uint64_t mftZoneUserOccupiedClusters{0};
    std::array<std::uint64_t, 5> hist{};
};

struct CsvSkipRow {
    std::uint32_t schemaVersion{0};
    FileId fileId{0};
    std::string fileName;
    SkipReason reason{SkipReason::None};
};

// A single RFC 4180 field plus what the writer did to it.
struct CsvField {
    std::string value;        // value after unquoting and de-escaping
    bool wasQuoted{false};
    bool wasNeutralized{false}; // writer prefixed a single quote to defang a formula
};

// Low-level RFC 4180 tokenizer. Handles CRLF/LF, quoted fields, embedded
// commas/quotes/newlines, and reports malformed input instead of guessing.
Result<std::vector<std::vector<CsvField>>> ParseCsv(std::string_view text);

class CsvReader {
public:
    // Strips the UTF-8 BOM if present and validates the schema-version header.
    static Result<std::vector<CsvFileRow>> ParseFileList(std::string_view csv);
    static Result<std::vector<CsvSkipRow>> ParseSkipReport(std::string_view csv);
    static Result<CsvStatisticsRow> ParseStatistics(std::string_view csv);

    // Round-trip equality helpers. These are what the acceptance test asserts:
    // every field survives writer -> reader unchanged, including the ones the
    // writer had to neutralise (compared against the defanged form).
    static bool RoundTrips(const FileEntry& original, const CsvFileRow& parsed);
    static bool RoundTrips(const SkippedPlanItem& original, const CsvSkipRow& parsed);
};

// Escape/unescape pair exposed for fixture tests: the property that matters is
// UnescapeField(EscapeField(x)) == expected round-trip value for all x.
std::string UnescapeField(const CsvField& field);

} // namespace auralite
