#include "reporting/csv_reader.hpp"
#include "core/error_catalog.hpp"

#include <array>
#include <string_view>
#include <cstdlib>
#include <limits>

namespace auralite {

namespace {

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
constexpr std::uint32_t kSupportedSchemaVersion = 1;

std::string_view StripBom(std::string_view text) {
    if (text.size() >= kUtf8Bom.size() && text.substr(0, kUtf8Bom.size()) == kUtf8Bom) {
        text.remove_prefix(kUtf8Bom.size());
    }
    return text;
}

// Trim a trailing '\r' left by CRLF line endings inside an unquoted field.
void TrimTrailingCr(std::string& s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
}

bool IsFormulaPrefix(char c) noexcept {
    return c == '=' || c == '+' || c == '-' || c == '@' || c == '\t' || c == '\r';
}

Result<std::uint64_t> ToU64(const std::string& s) {
    if (s.empty()) return Result<std::uint64_t>::Success(0);
    std::size_t consumed = 0;
    unsigned long long v = 0;
    try {
        v = std::stoull(s, &consumed, 10);
    } catch (...) {
        return Result<std::uint64_t>::Failure(
            MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: non-numeric integer field"));
    }
    if (consumed != s.size()) {
        return Result<std::uint64_t>::Failure(
            MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: trailing garbage in integer field"));
    }
    return Result<std::uint64_t>::Success(static_cast<std::uint64_t>(v));
}

Result<double> ToDouble(const std::string& s) {
    if (s.empty()) return Result<double>::Success(0.0);
    std::size_t consumed = 0;
    double v = 0.0;
    try {
        v = std::stod(s, &consumed);
    } catch (...) {
        return Result<double>::Failure(
            MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: non-numeric float field"));
    }
    if (consumed != s.size()) {
        return Result<double>::Failure(
            MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: trailing garbage in float field"));
    }
    return Result<double>::Success(v);
}

FileState ParseState(const std::string& s) {
    if (s == "Fragmented") return FileState::Fragmented;
    if (s == "Unmovable") return FileState::Unmovable;
    if (s == "Excluded") return FileState::Excluded;
    if (s == "InProgress") return FileState::InProgress;
    if (s == "Skipped") return FileState::Skipped;
    if (s == "Defragmented") return FileState::Defragmented;
    return FileState::Ready;
}

SkipReason ParseSkipReason(const std::string& s) {
    if (s == "MetadataFile") return SkipReason::MetadataFile;
    if (s == "PageOrSystemFile") return SkipReason::PageOrSystemFile;
    if (s == "LockedOrInUse") return SkipReason::LockedOrInUse;
    if (s == "AccessDenied") return SkipReason::AccessDenied;
    if (s == "ExcludedByRule") return SkipReason::ExcludedByRule;
    if (s == "UnsupportedStream") return SkipReason::UnsupportedStream;
    if (s == "InsufficientContiguousSpace") return SkipReason::InsufficientContiguousSpace;
    if (s == "StaleBitmapConflict") return SkipReason::StaleBitmapConflict;
    if (s == "ReadOnlyOrPreFlightBlocked") return SkipReason::ReadOnlyOrPreFlightBlocked;
    if (s == "IoError") return SkipReason::IoError;
    if (s == "MftSafeguardAbort") return SkipReason::MftSafeguardAbort;
    if (s == "FileVanished") return SkipReason::FileVanished;
    return SkipReason::None;
}

// Convert a raw tokenizer cell into a field with neutralisation tracked.
CsvField MakeField(const std::string& raw, bool quoted) {
    CsvField f;
    f.wasQuoted = quoted;
    f.value = raw;
    if (quoted && !f.value.empty() && f.value.front() == '\'' && f.value.size() > 1 &&
        IsFormulaPrefix(f.value[1])) {
        f.wasNeutralized = true;
    }
    return f;
}

} // namespace

std::string UnescapeField(const CsvField& field) {
    return field.value;
}

Result<std::vector<std::vector<CsvField>>> ParseCsv(std::string_view text) {
    text = StripBom(text);

    std::vector<std::vector<CsvField>> rows;
    std::vector<CsvField> row;
    std::string cell;
    bool inQuotes = false;
    bool cellWasQuoted = false;
    bool rowHasData = false;

    const std::size_t n = text.size();
    for (std::size_t i = 0; i < n; ++i) {
        const char c = text[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < n && text[i + 1] == '"') {
                    cell.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                cell.push_back(c);
            }
            continue;
        }

        if (c == '"' && cell.empty()) {
            inQuotes = true;
            cellWasQuoted = true;
            rowHasData = true;
            continue;
        }
        if (c == ',') {
            row.push_back(MakeField(cell, cellWasQuoted));
            cell.clear();
            cellWasQuoted = false;
            rowHasData = true;
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < n && text[i + 1] == '\n') ++i;
            TrimTrailingCr(cell);
            row.push_back(MakeField(cell, cellWasQuoted));
            if (rowHasData || row.size() > 1 || !row.front().value.empty()) {
                rows.push_back(std::move(row));
            }
            row.clear();
            cell.clear();
            cellWasQuoted = false;
            rowHasData = false;
            continue;
        }
        cell.push_back(c);
        rowHasData = true;
    }

    if (inQuotes) {
        return Result<std::vector<std::vector<CsvField>>>::Failure(
            MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: unterminated quoted field"));
    }
    if (rowHasData || !cell.empty()) {
        TrimTrailingCr(cell);
        row.push_back(MakeField(cell, cellWasQuoted));
        rows.push_back(std::move(row));
    }
    return Result<std::vector<std::vector<CsvField>>>::Success(std::move(rows));
}

Result<std::vector<CsvFileRow>> CsvReader::ParseFileList(std::string_view csv) {
    auto parsed = ParseCsv(csv);
    if (!parsed.Ok()) return Result<std::vector<CsvFileRow>>::Failure(parsed.Error());

    auto& rows = parsed.Value();
    if (rows.empty()) {
        return Result<std::vector<CsvFileRow>>::Failure(
            MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: empty file list"));
    }
    if (rows[0].size() < 9) {
        return Result<std::vector<CsvFileRow>>::Failure(
            MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: file-list header too short"));
    }

    std::vector<CsvFileRow> out;
    out.reserve(rows.size() - 1);
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (row.size() != 9) {
            return Result<std::vector<CsvFileRow>>::Failure(
                MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: file-list row column mismatch"));
        }
        CsvFileRow fr;
        auto sv = ToU64(row[0].value);
        if (!sv.Ok()) return Result<std::vector<CsvFileRow>>::Failure(sv.Error());
        fr.schemaVersion = static_cast<std::uint32_t>(sv.Value());
        if (fr.schemaVersion != kSupportedSchemaVersion) {
            return Result<std::vector<CsvFileRow>>::Failure(
                MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: unsupported schema version"));
        }
        auto id = ToU64(row[1].value);
        if (!id.Ok()) return Result<std::vector<CsvFileRow>>::Failure(id.Error());
        fr.id = static_cast<FileId>(id.Value());
        fr.name = row[2].value;
        fr.path = row[3].value;
        auto size = ToU64(row[4].value);
        if (!size.Ok()) return Result<std::vector<CsvFileRow>>::Failure(size.Error());
        fr.sizeBytes = size.Value();
        auto alloc = ToU64(row[5].value);
        if (!alloc.Ok()) return Result<std::vector<CsvFileRow>>::Failure(alloc.Error());
        fr.allocatedClusters = static_cast<ClusterCount>(alloc.Value());
        auto frags = ToU64(row[6].value);
        if (!frags.Ok()) return Result<std::vector<CsvFileRow>>::Failure(frags.Error());
        fr.fragmentCount = static_cast<std::uint32_t>(frags.Value());
        fr.state = ParseState(row[7].value);
        fr.inMftZone = (row[8].value == "true");
        out.push_back(std::move(fr));
    }
    return Result<std::vector<CsvFileRow>>::Success(std::move(out));
}

Result<std::vector<CsvSkipRow>> CsvReader::ParseSkipReport(std::string_view csv) {
    auto parsed = ParseCsv(csv);
    if (!parsed.Ok()) return Result<std::vector<CsvSkipRow>>::Failure(parsed.Error());

    auto& rows = parsed.Value();
    if (rows.empty() || rows[0].size() < 4) {
        return Result<std::vector<CsvSkipRow>>::Failure(
            MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: bad skip-report header"));
    }

    std::vector<CsvSkipRow> out;
    out.reserve(rows.size() - 1);
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (row.size() != 4) {
            return Result<std::vector<CsvSkipRow>>::Failure(
                MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: skip row column mismatch"));
        }
        CsvSkipRow sr;
        auto sv = ToU64(row[0].value);
        if (!sv.Ok()) return Result<std::vector<CsvSkipRow>>::Failure(sv.Error());
        sr.schemaVersion = static_cast<std::uint32_t>(sv.Value());
        auto id = ToU64(row[1].value);
        if (!id.Ok()) return Result<std::vector<CsvSkipRow>>::Failure(id.Error());
        sr.fileId = static_cast<FileId>(id.Value());
        sr.fileName = row[2].value;
        sr.reason = ParseSkipReason(row[3].value);
        out.push_back(std::move(sr));
    }
    return Result<std::vector<CsvSkipRow>>::Success(std::move(out));
}

Result<CsvStatisticsRow> CsvReader::ParseStatistics(std::string_view csv) {
    auto parsed = ParseCsv(csv);
    if (!parsed.Ok()) return Result<CsvStatisticsRow>::Failure(parsed.Error());

    auto& rows = parsed.Value();
    if (rows.size() < 2) {
        return Result<CsvStatisticsRow>::Failure(
            MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: statistics needs header + row"));
    }
    const auto& row = rows[1];
    if (row.size() < 17) {
        return Result<CsvStatisticsRow>::Failure(
            MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "CSV: statistics row too short"));
    }

    CsvStatisticsRow s;
    auto sv = ToU64(row[0].value);
    if (!sv.Ok()) return Result<CsvStatisticsRow>::Failure(sv.Error());
    s.schemaVersion = static_cast<std::uint32_t>(sv.Value());
    s.volumeId = row[1].value;

    const std::array<std::size_t, 10> u64Cols{2, 3, 4, 5, 8, 9, 10, 11, 12, 13};
    std::uint64_t* u64Targets[10] = {&s.totalClusters,   &s.freeClusters,
                                     &s.totalFiles,      &s.fragmentedFiles,
                                     &s.mftFragments,    &s.mftZoneTotalClusters,
                                     &s.mftZoneFreeClusters, &s.mftZoneUserOccupiedClusters,
                                     &s.hist[0],         &s.hist[1]};
    for (std::size_t k = 0; k < u64Cols.size(); ++k) {
        auto v = ToU64(row[u64Cols[k]].value);
        if (!v.Ok()) return Result<CsvStatisticsRow>::Failure(v.Error());
        *u64Targets[k] = v.Value();
    }
    for (std::size_t k = 0; k < 3; ++k) {
        auto v = ToU64(row[14 + k].value);
        if (!v.Ok()) return Result<CsvStatisticsRow>::Failure(v.Error());
        s.hist[2 + k] = v.Value();
    }
    auto vf = ToDouble(row[6].value);
    if (!vf.Ok()) return Result<CsvStatisticsRow>::Failure(vf.Error());
    s.volumeFragmentationPct = vf.Value();
    auto ff = ToDouble(row[7].value);
    if (!ff.Ok()) return Result<CsvStatisticsRow>::Failure(ff.Error());
    s.fileFragmentationPct = ff.Value();
    return Result<CsvStatisticsRow>::Success(s);
}

bool CsvReader::RoundTrips(const FileEntry& original, const CsvFileRow& parsed) {
    if (original.id != parsed.id) return false;
    if (original.sizeBytes != parsed.sizeBytes) return false;
    if (original.allocatedClusters != parsed.allocatedClusters) return false;
    if (original.fragmentCount != parsed.fragmentCount) return false;
    if (original.state != parsed.state) return false;
    if (original.flags.inMftZone != parsed.inMftZone) return false;

    // The writer defangs formula prefixes by prepending a single quote. The
    // round-trip value is therefore either the original text (no defanging
    // needed) or the defanged form — never anything else.
    const auto matches = [](const std::string& orig, const std::string& got) {
        if (orig == got) return true;
        std::string defanged;
        defanged.reserve(orig.size() + 1);
        defanged.push_back('\'');
        defanged.append(orig);
        return defanged == got;
    };
    if (!matches(original.name, parsed.name)) return false;
    if (!matches(original.path, parsed.path)) return false;
    return true;
}

bool CsvReader::RoundTrips(const SkippedPlanItem& original, const CsvSkipRow& parsed) {
    if (original.fileId != parsed.fileId) return false;
    if (original.reason != parsed.reason) return false;
    if (original.fileName == parsed.fileName) return true;
    std::string defanged = "'" + original.fileName;
    return defanged == parsed.fileName;
}

} // namespace auralite
