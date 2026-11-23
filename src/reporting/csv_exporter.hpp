#pragma once

#include "core/result.hpp"
#include "interfaces/snapshots.hpp"
#include "reporting/statistics.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace auralite {

class CsvExporter {
public:
    static std::string EscapeField(std::string_view value);

    static std::string BuildFileListCsv(const AnalysisSnapshot& snapshot);
    static std::string BuildStatisticsCsv(const AnalysisSnapshot& snapshot,
                                          const BeforeAfterReport* beforeAfter = nullptr);
    static std::string BuildSkipReportCsv(const std::vector<SkippedPlanItem>& skipped);

    static Result<void> WriteFileAtomic(const std::filesystem::path& targetPath,
                                        std::string_view csvContent);
};

} // namespace auralite
