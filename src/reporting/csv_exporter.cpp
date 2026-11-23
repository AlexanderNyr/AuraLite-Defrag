#include "reporting/csv_exporter.hpp"
#include "core/error_catalog.hpp"

#include <fstream>
#include <sstream>

namespace auralite {

namespace {

const char* FileStateName(FileState st) noexcept {
    switch (st) {
        case FileState::Ready: return "Ready";
        case FileState::Fragmented: return "Fragmented";
        case FileState::Unmovable: return "Unmovable";
        case FileState::Excluded: return "Excluded";
        case FileState::InProgress: return "InProgress";
        case FileState::Skipped: return "Skipped";
        case FileState::Defragmented: return "Defragmented";
    }
    return "Ready";
}

const char* SkipReasonName(SkipReason r) noexcept {
    switch (r) {
        case SkipReason::None: return "None";
        case SkipReason::MetadataFile: return "MetadataFile";
        case SkipReason::PageOrSystemFile: return "PageOrSystemFile";
        case SkipReason::LockedOrInUse: return "LockedOrInUse";
        case SkipReason::AccessDenied: return "AccessDenied";
        case SkipReason::ExcludedByRule: return "ExcludedByRule";
        case SkipReason::UnsupportedStream: return "UnsupportedStream";
        case SkipReason::InsufficientContiguousSpace: return "InsufficientContiguousSpace";
        case SkipReason::StaleBitmapConflict: return "StaleBitmapConflict";
        case SkipReason::ReadOnlyOrPreFlightBlocked: return "ReadOnlyOrPreFlightBlocked";
        case SkipReason::IoError: return "IoError";
        case SkipReason::MftSafeguardAbort: return "MftSafeguardAbort";
        case SkipReason::FileVanished: return "FileVanished";
    }
    return "None";
}

bool NeedsFormulaNeutralization(std::string_view v) noexcept {
    if (v.empty()) return false;
    const char c = v.front();
    return c == '=' || c == '+' || c == '-' || c == '@' || c == '\t' || c == '\r';
}

} // namespace

std::string CsvExporter::EscapeField(std::string_view value) {
    std::string neutralized;
    if (NeedsFormulaNeutralization(value)) {
        neutralized.reserve(value.size() + 1);
        neutralized.push_back('\'');
        neutralized.append(value);
        value = neutralized;
    }

    bool mustQuote = false;
    for (char c : value) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            mustQuote = true;
            break;
        }
    }
    if (!mustQuote && neutralized.empty()) {
        return std::string(value);
    }

    std::string out;
    out.reserve(value.size() + 4);
    out.push_back('"');
    for (char c : value) {
        if (c == '"') {
            out.push_back('"');
            out.push_back('"');
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::string CsvExporter::BuildFileListCsv(const AnalysisSnapshot& snapshot) {
    std::ostringstream os;
    // UTF-8 BOM + schema version header (P4-T09 / P7-T08)
    os << "\xEF\xBB\xBF"
       << "SchemaVersion,FileId,Name,Path,SizeBytes,AllocatedClusters,Fragments,State,InMftZone\n";
    for (const auto& f : snapshot.files) {
        os << "1,"
           << f.id << ','
           << EscapeField(f.name) << ','
           << EscapeField(f.path) << ','
           << f.sizeBytes << ','
           << f.allocatedClusters << ','
           << f.fragmentCount << ','
           << FileStateName(f.state) << ','
           << (f.flags.inMftZone ? "true" : "false") << '\n';
    }
    return os.str();
}

std::string CsvExporter::BuildStatisticsCsv(const AnalysisSnapshot& snapshot,
                                            const BeforeAfterReport* beforeAfter) {
    const auto hist = StatisticsEngine::ComputeHistogram(snapshot);
    std::ostringstream os;
    os << "\xEF\xBB\xBF"
       << "SchemaVersion,VolumeId,TotalClusters,FreeClusters,TotalFiles,FragmentedFiles,"
       << "VolumeFragmentationPct,FileFragmentationPct,MftFragments,MftZoneTotalClusters,"
       << "MftZoneFreeClusters,MftZoneUserOccupiedClusters,Hist1,Hist2_4,Hist5_16,Hist17_64,Hist65Plus";
    if (beforeAfter) {
        os << ",BeforeVolumeFragPct,AfterVolumeFragPct,LargestFreeBefore,LargestFreeAfter";
    }
    os << '\n';

    os << "1,"
       << EscapeField(snapshot.volumeId) << ','
       << snapshot.summary.totalClusters << ','
       << snapshot.summary.freeClusters << ','
       << snapshot.summary.totalFiles << ','
       << snapshot.summary.fragmentedFiles << ','
       << snapshot.summary.volumeFragmentationPct << ','
       << snapshot.summary.fileFragmentationPct << ','
       << snapshot.mftInfo.fragmentCount << ','
       << snapshot.mftZone.totalZoneClusters << ','
       << snapshot.mftZone.freeZoneClusters << ','
       << snapshot.mftZone.occupiedByUserClusters << ','
       << hist.bins[0] << ','
       << hist.bins[1] << ','
       << hist.bins[2] << ','
       << hist.bins[3] << ','
       << hist.bins[4];
    if (beforeAfter) {
        os << ',' << beforeAfter->volumeFragBeforePct
           << ',' << beforeAfter->volumeFragAfterPct
           << ',' << beforeAfter->largestFreeRunBefore
           << ',' << beforeAfter->largestFreeRunAfter;
    }
    os << '\n';
    return os.str();
}

std::string CsvExporter::BuildSkipReportCsv(const std::vector<SkippedPlanItem>& skipped) {
    std::ostringstream os;
    os << "\xEF\xBB\xBF"
       << "SchemaVersion,FileId,Name,SkipReason\n";
    for (const auto& s : skipped) {
        os << "1,"
           << s.fileId << ','
           << EscapeField(s.fileName) << ','
           << SkipReasonName(s.reason) << '\n';
    }
    return os.str();
}

Result<void> CsvExporter::WriteFileAtomic(const std::filesystem::path& targetPath,
                                          std::string_view csvContent) {
    if (targetPath.empty() || targetPath.string().find("..") != std::string::npos) {
        return Result<void>::Failure(MakeError(ErrorCode::E_EXPORT_INVALID_PATH));
    }
    std::error_code ec;
    if (targetPath.has_parent_path()) {
        std::filesystem::create_directories(targetPath.parent_path(), ec);
    }
    auto tmp = targetPath;
    tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return Result<void>::Failure(MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "Cannot open temp CSV path"));
        }
        ofs.write(csvContent.data(), static_cast<std::streamsize>(csvContent.size()));
        ofs.flush();
        if (!ofs) {
            std::filesystem::remove(tmp, ec);
            return Result<void>::Failure(MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "Failed writing CSV data"));
        }
    }
    std::filesystem::rename(tmp, targetPath, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return Result<void>::Failure(MakeError(ErrorCode::E_EXPORT_IO, {}, 0, 0, "Atomic CSV rename failed"));
    }
    return Result<void>::Success();
}

} // namespace auralite
