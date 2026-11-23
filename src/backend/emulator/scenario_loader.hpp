#pragma once

#include "backend/emulator/emulator_backend.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace auralite {

struct ScenarioSpec {
    std::uint32_t schemaVersion{1};
    std::string id;
    std::uint64_t seed{1};
    std::string description;

    // Volume
    ClusterCount clusters{16384};
    std::uint32_t bytesPerCluster{4096};
    double freeTargetPct{40.0};
    std::string fsName{"NTFS"};
    MediaClass mediaClass{MediaClass::Spinning};
    bool dirty{false};
    bool readOnly{false};
    bool bitLockerLocked{false};
    bool supportsDirectMftMove{false};

    // MFT
    std::uint32_t mftFragments{1};
    double zonePct{12.5};
    double occupiedByUserFilesPct{0.0};
    std::uint64_t mftRecordCount{4096};
    std::uint64_t mftInUseRecords{1024};

    // Generator
    std::string generator{"G-1"};

    // Files
    std::uint32_t fileCount{100};
    std::uint32_t extentsPerFile{4};
    std::uint32_t sizeMinClusters{4};
    std::uint32_t sizeMaxClusters{32};
    double residentPct{0.0};
    std::uint32_t compressedCount{0};
    std::uint32_t sparseCount{0};
    std::uint32_t encryptedCount{0};
    std::uint32_t reparseCount{0};
    std::uint32_t lockedCount{0};
    std::uint32_t adsCount{0};
    bool longPaths{false};
    bool unicodeNames{false};
    bool includeSystemFiles{false};

    // Faults
    std::vector<ScheduledFault> faults;

    // Expectations
    double fragmentationBeforeMin{0.0};
    double fragmentationAfterMax{100.0};
    bool expectPreflightFail{false};
    bool expectZeroMoves{false};
    MftTier expectedMftTier{MftTier::Tier3ReportOnly};
    double agreementMinPct{98.0};
};

Result<ScenarioSpec> ParseScenarioJson(std::string_view jsonText);
Result<ScenarioSpec> LoadScenarioFile(const std::filesystem::path& path);
Result<std::unique_ptr<EmulatorBackend>> BuildEmulatorFromScenario(const ScenarioSpec& spec);

} // namespace auralite
