#include "backend/emulator/emulator_backend.hpp"
#include "backend/emulator/scenario_loader.hpp"
#include "cli/cli_app.hpp"
#include "core/config.hpp"
#include "core/error_catalog.hpp"
#include "core/json.hpp"
#include "core/logger.hpp"
#include "core/strings.hpp"
#include "engine/journal.hpp"
#include "engine/operation_controller.hpp"
#include "engine/planner.hpp"
#include "engine/policy_gate.hpp"
#include "reporting/cluster_aggregator.hpp"
#include "reporting/csv_exporter.hpp"
#include "reporting/statistics.hpp"
#include "scheduler/task_scheduler.hpp"
#include "ui/theme.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace auralite;

namespace {

int gTestsRun = 0;
int gTestsFailed = 0;

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "]: " #cond "\n";      \
            ++gTestsFailed;                                                                 \
            return;                                                                         \
        }                                                                                   \
    } while (false)

#define RUN_TEST(fn)                                    \
    do {                                                \
        ++gTestsRun;                                    \
        int beforeFail = gTestsFailed;                  \
        fn();                                           \
        if (gTestsFailed == beforeFail) {               \
            std::cout << "  [PASS] " #fn "\n";          \
        } else {                                        \
            std::cout << "  [FAIL] " #fn "\n";          \
        }                                               \
    } while (false)

class NullProgressSink final : public IProgressSink {
public:
    void OnSnapshot(const ProgressSnapshot& s) override { snapshots.push_back(s); }
    void OnEvent(const LogEvent& e) override { events.push_back(e); }
    std::vector<ProgressSnapshot> snapshots;
    std::vector<LogEvent> events;
};

std::filesystem::path TempTestDir() {
    auto p = std::filesystem::temp_directory_path() / "auralite_test_suite";
    std::filesystem::create_directories(p);
    return p;
}

void TestErrorTaxonomyAndMapping() {
    const ErrorCode codes[] = {
        ErrorCode::Ok, ErrorCode::E_PREFLIGHT_DIRTY, ErrorCode::E_PREFLIGHT_READ_ONLY,
        ErrorCode::E_PREFLIGHT_LOCKED_BITLOCKER, ErrorCode::E_PREFLIGHT_UNSUPPORTED_FS,
        ErrorCode::E_PREFLIGHT_SSD_BLOCKED, ErrorCode::E_PREFLIGHT_INSUFFICIENT_SPACE,
        ErrorCode::E_PREFLIGHT_NOT_ELEVATED, ErrorCode::E_PREFLIGHT_VOLUME_INACCESSIBLE,
        ErrorCode::E_ENUM_IO, ErrorCode::E_ENUM_ACCESS_DENIED, ErrorCode::E_ENUM_MALFORMED_RECORD,
        ErrorCode::E_ENUM_PATH_ESCAPE, ErrorCode::E_BITMAP_IO, ErrorCode::E_BITMAP_INVALID_RANGE,
        ErrorCode::E_BITMAP_STALE, ErrorCode::E_MOVE_STALE_BITMAP, ErrorCode::E_MOVE_ACCESS_DENIED,
        ErrorCode::E_MOVE_INVALID_PARAM, ErrorCode::E_MOVE_UNSUPPORTED_STREAM,
        ErrorCode::E_MOVE_INTERRUPTED, ErrorCode::E_MOVE_IO, ErrorCode::E_MOVE_LOCKED,
        ErrorCode::E_MOVE_FILE_VANISHED, ErrorCode::E_MOVE_PARTIAL, ErrorCode::E_MOVE_MFT_UNAVAILABLE,
        ErrorCode::E_VERIFY_EXTENT_MISMATCH, ErrorCode::E_VERIFY_CHECKSUM_MISMATCH,
        ErrorCode::E_VERIFY_READBACK_FAILED, ErrorCode::E_JOURNAL_CORRUPT_HEADER,
        ErrorCode::E_JOURNAL_CRC_MISMATCH, ErrorCode::E_JOURNAL_IO,
        ErrorCode::E_JOURNAL_PLAN_HASH_MISMATCH, ErrorCode::E_JOURNAL_SERIAL_MISMATCH,
        ErrorCode::E_JOURNAL_RECOVERED, ErrorCode::E_CONFIG_PARSE, ErrorCode::E_CONFIG_SCHEMA,
        ErrorCode::E_CONFIG_SIZE_LIMIT, ErrorCode::E_CONFIG_DEPTH_LIMIT,
        ErrorCode::E_CONFIG_PATH_TRAVERSAL, ErrorCode::E_CONFIG_IO,
        ErrorCode::E_EXPORT_IO, ErrorCode::E_EXPORT_WIC, ErrorCode::E_EXPORT_INVALID_PATH,
        ErrorCode::E_INTERNAL_INVALID_STATE, ErrorCode::E_INTERNAL_HANDLE,
        ErrorCode::E_INTERNAL_CANCELLED, ErrorCode::E_INTERNAL_VOLUME_REMOVED,
        ErrorCode::E_INTERNAL_POLICY_VIOLATION,
    };

    for (ErrorCode c : codes) {
        const auto& d = DescribeError(c);
        CHECK(d.code == c);
        CHECK(d.userMessageKey != nullptr);
        CHECK(!StringTable::Lookup(d.userMessageKey).empty());
    }
    CHECK(MapWin32ToMoveError(5) == ErrorCode::E_MOVE_ACCESS_DENIED);
    CHECK(MapWin32ToMoveError(32) == ErrorCode::E_MOVE_LOCKED);
    CHECK(MapWin32ToMoveError(183) == ErrorCode::E_MOVE_STALE_BITMAP);
    CHECK(MapWin32ToMoveError(2) == ErrorCode::E_MOVE_FILE_VANISHED);
    CHECK(MapErrorToCliExitCode(ErrorCode::E_PREFLIGHT_SSD_BLOCKED) == 12);
    CHECK(MapErrorToCliExitCode(ErrorCode::E_VERIFY_EXTENT_MISMATCH) == 20);
}

void TestStructuredLoggerAndRotation() {
    auto dir = TempTestDir() / "logs";
    std::filesystem::remove_all(dir);
    auto logFile = dir / "auralite.jsonl";
    StructuredLogger logger(logFile, 512, 3);

    for (int i = 0; i < 20; ++i) {
        LogEvent ev{};
        ev.level = LogLevel::Info;
        ev.opId = "op-1\r\ninjected";
        ev.volumeId = "D:";
        ev.errorCode = ErrorCode::Ok;
        ev.stage = ErrorStage::Move;
        ev.durationMs = 12;
        ev.clusters = 64;
        ev.messageKey = "IDS_OP_COMPLETE";
        ev.arguments = {HashPathForPrivacy("C:\\secret\\user\\file.txt")};
        logger.Log(ev);
    }

    CHECK(std::filesystem::exists(logFile));
    CHECK(std::filesystem::exists(dir / "auralite.jsonl.1"));
    std::ifstream ifs(logFile);
    std::string line;
    std::getline(ifs, line);
    CHECK(line.find("ts_utc") != std::string::npos);
    CHECK(line.find("opId") != std::string::npos);
    CHECK(line.find("C:\\secret") == std::string::npos);
    CHECK(line.find("pid:") != std::string::npos);
    CHECK(line.find("op-1  injected") != std::string::npos);
}

void TestConfigValidationAndPersistence() {
    AppConfig cfg{};
    cfg.theme = ThemeMode::Dark;
    cfg.fullDefragThresholdPct = 12.0;
    cfg.exclusions.push_back({"*.vhdx", 0, 0, true});

    const std::string serialized = SerializeConfigJson(cfg);
    auto parsed = ParseConfigJson(serialized);
    CHECK(parsed.Ok());
    CHECK(parsed.Value().theme == ThemeMode::Dark);
    CHECK(parsed.Value().fullDefragThresholdPct == 12.0);
    CHECK(parsed.Value().exclusions.size() == 1);

    // Reject out-of-range A-14 threshold (<5 or >15)
    CHECK(!ParseConfigJson(R"({"schemaVersion":1,"fullDefragThresholdPct":3.0})").Ok());
    CHECK(!ParseConfigJson(R"({"schemaVersion":1,"fullDefragThresholdPct":25.0})").Ok());
    // Reject unknown key
    CHECK(!ParseConfigJson(R"({"schemaVersion":1,"forceSsdOverride":true})").Ok());
    // Reject path traversal
    CHECK(!ParseConfigJson(R"({"schemaVersion":1,"exclusions":[{"pattern":"..\\..\\etc"}]})").Ok());

    auto cfgPath = TempTestDir() / "config.json";
    CHECK(SaveConfigFileAtomic(cfgPath, cfg).Ok());
    auto loaded = LoadConfigFile(cfgPath);
    CHECK(loaded.Ok());
    CHECK(loaded.Value().theme == ThemeMode::Dark);

    // Portable detection
    auto portDir = TempTestDir() / "portable_test";
    std::filesystem::remove_all(portDir);
    std::filesystem::create_directories(portDir);
    CHECK(!IsPortableMode(portDir));
    std::ofstream(portDir / "auralite.portable") << "1";
    CHECK(IsPortableMode(portDir));
}

void TestStringsFormattingAndThemeContrast() {
    CHECK(FormatInteger(1234567) == "1,234,567");
    CHECK(FormatInteger(1234567, '.') == "1.234.567");
    CHECK(FormatByteSize(1536) == "1.50 KiB");
    CHECK(FormatPercent(12.345, ',') == "12,35%");
    CHECK(StringTable::Format("IDS_MFT_TIER1_OK", {"5"}) ==
          "The master file table was consolidated online. 5 extents were combined into one.");

    for (ThemeMode m : {ThemeMode::Light, ThemeMode::Dark}) {
        const auto pal = GetThemePalette(m);
        CHECK(ComputeContrastRatio(pal.textPrimary, pal.windowBg) >= 4.5);
        CHECK(ComputeContrastRatio(pal.textPrimary, pal.panelBg) >= 4.5);
        CHECK(ComputeContrastRatio(pal.textSecondary, pal.panelBg) >= 4.5);
        CHECK(ComputeContrastRatio(pal.warningText, pal.warningBg) >= 4.5);
    }
}

void TestJsonFuzzingAndLimits() {
    // Depth > 8 rejected
    CHECK(ParseJsonStrict("[[[[[[[[[1]]]]]]]]]").Error().code == ErrorCode::E_CONFIG_DEPTH_LIMIT);
    // Size > 1 MiB rejected
    std::string huge(1024U * 1024U + 10U, ' ');
    CHECK(ParseJsonStrict(huge).Error().code == ErrorCode::E_CONFIG_SIZE_LIMIT);
    // Duplicate key rejected
    CHECK(!ParseJsonStrict(R"({"a":1,"a":2})").Ok());

    // 10,000 deterministic fuzz inputs under ASan/UBSan
    DeterministicRng rng(99991ULL);
    const char alphabet[] = "{}[]:,\"tfnul0123456789-.eE\\u0041 \n\t\xFF";
    for (int i = 0; i < 10000; ++i) {
        const std::size_t len = static_cast<std::size_t>(rng.Range(0, 64));
        std::string fuzz;
        fuzz.reserve(len);
        for (std::size_t j = 0; j < len; ++j) {
            fuzz.push_back(alphabet[rng.Range(0, sizeof(alphabet) - 2)]);
        }
        auto res = ParseJsonStrict(fuzz);
        (void)res;
    }
}

void TestClusterAggregatorInvariantsAndPerf() {
    // Priority matrix test: all 9 categories
    std::array<ClusterCount, kClusterCategoryCount> counts{};
    for (int cat = 0; cat < static_cast<int>(kClusterCategoryCount); ++cat) {
        counts.fill(0);
        counts[static_cast<std::size_t>(cat)] = 1;
        CHECK(ClusterAggregator::ResolveDominantCategory(counts) == static_cast<ClusterCategory>(cat));
        for (int lower = 0; lower < cat; ++lower) {
            counts[static_cast<std::size_t>(lower)] = 100;
            CHECK(ClusterAggregator::ResolveDominantCategory(counts) == static_cast<ClusterCategory>(cat));
        }
    }

    // 4M-cluster aggregation performance & invariant check (budget <= 50 ms on RH-1; check sanity here)
    AnalysisSnapshot snap{};
    snap.volumeId = "SIM-PERF";
    snap.geometry.totalClusters = 4000000ULL;
    snap.geometry.bytesPerCluster = 4096;
    snap.mftZone.zoneStartLcn = 100;
    snap.mftZone.zoneEndLcn = 50000;
    for (int i = 0; i < 5000; ++i) {
        FileEntry fe{};
        fe.id = static_cast<FileId>(10 + i);
        fe.name = "f_" + std::to_string(i);
        fe.extents = {{0, static_cast<Lcn>(60000ULL + static_cast<Lcn>(i) * 100ULL), 40}};
        fe.allocatedClusters = 40;
        fe.fragmentCount = (i % 3 == 0) ? 3 : 1;
        snap.files.push_back(std::move(fe));
    }

    auto t0 = std::chrono::steady_clock::now();
    ClusterGrid grid = ClusterAggregator::BuildGrid(snap, 200, 100, 10, 11);
    auto t1 = std::chrono::steady_clock::now();
    const auto msBuild = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "    4M-cluster grid aggregation: " << msBuild << " ms\n";
    CHECK(grid.cells.size() == 20000);

    // Every cell category count sum == cell.totalClusters
    ClusterCount totalSeen = 0;
    for (const auto& cell : grid.cells) {
        ClusterCount sum = 0;
        for (auto c : cell.counts) sum += c;
        CHECK(sum == cell.totalClusters);
        totalSeen += cell.totalClusters;
    }
    CHECK(totalSeen == snap.geometry.totalClusters);

    // Incremental update of 1,000 extents <= 2 ms target
    std::vector<IncrementalExtentUpdate> updates;
    updates.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        updates.push_back({static_cast<Lcn>(60000ULL + i * 100ULL),
                           static_cast<Lcn>(2000000ULL + i * 50ULL),
                           10, ClusterCategory::UsedContiguous});
    }
    auto u0 = std::chrono::steady_clock::now();
    ClusterAggregator::ApplyIncrementalUpdates(grid, updates);
    auto u1 = std::chrono::steady_clock::now();
    const auto usInc = std::chrono::duration_cast<std::chrono::microseconds>(u1 - u0).count();
    std::cout << "    1000-extent incremental update: " << usInc << " us\n";

    // Invariants still hold after incremental updates
    for (const auto& cell : grid.cells) {
        ClusterCount sum = 0;
        for (auto c : cell.counts) sum += c;
        CHECK(sum == cell.totalClusters);
    }

    // Hit test
    const auto hit = ClusterAggregator::HitTest(grid, snap, 0, 0);
    CHECK(hit.valid);
    CHECK(hit.endLcn > hit.startLcn);
}

void TestProperty100Seeds() {
    PolicyGate policy;
    StructuredLogger logger;
    NullProgressSink sink;
    for (std::uint64_t seed = 1; seed <= 100; ++seed) {
        ScenarioSpec sc{};
        sc.id = "PROP-" + std::to_string(seed);
        sc.seed = seed * 7919ULL;
        sc.clusters = 2048;
        sc.fileCount = 30;
        sc.extentsPerFile = 3;
        sc.sizeMinClusters = 4;
        sc.sizeMaxClusters = 12;
        sc.freeTargetPct = 40.0;
        auto emuRes = BuildEmulatorFromScenario(sc);
        CHECK(emuRes.Ok());
        auto& emu = *emuRes.Value();
        std::string invErr;
        CHECK(emu.VerifyModelInvariants(&invErr));

        OperationController ctrl(policy, logger);
        CancelToken tok;
        BinaryJournal journal(TempTestDir() / "prop.journal");
        std::filesystem::remove(journal.Path());
        PlanOptions opts{};
        opts.mode = DefragMode::FullDefrag;
        AnalysisSnapshot before{}, after{};
        auto r = ctrl.RunOperation(emu, emu, opts, sink, journal, tok, true, &before, &after);
        CHECK(r.Ok());
        CHECK(emu.VerifyModelInvariants(&invErr));
        CHECK(after.summary.fragmentedFiles <= before.summary.fragmentedFiles);
    }
}

void TestJournalCrashRecovery200Trials() {
    auto jDir = TempTestDir() / "crash_campaign";
    std::filesystem::create_directories(jDir);

    // 200 crash/corruption trials covering all 5 crash points + torn header slots + torn tail records
    for (int trial = 0; trial < 200; ++trial) {
        ScenarioSpec sc{};
        sc.id = "CRASH-" + std::to_string(trial);
        sc.seed = 5000ULL + static_cast<std::uint64_t>(trial);
        sc.clusters = 4096;
        sc.fileCount = 24;
        sc.extentsPerFile = 4;
        sc.sizeMinClusters = 4;
        sc.sizeMaxClusters = 12;
        const std::uint64_t crashAt = static_cast<std::uint64_t>(trial % 8);
        sc.faults.push_back({FaultType::InterruptionMidMove, crashAt, kInvalidFileId, 995, false});

        auto emuRes = BuildEmulatorFromScenario(sc);
        CHECK(emuRes.Ok());
        auto& emu = *emuRes.Value();

        PolicyGate policy;
        StructuredLogger logger;
        NullProgressSink sink;
        OperationController ctrl(policy, logger);
        CancelToken tok;

        AnalysisSnapshot snapBefore{};
        CHECK(ctrl.Analyze(emu, emu, sink, tok).Ok());
        snapBefore = ctrl.Analyze(emu, emu, sink, tok).Value();

        DefragPlanner planner;
        PlanOptions opts{};
        opts.mode = DefragMode::FullDefrag;
        auto planRes = planner.Plan(snapBefore, opts, policy);
        CHECK(planRes.Ok());
        const auto plan = planRes.Value();

        auto jPath = jDir / ("trial_" + std::to_string(trial % 4) + ".journal");
        std::filesystem::remove(jPath);

        {
            BinaryJournal j(jPath);
            OnlineMode om(emu, emu, policy, logger, true, true);
            auto execRes = om.ExecuteWithOutcome(plan, sink, j, tok, nullptr);
            if (crashAt < plan.moves.size()) {
                CHECK(!execRes.Ok());
                CHECK(execRes.Error().code == ErrorCode::E_MOVE_INTERRUPTED);
            }
        }

        // Inject torn header slot OR trailing partial record corruption on alternating trials
        if (trial % 3 == 0) {
            // Corrupt header slot 0 or 1 (simulate torn header write; other valid slot must recover!)
            std::fstream fs(jPath, std::ios::binary | std::ios::in | std::ios::out);
            const std::uint64_t corruptOffset = (trial % 2 == 0) ? 0ULL : 512ULL;
            fs.seekp(static_cast<std::streamoff>(corruptOffset));
            const char junk[16] = "TORN_HDR_WRITE!";
            fs.write(junk, 16);
        } else if (trial % 3 == 1) {
            // Append torn partial record bytes at EOF
            std::ofstream ofs(jPath, std::ios::binary | std::ios::app);
            const char partial[23] = "PARTIAL_RECORD_BYTES!!";
            ofs.write(partial, sizeof(partial));
        }

        // Recover & resume
        BinaryJournal jRec(jPath);
        auto resRes = ctrl.ResumeOperation(emu, emu, plan, sink, jRec, tok, true);
        CHECK(resRes.Ok());
        CHECK(emu.VerifyModelInvariants());
    }
}

void TestAll20Scenarios() {
    const std::filesystem::path scDir = "tests/scenarios";
    PolicyGate policy;
    StructuredLogger logger;

    for (int i = 1; i <= 20; ++i) {
        char fname[32]{};
        std::snprintf(fname, sizeof(fname), "SC-%02d.json", i);
        auto scRes = LoadScenarioFile(scDir / fname);
        CHECK(scRes.Ok());
        const auto& spec = scRes.Value();

        auto emuRes = BuildEmulatorFromScenario(spec);
        CHECK(emuRes.Ok());
        auto& emu = *emuRes.Value();
        CHECK(emu.VerifyModelInvariants());

        // Determinism check: second build with same scenario produces identical geometry & file count
        auto emuRes2 = BuildEmulatorFromScenario(spec);
        CHECK(emuRes2.Ok());
        CHECK(emu.Geometry().Value().freeClusters == emuRes2.Value()->Geometry().Value().freeClusters);
        CHECK(emu.Enumerate(100000).Value().size() == emuRes2.Value()->Enumerate(100000).Value().size());

        OperationController ctrl(policy, logger);
        NullProgressSink sink;
        CancelToken tok;
        auto jPath = TempTestDir() / (spec.id + ".journal");
        std::filesystem::remove(jPath);
        BinaryJournal journal(jPath);

        PlanOptions opts{};
        if (i == 2) opts.mode = DefragMode::DefragFreeSpace;
        else if (i == 3) { opts.mode = DefragMode::MftOptimize; opts.mftTier = MftTier::Tier3ReportOnly; }
        else if (i == 4) opts.mode = DefragMode::MftZoneEvacuate;
        else if (i == 6) opts.mode = DefragMode::MoveLargeToEnd;
        else opts.mode = DefragMode::FullDefrag;

        AnalysisSnapshot before{}, after{};
        auto r = ctrl.RunOperation(emu, emu, opts, sink, journal, tok, true, &before, &after);

        if (spec.expectPreflightFail) {
            CHECK(!r.Ok());
            CHECK(emu.MoveCallCount() == 0);
            if (i == 14) CHECK(r.Error().code == ErrorCode::E_PREFLIGHT_SSD_BLOCKED);
            if (i == 13) CHECK(r.Error().code == ErrorCode::E_PREFLIGHT_DIRTY);
            if (i == 12) CHECK(r.Error().code == ErrorCode::E_PREFLIGHT_READ_ONLY);
            if (i == 16) CHECK(r.Error().code == ErrorCode::E_PREFLIGHT_LOCKED_BITLOCKER);
            if (i == 9) CHECK(r.Error().code == ErrorCode::E_PREFLIGHT_INSUFFICIENT_SPACE);
        } else if (i == 8 || i == 19) {
            CHECK(!r.Ok());
            CHECK(r.Error().code == ErrorCode::E_MOVE_INTERRUPTED);
        } else {
            CHECK(r.Ok());
            if (spec.expectZeroMoves) {
                CHECK(r.Value().stepsCompleted == 0);
            }
            if (i == 1) {
                CHECK(after.summary.volumeFragmentationPct <= spec.fragmentationAfterMax);
            }
            if (i == 2) {
                CHECK(after.summary.largestFreeRunClusters >= before.summary.largestFreeRunClusters);
            }
            if (i == 4) {
                CHECK(after.mftZone.occupiedByUserClusters * 5 <= before.mftZone.occupiedByUserClusters); // >=80% evacuated
            }
            if (i == 6) {
                const Lcn top10 = (after.geometry.totalClusters * 9ULL) / 10ULL;
                ClusterCount inTop10 = 0;
                ClusterCount totalLarge = 0;
                for (const auto& f : after.files) {
                    if (f.sizeBytes >= opts.largeFileThresholdBytes) {
                        for (const auto& e : f.extents) {
                            totalLarge += e.clusterCount;
                            if (e.startLcn >= top10) inTop10 += e.clusterCount;
                        }
                    }
                }
                CHECK(totalLarge > 0);
                CHECK((inTop10 * 100ULL) / totalLarge >= 90ULL);
            }
            if (i == 17) {
                const std::string csv = CsvExporter::BuildFileListCsv(after);
                CHECK(csv.find("\xE6\x97\xA5\xE6\x9C\xAC") != std::string::npos);
            }
        }
        CHECK(emu.VerifyModelInvariants());
    }
}

void TestCsvInjectionSafetyAndStatistics() {
    AnalysisSnapshot snap{};
    snap.volumeId = "SIM-CSV";
    snap.files = {
        {10, "=cmd|' /C calc'!A0", "\\=evil.txt", 4096, 1, {}, FileCategory::Other, FileState::Ready, SkipReason::None, {{0, 10, 1}}, 1, 0},
        {11, "+SUM(1,2)", "\\+plus.txt", 8192, 2, {}, FileCategory::Other, FileState::Fragmented, SkipReason::None, {{0, 20, 1}, {1, 30, 1}}, 2, 0},
        {12, "-1+1", "\\-minus.txt", 4096, 1, {}, FileCategory::Other, FileState::Ready, SkipReason::None, {{0, 40, 1}}, 1, 0},
        {13, "@macro", "\\@at.txt", 4096, 1, {}, FileCategory::Other, FileState::Ready, SkipReason::None, {{0, 50, 1}}, 1, 0},
    };
    const std::string csv = CsvExporter::BuildFileListCsv(snap);
    CHECK(csv.rfind("\xEF\xBB\xBF", 0) == 0); // UTF-8 BOM
    CHECK(csv.find("\"'=cmd") != std::string::npos);
    CHECK(csv.find("'+SUM") != std::string::npos);
    CHECK(csv.find("'-1+1") != std::string::npos);
    CHECK(csv.find("'@macro") != std::string::npos);

    const auto hist = StatisticsEngine::ComputeHistogram(snap);
    CHECK(hist.totalFiles == 4);
    CHECK(hist.bins[0] == 3);
    CHECK(hist.bins[1] == 1);
    const auto top = StatisticsEngine::ComputeTopN(snap, 10);
    CHECK(top.size() == 1 && top[0].id == 11);
}

void TestCliAndSchedulerSsdPolicy() {
    // Scheduler refuses SSD & Unknown
    ScheduledTaskSpec ts{};
    ts.volumeId = "E:";
    ts.mediaClass = MediaClass::SolidState;
    CHECK(TaskSchedulerManager::RegisterTask(ts).Error().code == ErrorCode::E_PREFLIGHT_SSD_BLOCKED);
    ts.mediaClass = MediaClass::Unknown;
    CHECK(TaskSchedulerManager::RegisterTask(ts).Error().code == ErrorCode::E_PREFLIGHT_SSD_BLOCKED);

    // CLI refuses --force / --override-ssd
    {
        const char* argv[] = {"auralite-cli", "defrag", "--force"};
        CliOptions opts{};
        std::string err;
        CHECK(!CliApp::ParseArgs(3, argv, opts, err));
    }
    // CLI SSD scenario returns exit code 12
    {
        const char* argv[] = {"auralite-cli", "defrag", "--sim", "tests/scenarios/SC-14.json"};
        CliOptions opts{};
        std::string err;
        CHECK(CliApp::ParseArgs(4, argv, opts, err));
        std::ostringstream outStream, errStream;
        CHECK(CliApp::Run(opts, outStream, errStream) == 12);
    }
    // CLI JSON output & dry-run
    {
        const char* argv[] = {"auralite-cli", "defrag", "--sim", "tests/scenarios/SC-01.json", "--dry-run", "--format", "json"};
        CliOptions opts{};
        std::string err;
        CHECK(CliApp::ParseArgs(7, argv, opts, err));
        std::ostringstream outStream, errStream;
        CHECK(CliApp::Run(opts, outStream, errStream) == 0);
        auto parsed = ParseJsonStrict(outStream.str());
        CHECK(parsed.Ok());
        CHECK(parsed.Value().Find("dryRun")->AsBool() == true);
        CHECK(parsed.Value().Find("stepsCompleted")->AsInt() == 0);
    }
}

} // namespace

int main() {
    std::cout << "Running AuraLite-Defrag Test Suite...\n";
    RUN_TEST(TestErrorTaxonomyAndMapping);
    RUN_TEST(TestStructuredLoggerAndRotation);
    RUN_TEST(TestConfigValidationAndPersistence);
    RUN_TEST(TestStringsFormattingAndThemeContrast);
    RUN_TEST(TestJsonFuzzingAndLimits);
    RUN_TEST(TestClusterAggregatorInvariantsAndPerf);
    RUN_TEST(TestProperty100Seeds);
    RUN_TEST(TestJournalCrashRecovery200Trials);
    RUN_TEST(TestAll20Scenarios);
    RUN_TEST(TestCsvInjectionSafetyAndStatistics);
    RUN_TEST(TestCliAndSchedulerSsdPolicy);

    std::cout << "\nSummary: " << (gTestsRun - gTestsFailed) << "/" << gTestsRun << " passed.\n";
    return gTestsFailed == 0 ? 0 : 1;
}
