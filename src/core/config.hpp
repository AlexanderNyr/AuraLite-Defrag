#pragma once

#include "core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace auralite {

enum class ThemeMode : std::uint8_t {
    Light = 0,
    Dark = 1,
};

// ExclusionKind lives in core/exclusions.hpp. Declared here as a plain enum to
// avoid a config.hpp -> exclusions.hpp include cycle; the two must stay in sync
// (exclusions.cpp static_asserts the numeric values).
enum class ExclusionKind : std::uint8_t;

struct ExclusionRule {
    std::string pattern; // path prefix, glob wildcard, or extension (e.g., "*.vhdx")
    std::uint64_t minSizeBytes{0};
    std::uint64_t maxSizeBytes{0}; // 0 = unbounded
    bool enabled{true};
    // Explicit kind; Path preserves the original semantics and is the default so
    // existing config files keep working unchanged.
    ExclusionKind kind = static_cast<ExclusionKind>(0);
};

struct AppConfig {
    std::uint32_t schemaVersion{1};
    ThemeMode theme{ThemeMode::Light};

    // A-14 free-space gates: <5% analysis-only, 5-15% quick+warning, >=15% full defrag
    double minFreeSpaceAnalysisOnlyPct{5.0}; // fixed safety floor
    double fullDefragThresholdPct{15.0};     // configurable bounded [5.0 .. 15.0]

    // Large-files-to-end threshold (P4-T08)
    std::uint64_t largeFileThresholdBytes{100ULL * 1024ULL * 1024ULL};

    // Quick defrag parameters
    std::uint32_t quickDefragMinFragments{2};
    std::uint64_t quickDefragMinSizeBytes{64ULL * 1024ULL};

    // Map grid dimensions (A-15: default 200x100, capped at 400x200)
    std::uint32_t mapCols{200};
    std::uint32_t mapRows{100};

    // A-17 logging/history retention
    std::uint32_t logRetentionDays{90};
    std::uint32_t maxHistoryOperations{2000};
    std::uint64_t logMaxFileBytes{10ULL * 1024ULL * 1024ULL};
    std::uint32_t logMaxRotations{5};
    bool verboseLogPaths{false};

    // Tray & notifications
    bool minimizeToTray{false};
    bool notifyOnComplete{true};

    // Verification sampling
    bool verifyChecksumsSample{true};

    std::vector<ExclusionRule> exclusions;
};

Result<AppConfig> ParseConfigJson(std::string_view jsonText);
std::string SerializeConfigJson(const AppConfig& cfg);
Result<AppConfig> LoadConfigFile(const std::filesystem::path& path);
Result<void> SaveConfigFileAtomic(const std::filesystem::path& path, const AppConfig& cfg);
bool IsPortableMode(const std::filesystem::path& exeDir);

} // namespace auralite
