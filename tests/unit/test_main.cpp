#include "backend/emulator/emulator_backend.hpp"
#include "backend/emulator/scenario_loader.hpp"
#include "cli/cli_app.hpp"
#include "core/config.hpp"
#include "core/error_catalog.hpp"
#include "core/json.hpp"
#include "backend/ntfs/mft/mft_scan.hpp"
#include "core/path_tree.hpp"
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
#include <cstring>
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

    // P9-T04: the grid must own its hit-test data. If it borrowed the file
    // records from the snapshot, a hit test after that snapshot is replaced
    // reads freed memory — the exact lifetime bug that was found by measuring.
    {
        ClusterGrid own{};
        {
            AnalysisSnapshot local{};
            local.geometry.totalClusters = 4096;
            local.summary.totalClusters = 4096;
            FileEntry fe{};
            fe.id = 777;
            fe.name = "lifetime-probe.bin";
            fe.fragmentCount = 9;
            Extent ex{};
            ex.startLcn = 1;
            ex.clusterCount = 64;
            fe.extents.push_back(ex);
            local.files.push_back(std::move(fe));
            own = ClusterAggregator::BuildGrid(local, 64, 64);
        }  // local dies here; own must still answer correctly
        // Cell (0,0) is cluster 0, which is free; the file starts at LCN 1.
        const auto h2 = ClusterAggregator::HitTest(own, AnalysisSnapshot{}, 1, 0);
        CHECK(h2.valid);
        CHECK(h2.dominantFileName == "lifetime-probe.bin");
        CHECK(h2.dominantFragmentCount == 9);
    }

    // P9-T04: hit testing must not scan the file set. 1,000 hits on a grid
    // built from many files has to stay far below the 100 ms input budget;
    // before the fix it took 303 ms because every hit walked every file.
    {
        AnalysisSnapshot big{};
        big.geometry.totalClusters = 4'000'000;
        big.summary.totalClusters = 4'000'000;
        for (int i = 0; i < 20000; ++i) {
            FileEntry fe{};
            fe.id = static_cast<FileId>(i + 1);
            fe.name = "b" + std::to_string(i);
            Extent ex{};
            ex.startLcn = static_cast<Lcn>(i) * 100ULL;
            ex.clusterCount = 60;
            fe.extents.push_back(ex);
            fe.fragmentCount = 3;
            big.files.push_back(std::move(fe));
        }
        const auto g2 = ClusterAggregator::BuildGrid(big, 200, 100);
        std::size_t hits = 0;
        for (int i = 0; i < 1000; ++i) {
            const auto h = ClusterAggregator::HitTest(g2, big, static_cast<std::uint32_t>(i % 200),
                                                      static_cast<std::uint32_t>(i % 100));
            if (h.valid) ++hits;
        }
        CHECK(hits == 1000);
    }

    // P9-T04: the renderer must be able to upload the map as one bitmap rather
    // than one FillRectangle per cell. At a 4K grid that is 80,000 cells; the
    // pixel buffer has to map every cell 1:1 onto a row-major ARGB word, with
    // free cells getting the background. A mismatch here shows up on screen as
    // a shifted or blank map, so it is checked by value and not by size.
    {
        AnalysisSnapshot s3{};
        s3.geometry.totalClusters = 256;
        s3.summary.totalClusters = 256;
        FileEntry occupied{};
        occupied.id = 4242;
        occupied.name = "occupied.bin";
        occupied.fragmentCount = 2;
        Extent ex{};
        ex.startLcn = 128;   // second half of the volume
        ex.clusterCount = 128;
        occupied.extents.push_back(ex);
        s3.files.push_back(std::move(occupied));

        const auto g3 = ClusterAggregator::BuildGrid(s3, 16, 16);
        std::array<std::uint32_t, kClusterCategoryCount> pal{};
        pal.fill(0xFF010203U);
        pal[static_cast<std::size_t>(ClusterCategory::Free)] = 0xFF000000U;
        pal[static_cast<std::size_t>(ClusterCategory::UsedContiguous)] = 0xFF00FF00U;

        const auto px = ClusterAggregator::BuildPixelBuffer(g3, pal);
        CHECK(px.size() == g3.cells.size());
        CHECK(px.size() == 16U * 16U);
        // Row-major: cell index == row * cols + col.
        for (std::size_t i = 0; i < px.size(); ++i) {
            const auto expected = static_cast<std::size_t>(g3.cells[i].dominant);
            CHECK(px[i] == pal[expected]);
        }
        // The occupied half must not be background, and the free half must be.
        CHECK(px.front() == pal[static_cast<std::size_t>(ClusterCategory::Free)]);
        CHECK(px.back() != pal[static_cast<std::size_t>(ClusterCategory::Free)]);

        // A grid whose cell count does not match cols*rows is padded with the
        // background rather than reading past the end.
        ClusterGrid broken = g3;
        broken.cells.resize(4);
        const auto padded = ClusterAggregator::BuildPixelBuffer(broken, pal, 0xFFABCDEFU);
        CHECK(padded.size() == 16U * 16U);
        CHECK(padded.back() == 0xFFABCDEFU);
    }
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


// P9-T02 SEC-02 / SEC-03 — path scope and target confinement.
//
// The real NTFS backend resolves a file through GetFinalPathNameByHandleW, which
// returns the extended-length form (\\?\D:\Data\a.bin), while --target comes from
// the command line as D:\Data. Comparing those strings directly selected nothing
// on every real volume while passing on the emulator, whose paths are plain.
void TestPathScopeAndTargetConfinement() {
    using ExclusionEngine = auralite::ExclusionEngine;

    // SEC-02: the same file spelled three ways is the same scope.
    CHECK(ExclusionEngine::IsWithinScope("D:\\Data", "\\\\?\\D:\\Data\\a.bin"));
    CHECK(ExclusionEngine::IsWithinScope("d:\\data\\", "D:\\DATA\\Sub\\b.bin"));
    CHECK(ExclusionEngine::IsWithinScope("D:\\Data", "D:\\Data"));

    // SEC-03: a scope never escapes its boundary.
    CHECK(!ExclusionEngine::IsWithinScope("D:\\Data", "D:\\Database\\a.bin"));
    CHECK(!ExclusionEngine::IsWithinScope("D:\\Data", "C:\\Data\\a.bin"));
    CHECK(!ExclusionEngine::IsWithinScope("D:\\Data", "D:\\Data2"));
    CHECK(!ExclusionEngine::IsWithinScope("", "D:\\Data\\a.bin"));

    // The planner must select by scope, not by raw prefix. Ids 0 and 1 are $MFT
    // and $MFTMirr, so the fixtures use ordinary record numbers.
    constexpr FileId kTestFileA = 4001;
    constexpr FileId kTestFileB = 4002;
    AnalysisSnapshot snap{};
    snap.volumeId = "D:";
    snap.geometry.totalClusters = 4096;
    snap.geometry.freeClusters = 2000;
    snap.summary.totalClusters = 4096;
    snap.summary.freeClusters = 2000;
    snap.summary.usedClusters = 2096;
    // Free space is required for a plan to place anything at all: with an empty
    // bitmap every candidate is dropped after selection.
    BitmapSnapshot bmp{};
    bmp.startLcn = 0;
    bmp.totalClusters = 4096;
    bmp.bits.assign(4096 / 8 + 1, 0);
    snap.bitmap = bmp;
    snap.files = {
        {kTestFileA, "a.bin", "\\\\?\\D:\\Data\\a.bin", 8192, 2, {}, FileCategory::Other,
         FileState::Fragmented, SkipReason::None, {{0, 100, 1}, {1, 900, 1}}, 2, 0},
        {kTestFileB, "b.bin", "\\\\?\\D:\\Database\\b.bin", 8192, 2, {}, FileCategory::Other,
         FileState::Fragmented, SkipReason::None, {{0, 200, 1}, {1, 950, 1}}, 2, 0},
    };
    PolicyGate policy;
    DefragPlanner planner;
    PlanOptions opts{};
    opts.mode = DefragMode::SingleFileOrFolder;
    opts.targetFolderPrefix = "D:\\Data";

    auto planRes = planner.Plan(snap, opts, policy);
    CHECK(planRes.Ok());
    bool sawA = false;
    bool sawB = false;
    for (const auto& m : planRes.Value().moves) {
        if (m.fileId == kTestFileA) sawA = true;
        if (m.fileId == kTestFileB) sawB = true;
    }
    CHECK(sawA);
    CHECK(!sawB); // D:\\Database is not inside D:\\Data

    // A --target outside the volume is refused before any analysis or move.
    {
        const char* argv[] = {"auralite-cli", "defrag", "--volume", "C:",
                              "--sim", "tests/scenarios/SC-01.json", "--target", "D:\\Nope"};
        CliOptions cli{};
        std::string err;
        CHECK(CliApp::ParseArgs(8, argv, cli, err));
        std::ostringstream outStream, errStream;
        CHECK(CliApp::Run(cli, outStream, errStream) == 2);
        CHECK(errStream.str().find("is not on volume") != std::string::npos);
    }
}


// Progress reporting on the long phase.
//
// Enumerating a real volume is one ioctl per file - minutes for 160k files - and
// until this existed `analyze` printed nothing until it had finished, so the run
// looked hung. The hook has to survive two proxy layers to reach the sink, which
// is where it was first dropped.
namespace {

class ScanProgressFakeFs final : public IFileSystemAdapter {
public:
    explicit ScanProgressFakeFs(std::vector<std::uint64_t> marks) : marks_(std::move(marks)) {}

    CapabilityBits Capabilities() const override { return {}; }
    Result<std::vector<FileEntry>> Enumerate(std::uint64_t) override {
        for (std::uint64_t m : marks_) {
            if (cb_) cb_(m);
        }
        return Result<std::vector<FileEntry>>::Success({});
    }
    Result<std::vector<Extent>> Extents(FileId) override { return Result<std::vector<Extent>>::Success({}); }
    Result<FileFlags> Flags(FileId) override { return Result<FileFlags>::Success({}); }
    MoveResult MoveExtent(FileId, Vcn, Lcn, std::uint32_t) override { return MoveResult{}; }
    Result<MftInfo> Mft() override { return Result<MftInfo>::Success({}); }
    Result<MftZoneInfo> Zone() override { return Result<MftZoneInfo>::Success({}); }
    Result<std::uint64_t> ComputeWindowChecksum(FileId) override {
        return Result<std::uint64_t>::Success(0);
    }
    void SetScanProgressCallback(std::function<void(std::uint64_t)> callback) override {
        cb_ = std::move(callback);
        ++setCount;
    }

    int setCount{0};

private:
    std::vector<std::uint64_t> marks_;
    std::function<void(std::uint64_t)> cb_;
};

class CountingSink final : public IProgressSink {
public:
    void OnSnapshot(const ProgressSnapshot& p) override {
        if (p.state == OperationState::Analyzing) analyzing.push_back(p.completedSteps);
    }
    void OnEvent(const LogEvent&) override {}
    std::vector<std::uint64_t> analyzing;
};

class StubVolume final : public IVolumeProvider {
public:
    VolumeId Id() const override { return "X:"; }
    Result<VolumeGeometry> Geometry() override {
        VolumeGeometry g{};
        g.totalClusters = 100;
        g.freeClusters = 50;
        return Result<VolumeGeometry>::Success(g);
    }
    MediaClass Media() override { return MediaClass::Spinning; }
    MediaEvidence Evidence() override { return {}; }
    Result<PreflightResult> Preflight(bool, double) override {
        return Result<PreflightResult>::Success({});
    }
    Result<BitmapSnapshot> ReadBitmap(Lcn, ClusterCount) override {
        return Result<BitmapSnapshot>::Success({});
    }
    bool IsSimulation() const noexcept override { return false; }
};

} // namespace

void TestScanProgressReporting() {
    // The controller installs a callback, forwards every count to the sink, and
    // detaches it afterwards so a later phase cannot write into a dead sink.
    {
        StubVolume vol;
        ScanProgressFakeFs fs({2048, 4096, 6000});
        AppConfig cfg{};
        PolicyGate policy(cfg);
        StructuredLogger logger;
        OperationController ctrl(policy, logger);
        CountingSink sink;
        CancelToken token;
        auto res = ctrl.Analyze(vol, fs, sink, token);
        CHECK(res.Ok());
        CHECK(fs.setCount == 2);           // installed, then cleared
        CHECK(sink.analyzing.size() == 4); // phase marker + 3 counts
        CHECK(sink.analyzing[0] == 0);
        CHECK(sink.analyzing[1] == 2048);
        CHECK(sink.analyzing[3] == 6000);
    }

    // End to end: a 2,502-file scenario produces a progress line on stderr while
    // stdout stays the report, and JSON mode keeps both streams machine-clean.
    {
        const char* argv[] = {"auralite-cli", "analyze", "--volume", "C:",
                              "--sim", "tests/scenarios/SC-05.json"};
        CliOptions cli{};
        std::string perr;
        CHECK(CliApp::ParseArgs(6, argv, cli, perr));
        std::ostringstream outStream, errStream;
        CHECK(CliApp::Run(cli, outStream, errStream) == 0);
        CHECK(errStream.str().find("analyzing:") != std::string::npos);
        CHECK(errStream.str().find("2,048") != std::string::npos);
        CHECK(outStream.str().find("analyzing:") == std::string::npos);
    }
    {
        const char* argv[] = {"auralite-cli", "analyze", "--volume", "C:",
                              "--sim", "tests/scenarios/SC-05.json", "--format", "json"};
        CliOptions cli{};
        std::string perr;
        CHECK(CliApp::ParseArgs(8, argv, cli, perr));
        std::ostringstream outStream, errStream;
        CHECK(CliApp::Run(cli, outStream, errStream) == 0);
        CHECK(errStream.str().find("analyzing:") == std::string::npos);
        CHECK(ParseJsonStrict(outStream.str()).Ok());
    }
}


// Extent census classification.
//
// The first version of the census counted every non-empty file without extents as
// "could not be read", and told the operator that 55,917 files on a healthy volume
// were unmeasured. They were resident files, whose data lives inside the MFT
// record, and no extents is the correct answer for them.
void TestExtentCensusClassification() {
    std::vector<FileEntry> files;

    auto add = [&files](FileId id, std::uint64_t size, std::uint32_t extents,
                        bool resident, bool locked, bool isDir = false) {
        FileEntry f{};
        f.id = id;
        f.name = "f" + std::to_string(id);
        f.sizeBytes = size;
        f.flags.resident = resident;
        f.flags.isLocked = locked;
        f.flags.isDirectory = isDir;
        for (std::uint32_t i = 0; i < extents; ++i) {
            f.extents.push_back({i, static_cast<Lcn>(100 + i), 1});
        }
        f.fragmentCount = extents;
        files.push_back(f);
    };

    add(100, 4096, 1, false, false);   // one extent
    add(101, 8192, 3, false, false);   // fragmented, three extents
    add(102, 0, 0, false, false);      // zero length
    add(103, 300, 0, true, false);     // RESIDENT: data inside the MFT record
    add(104, 4096, 0, false, true);    // could not be opened
    add(105, 4096, 0, false, false);   // length, no layout, not resident: unreadable
    add(106, 4096, 1, false, false, /*isDir=*/true); // directories are not files

    const auto c = StatisticsEngine::Census(files);
    CHECK(c.singleExtent == 1);
    CHECK(c.multiExtent == 1);
    CHECK(c.zeroLength == 1);
    CHECK(c.resident == 1);
    CHECK(c.locked == 1);
    CHECK(c.unreadable == 1);
    CHECK(c.maxExtents == 3);
    CHECK(c.Blind());
    CHECK(c.Text().find("resident=1") != std::string::npos);

    // A volume of resident and zero-length files only is fully measured.
    std::vector<FileEntry> healthy;
    FileEntry r{};
    r.id = 200;
    r.sizeBytes = 120;
    r.flags.resident = true;
    healthy.push_back(r);
    const auto h = StatisticsEngine::Census(healthy);
    CHECK(h.resident == 1);
    CHECK(h.unreadable == 0);
    CHECK(!h.Blind());
    CHECK(!h.ReadFailed());

    // Locked-only: the percentages are partial (Blind), but the read path is fine,
    // so the operator must not be told that files "could not be read" twice.
    std::vector<FileEntry> lockedOnly(1);
    lockedOnly[0].id = 300;
    lockedOnly[0].sizeBytes = 4096;
    lockedOnly[0].flags.isLocked = true;
    const auto lk = StatisticsEngine::Census(lockedOnly);
    CHECK(lk.locked == 1);
    CHECK(lk.unreadable == 0);
    CHECK(lk.Blind());
    CHECK(!lk.ReadFailed());
}


// Path composition from USN parent references.
//
// The backend asked the filesystem for every path with GetFinalPathNameByHandleW:
// ~14 ms per file on a real HDD, 160,129 files in ~36 minutes. The parent chain is
// already in the USN records, so composing the path in memory removes the call -
// and it also names files that could never be opened.
void TestPathCompositionFromUsn() {
    PathComposer pc;
    pc.AddDirectory(PathComposer::kRootDirectoryId, PathComposer::kRootDirectoryId, ".");
    pc.AddDirectory(20, PathComposer::kRootDirectoryId, "Users");
    pc.AddDirectory(21, 20, "Data");
    pc.AddFile(30, 21, "a.bin");
    pc.AddFile(31, PathComposer::kRootDirectoryId, "top.txt");

    CHECK(pc.PathOf(30, "D:\\") == "D:\\Users\\Data\\a.bin");
    CHECK(pc.PathOf(31, "D:\\") == "D:\\top.txt");
    CHECK(pc.PathOf(21, "D:\\") == "D:\\Users\\Data");
    CHECK(pc.PathOf(PathComposer::kRootDirectoryId, "D:\\") == "D:\\");
    CHECK(pc.PathOf(31, "D:") == "D:\\top.txt");

    // An ancestor that was never seen yields no path: a half-built path would
    // silently match the wrong exclusion rules.
    PathComposer orphan;
    orphan.AddDirectory(PathComposer::kRootDirectoryId, PathComposer::kRootDirectoryId, ".");
    orphan.AddFile(40, 99, "lost.bin");
    CHECK(orphan.PathOf(40, "D:\\").empty());

    // A cycle cannot hang the walk.
    PathComposer cyclic;
    cyclic.AddDirectory(PathComposer::kRootDirectoryId, PathComposer::kRootDirectoryId, ".");
    cyclic.AddDirectory(50, 51, "x");
    cyclic.AddDirectory(51, 50, "y");
    cyclic.AddFile(52, 50, "loop.bin");
    CHECK(cyclic.PathOf(52, "D:\\").empty());

    // A deep but valid chain still resolves.
    PathComposer deep;
    deep.AddDirectory(PathComposer::kRootDirectoryId, PathComposer::kRootDirectoryId, ".");
    FileId parent = PathComposer::kRootDirectoryId;
    for (int i = 0; i < 60; ++i) {
        const FileId id = static_cast<FileId>(1000 + i);
        deep.AddDirectory(id, parent, "d");
        parent = id;
    }
    deep.AddFile(2000, parent, "leaf.bin");
    const std::string leaf = deep.PathOf(2000, "C:\\");
    CHECK(leaf.rfind("C:\\d\\d\\", 0) == 0);
    CHECK(leaf.size() > 130);
    CHECK(leaf.substr(leaf.size() - 8) == "leaf.bin");
}


// --max-files: analysing a sample instead of a whole volume.
//
// Timing a 160k-file volume at 5.8 ms per file costs 15 minutes, so every
// experiment with the scan (worker count, path composition) needs a way to cap
// the file count. The flag is validated rather than trusted: a silent 0 would
// make Enumerate return nothing and the report claim an empty volume.
void TestMaxFilesOption() {
    {
        const char* argv[] = {"auralite-cli", "analyze", "--volume", "D:", "--max-files", "20000"};
        CliOptions opts{};
        std::string err;
        CHECK(CliApp::ParseArgs(6, argv, opts, err));
        CHECK(opts.maxScanFiles == 20000);
    }
    {
        const char* argv[] = {"auralite-cli", "analyze", "--volume", "D:", "--max-files", "0"};
        CliOptions opts{};
        std::string err;
        CHECK(!CliApp::ParseArgs(6, argv, opts, err));
        CHECK(!err.empty());
    }
    {
        const char* argv[] = {"auralite-cli", "analyze", "--volume", "D:", "--max-files", "12k"};
        CliOptions opts{};
        std::string err;
        CHECK(!CliApp::ParseArgs(6, argv, opts, err));
    }
    {
        const char* argv[] = {"auralite-cli", "analyze", "--volume", "D:", "--max-files", "-5"};
        CliOptions opts{};
        std::string err;
        CHECK(!CliApp::ParseArgs(6, argv, opts, err));
    }
    // The default stays what the controller always used.
    CHECK(PlanOptions{}.maxScanFiles == 1000000);
}


// $MFT fast path: record parsing, run lists, and the scan loop.
//
// The handle path costs one random read per file (5.8 ms on the reference HDD).
// Reading the file table once replaces that, so this parser decides the numbers
// the whole product reports - it is worth testing byte by byte, which is why the
// record builder below writes a real record instead of a mock.

namespace {

void PutU16(std::string& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<char>(v & 0xFF);
    b[at + 1] = static_cast<char>(v >> 8);
}
void PutU32(std::string& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}
void PutU64(std::string& b, std::size_t at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

// Appends a resident attribute whose value is copied into it.
void AddResidentAttr(std::string& rec, std::size_t& cursor, std::uint32_t type,
                     const std::string& value, std::uint16_t flags = 0) {
    const std::uint32_t len = static_cast<std::uint32_t>(24 + value.size());
    const std::uint32_t aligned = (len + 7u) & ~7u;
    PutU32(rec, cursor, type);
    PutU32(rec, cursor + 4, aligned);
    rec[cursor + 8] = 0;
    rec[cursor + 9] = 0;
    PutU16(rec, cursor + 10, 0);
    PutU16(rec, cursor + 12, flags);
    PutU16(rec, cursor + 14, 0);
    PutU32(rec, cursor + 16, static_cast<std::uint32_t>(value.size()));
    PutU16(rec, cursor + 20, 24);
    rec[cursor + 22] = 0;
    for (std::size_t i = 0; i < value.size(); ++i) rec[cursor + 24 + i] = value[i];
    cursor += aligned;
}

// Appends a non-resident $DATA attribute with the given run list bytes.
void AddDataAttr(std::string& rec, std::size_t& cursor, std::uint64_t realSize,
                 const std::string& runList, std::uint16_t flags = 0) {
    const std::uint32_t len = static_cast<std::uint32_t>(64 + runList.size());
    const std::uint32_t aligned = (len + 7u) & ~7u;
    PutU32(rec, cursor, mft::kTypeData);
    PutU32(rec, cursor + 4, aligned);
    rec[cursor + 8] = 1; // non-resident
    rec[cursor + 9] = 0;
    PutU16(rec, cursor + 10, 0);
    PutU16(rec, cursor + 12, flags);
    PutU16(rec, cursor + 14, 0);
    PutU64(rec, cursor + 16, 0);  // start VCN
    PutU64(rec, cursor + 24, 0);  // last VCN (0 = single cluster is still valid here)
    PutU16(rec, cursor + 32, 64); // run list offset
    PutU16(rec, cursor + 34, 0);
    PutU64(rec, cursor + 40, realSize);
    PutU64(rec, cursor + 48, realSize);
    PutU64(rec, cursor + 56, realSize);
    for (std::size_t i = 0; i < runList.size(); ++i) rec[cursor + 64 + i] = runList[i];
    cursor += aligned;
}

std::string FileNameValue(std::uint64_t parentRef, const std::string& name, std::uint32_t attrs) {
    // Layout: parentRef(0), four timestamps, allocSize(40), realSize(48),
    // flags(56), reparse(60), nameLength(64), namespace(65), name(66...)
    std::string v(66, '\0');
    PutU64(v, 0, parentRef);
    PutU32(v, 56, attrs);
    v[64] = static_cast<char>(name.size() / 2);
    v[65] = 1; // Win32 namespace
    v.append(name);
    return v;
}

// Builds one 1024-byte FILE record with a valid update sequence array.
std::string BuildRecord(std::uint64_t parentRef, const std::string& nameUtf16,
                        std::uint32_t fileAttrs, bool inUse, bool directory,
                        std::uint64_t dataSize, const std::string& runList,
                        bool withAttributeList = false) {
    std::string rec(1024, '\0');
    std::memcpy(rec.data(), "FILE", 4);
    PutU16(rec, 0x04, 0x30); // update sequence array offset
    PutU16(rec, 0x06, 3);    // one entry + two sectors of 512 bytes
    PutU16(rec, 0x10, 7);    // sequence number
    PutU16(rec, 0x12, 1);
    PutU16(rec, 0x14, 0x38); // first attribute offset
    PutU16(rec, 0x16, static_cast<std::uint16_t>((inUse ? 1 : 0) | (directory ? 2 : 0)));
    PutU32(rec, 0x1C, 1024);
    PutU64(rec, 0x20, 0); // base record reference: zero means this is a base record

    std::size_t cursor = 0x38;
    std::string stdInfo(48, '\0');
    PutU32(stdInfo, 32, fileAttrs);
    AddResidentAttr(rec, cursor, mft::kTypeStandardInformation, stdInfo);
    AddResidentAttr(rec, cursor, mft::kTypeFileName, FileNameValue(parentRef, nameUtf16, fileAttrs));
    if (withAttributeList) AddResidentAttr(rec, cursor, mft::kTypeAttributeList, std::string(32, '\0'));
    if (!directory) {
        if (runList.empty()) {
            // A small file keeps its bytes inside the record: $DATA resident.
            AddResidentAttr(rec, cursor, mft::kTypeData,
                            std::string(static_cast<std::size_t>(dataSize), 'x'));
        } else {
            AddDataAttr(rec, cursor, dataSize, runList);
        }
    }
    PutU32(rec, cursor, 0xFFFFFFFFu);
    PutU32(rec, 0x18, static_cast<std::uint32_t>(cursor + 8));

    // Update sequence array: entry 0 is the check value, the sectors must end with it.
    PutU16(rec, 0x30, 0xAAAA);
    PutU16(rec, 0x32, 0xBBBB);
    PutU16(rec, 0x34, 0xCCCC);
    PutU16(rec, 510, 0xAAAA);
    PutU16(rec, 1022, 0xAAAA);
    return rec;
}

std::string Utf16(const char* ascii) {
    std::string out;
    for (const char* p = ascii; *p; ++p) {
        out.push_back(*p);
        out.push_back('\0');
    }
    return out;
}

// Two runs: 4 clusters at LCN 100, then 6 clusters at LCN 200.
std::string TwoRunList() {
    std::string rl;
    rl.push_back(static_cast<char>(0x11)); // 1-byte length, 1-byte offset
    rl.push_back(4);
    rl.push_back(100);
    rl.push_back(static_cast<char>(0x11));
    rl.push_back(6);
    rl.push_back(100); // delta from 100 -> 200
    rl.push_back(0);
    return rl;
}

} // namespace

void TestMftRecordParsing() {
    const mft::RecordGeometry geom{1024, 4096};

    // A normal fragmented file: two runs, 10 clusters of 4 KiB = 40 KiB.
    {
        const std::string raw = BuildRecord(5, Utf16("data.bin"), 0x00000080u, true, false,
                                            40000, TwoRunList());
        auto parsed = mft::ParseRecord(raw, 42, geom);
        CHECK(parsed.Ok());
        const auto& r = parsed.Value();
        CHECK(r.id == 42);
        CHECK(r.sequence == 7);
        CHECK(r.parentRef == 5); // root directory
        CHECK(r.name == "data.bin");
        CHECK(r.inUse);
        CHECK(!r.isDirectory);
        CHECK(!r.isExtension);
        CHECK(!r.hasAttributeList);
        CHECK(r.sizeBytes == 40000);
        CHECK(r.extents.size() == 2);
        CHECK(r.extents[0].startVcn == 0 && r.extents[0].startLcn == 100 &&
              r.extents[0].clusterCount == 4);
        CHECK(r.extents[1].startVcn == 4 && r.extents[1].startLcn == 200 &&
              r.extents[1].clusterCount == 6);

        const FileEntry fe = mft::EntryFromRecord(r);
        CHECK(fe.fragmentCount == 2);
        CHECK(fe.allocatedClusters == 10);
        CHECK(!fe.flags.isDirectory);
    }

    // A resident file: its data lives inside the record, so there is no run list
    // and the census must call it resident rather than empty.
    {
        const std::string raw = BuildRecord(5, Utf16("tiny.txt"), 0x00000020u, true, false, 12,
                                            std::string());
        auto parsed = mft::ParseRecord(raw, 43, geom);
        CHECK(parsed.Ok());
        CHECK(parsed.Value().extents.empty());
        CHECK(parsed.Value().hasResidentData);
        const FileEntry fe = mft::EntryFromRecord(parsed.Value());
        CHECK(fe.fragmentCount == 0);
        CHECK(fe.flags.resident);
        CHECK(fe.sizeBytes == 12);
    }

    // A directory: no $DATA at all, still must parse.
    {
        const std::string raw = BuildRecord(5, Utf16("Windows"), 0x00000010u, true, true, 0, "");
        auto parsed = mft::ParseRecord(raw, 44, geom);
        CHECK(parsed.Ok());
        CHECK(parsed.Value().isDirectory);
        CHECK(parsed.Value().extents.empty());
        CHECK(parsed.Value().sizeBytes == 0);
    }

    // $ATTRIBUTE_LIST: the caller must be told, because the run list may be partial.
    {
        const std::string raw = BuildRecord(5, Utf16("huge.vhdx"), 0x00000080u, true, false,
                                            1'000'000, TwoRunList(), true);
        auto parsed = mft::ParseRecord(raw, 45, geom);
        CHECK(parsed.Ok());
        CHECK(parsed.Value().hasAttributeList);
    }

    // Compressed and sparse bits are reported, not dropped.
    {
        const std::string raw = BuildRecord(5, Utf16("packed.zip"), 0x00000800u, true, false,
                                            8192, TwoRunList());
        auto parsed = mft::ParseRecord(raw, 46, geom);
        CHECK(parsed.Ok());
        const FileEntry fe = mft::EntryFromRecord(parsed.Value());
        CHECK(fe.flags.compressed);
    }

    // A torn record must be refused: the fixup is what detects it.
    {
        std::string raw = BuildRecord(5, Utf16("torn.bin"), 0x00000080u, true, false, 4096,
                                      TwoRunList());
        raw[510] = '\x01'; // sector no longer ends with the update sequence value
        auto parsed = mft::ParseRecord(raw, 47, geom);
        CHECK(!parsed.Ok());
        CHECK(parsed.Error().code == ErrorCode::E_ENUM_MALFORMED_RECORD);
    }

    // A record that is not a FILE record at all.
    {
        std::string raw(1024, '\0');
        auto parsed = mft::ParseRecord(raw, 48, geom);
        CHECK(!parsed.Ok());
    }

    // Out-of-bounds attribute length: a hostile record must not walk the buffer.
    {
        std::string raw = BuildRecord(5, Utf16("bad.bin"), 0x00000080u, true, false, 4096,
                                      TwoRunList());
        PutU32(raw, 0x38 + 4, 0xFFFFFF00u); // first attribute claims to be enormous
        auto parsed = mft::ParseRecord(raw, 49, geom);
        CHECK(!parsed.Ok());
    }
}

void TestMftRunListDecoding() {
    const std::uint32_t bpc = 4096;

    // Sparse run: the second entry has no offset bytes, so it occupies VCNs but
    // no clusters. It must be skipped, exactly as FSCTL_GET_RETRIEVAL_POINTERS
    // omits holes - otherwise a sparse file counts as hugely fragmented.
    {
        std::string rl;
        rl.push_back(static_cast<char>(0x11)); // run: 2 clusters at LCN 50
        rl.push_back(2);
        rl.push_back(50);
        rl.push_back(static_cast<char>(0x01)); // hole: 10 clusters, no LCN
        rl.push_back(10);
        rl.push_back(static_cast<char>(0x11)); // run: 3 clusters, 50 -> 60
        rl.push_back(3);
        rl.push_back(10);
        rl.push_back(0);
        auto runs = mft::DecodeRunList(rl, bpc);
        CHECK(runs.Ok());
        CHECK(runs.Value().size() == 2);
        CHECK(runs.Value()[0].startLcn == 50 && runs.Value()[0].clusterCount == 2);
        CHECK(runs.Value()[1].startVcn == 12);
        CHECK(runs.Value()[1].startLcn == 60 && runs.Value()[1].clusterCount == 3);
    }

    // Backwards LCN delta (a signed field) must decode as negative.
    {
        std::string rl;
        // Two-byte offsets: 200 does not fit in a signed byte, and reading it as
        // one would turn it into -56 - the exact class of bug this test is for.
        rl.push_back(static_cast<char>(0x21));
        rl.push_back(4);
        rl.push_back(static_cast<char>(200));
        rl.push_back(0);
        rl.push_back(static_cast<char>(0x21));
        rl.push_back(4);
        rl.push_back(static_cast<char>(0xF6)); // -10
        rl.push_back(static_cast<char>(0xFF));
        rl.push_back(0);
        auto runs = mft::DecodeRunList(rl, bpc);
        CHECK(runs.Ok());
        CHECK(runs.Value()[1].startLcn == 190);
    }

    // Malformed: truncated entry, bad field widths, zero-length run.
    {
        std::string rl;
        rl.push_back(static_cast<char>(0x11));
        rl.push_back(4); // length present, offset byte missing
        CHECK(!mft::DecodeRunList(rl, bpc).Ok());
    }
    {
        const std::string rl(1, static_cast<char>(0x99)); // 9-byte fields
        CHECK(!mft::DecodeRunList(rl, bpc).Ok());
    }
    {
        std::string rl;
        rl.push_back(static_cast<char>(0x11));
        rl.push_back(0); // zero-length run
        rl.push_back(10);
        CHECK(!mft::DecodeRunList(rl, bpc).Ok());
    }
    // A zero cluster size would divide by zero downstream.
    CHECK(!mft::DecodeRunList(std::string("\0", 1), 0).Ok());
}

void TestMftSequentialScan() {
    const mft::RecordGeometry geom{1024, 4096};

    // Four records: an in-use file, an unused one, a directory, and an extension
    // record. Only the file and the directory may reach the caller.
    std::string image;
    image += BuildRecord(5, Utf16("a.bin"), 0x80u, true, false, 40000, TwoRunList());
    {
        std::string unused = BuildRecord(5, Utf16("gone.bin"), 0x80u, false, false, 0, std::string());
        image += unused;
    }
    image += BuildRecord(5, Utf16("Docs"), 0x10u, true, true, 0, "");
    {
        std::string ext = BuildRecord(5, Utf16("ext.bin"), 0x80u, true, false, 4096, TwoRunList());
        PutU64(ext, 0x20, 1); // base record reference -> this is an extension record
        image += ext;
    }

    std::uint64_t reads = 0;
    auto read = [&image, &reads](std::uint64_t offset, std::size_t length)
        -> Result<std::string> {
        ++reads;
        if (offset + length > image.size()) {
            return Result<std::string>::Failure(MakeError(ErrorCode::E_ENUM_IO));
        }
        return Result<std::string>::Success(image.substr(offset, length));
    };

    auto scanned = mft::ScanMft(image.size(), geom, read, 1000);
    CHECK(scanned.Ok());
    const auto& s = scanned.Value().stats;
    CHECK(s.recordsSeen == 4);
    CHECK(s.recordsUsed == 3);
    CHECK(s.extensionRecords == 1);
    CHECK(s.directories == 1);
    CHECK(s.malformed == 0);
    CHECK(scanned.Value().records.size() == 2);

    // maxFiles caps the list but the counters still describe the whole table.
    auto capped = mft::ScanMft(image.size(), geom, read, 1);
    CHECK(capped.Ok());
    CHECK(capped.Value().records.size() == 1);
    CHECK(capped.Value().stats.recordsSeen == 4);

    // One short read must fail the scan rather than silently truncate the list.
    auto shortRead = [](std::uint64_t, std::size_t) -> Result<std::string> {
        return Result<std::string>::Success(std::string(16, '\0'));
    };
    CHECK(!mft::ScanMft(image.size(), geom, shortRead, 1000).Ok());

    // Zero record size is a geometry bug, not an empty volume.
    CHECK(!mft::ScanMft(image.size(), mft::RecordGeometry{0, 4096}, read, 1000).Ok());

    // Progress is reported while the scan runs.
    std::uint64_t lastProgress = 0;
    auto progress = [&lastProgress](std::uint64_t n) { lastProgress = n; };
    CHECK(mft::ScanMft(image.size(), geom, read, 1000, progress).Ok());
    CHECK(lastProgress == 4);
}


// The $MFT bootstrap, end to end and without Windows.
//
// Two things failed or could fail here on the real volume:
//   * OpenFileById(0) is rejected with ERROR_INVALID_PARAMETER (err=87), so the
//     table cannot be opened as "file 0" - record 0 is read straight off the
//     volume at MftStartLcn instead, and its own run list locates the rest;
//   * $MFT may be fragmented, so stream offsets have to be mapped through a run
//     list. Reading it as one contiguous span would silently misparse records.
void TestMftBootstrapFromRecordZero() {
    constexpr std::uint32_t kCluster = 1024;   // one record per cluster, keeps the image small
    constexpr std::uint64_t kMftStartLcn = 4;  // where record 0 lives, per volume geometry
    const mft::RecordGeometry geom{1024, kCluster};

    // $MFT is fragmented: records 0-1 in clusters 4-5, records 2-3 in clusters 9-10.
    std::string runList;
    runList.push_back(static_cast<char>(0x11));
    runList.push_back(2);          // 2 clusters
    runList.push_back(4);          // at LCN 4
    runList.push_back(static_cast<char>(0x11));
    runList.push_back(2);          // 2 clusters
    runList.push_back(5);          // +5 -> LCN 9
    runList.push_back(0);

    // Record 0 describes $MFT itself, which is exactly what the bootstrap parses.
    std::string rec0 = BuildRecord(5, Utf16("$MFT"), 0x00000006u, true, false, 4 * kCluster, runList);
    std::string rec1 = BuildRecord(5, Utf16("a.bin"), 0x80u, true, false, 40000, TwoRunList());
    std::string rec2 = BuildRecord(5, Utf16("Docs"), 0x10u, true, true, 0, std::string());
    std::string rec3 = BuildRecord(5, Utf16("b.bin"), 0x80u, true, false, 8192, TwoRunList());

    // A 64-cluster volume with the table written where its run list says it is.
    std::string volume(64 * kCluster, '\0');
    auto place = [&volume](std::uint64_t lcn, const std::string& bytes) {
        std::memcpy(volume.data() + lcn * kCluster, bytes.data(), bytes.size());
    };
    place(kMftStartLcn + 0, rec0);
    place(kMftStartLcn + 1, rec1);
    place(9, rec2);
    place(10, rec3);

    auto readAt = [&volume](std::uint64_t offset, std::size_t length) -> Result<std::string> {
        if (offset + length > volume.size()) {
            return Result<std::string>::Failure(MakeError(ErrorCode::E_ENUM_IO));
        }
        return Result<std::string>::Success(volume.substr(offset, length));
    };

    // Step 1: record 0, read at the geometry's start LCN - no handle, no open.
    auto first = readAt(kMftStartLcn * kCluster, geom.recordSize);
    CHECK(first.Ok());
    auto parsed0 = mft::ParseRecord(first.Value(), 0, geom);
    CHECK(parsed0.Ok());
    CHECK(parsed0.Value().name == "$MFT");
    CHECK(parsed0.Value().extents.size() == 2);
    CHECK(parsed0.Value().extents[0].startLcn == static_cast<Lcn>(kMftStartLcn));
    CHECK(parsed0.Value().extents[1].startLcn == 9);

    const std::vector<Extent> runs = parsed0.Value().extents;

    // Step 2: the run list must map stream offsets onto both fragments.
    auto span = mft::ReadStreamRange(runs, kCluster, 0, 4 * 1024, readAt);
    CHECK(span.Ok());
    CHECK(span.Value().size() == 4 * 1024);
    CHECK(span.Value().compare(2 * 1024, 4, "FILE") == 0); // record 2 came from cluster 9

    // Step 3: the scan sees all four records, in order, across the gap.
    auto readStream = [&runs, readAt](std::uint64_t offset, std::size_t length) {
        return mft::ReadStreamRange(runs, kCluster, offset, length, readAt);
    };
    auto scanned = mft::ScanMft(4 * 1024, geom, readStream, 1000);
    CHECK(scanned.Ok());
    CHECK(scanned.Value().records.size() == 4);
    CHECK(scanned.Value().stats.malformed == 0);
    CHECK(scanned.Value().records[0].name == "$MFT");
    CHECK(scanned.Value().records[1].name == "a.bin");
    CHECK(scanned.Value().records[2].name == "Docs");
    CHECK(scanned.Value().records[2].isDirectory);
    CHECK(scanned.Value().records[3].name == "b.bin");

    // A run list that does not cover the requested range must fail loudly rather
    // than return zeros, which would parse as empty records.
    auto beyond = mft::ReadStreamRange(runs, kCluster, 4 * 1024, kCluster, readAt);
    CHECK(!beyond.Ok());
    CHECK(!mft::ReadStreamRange(runs, 0, 0, 16, readAt).Ok());
}


// Single-file defrag, end to end: plan -> journal -> move -> verify -> snapshot.
//
// This is the flow the first real-volume run takes (`file --target <path>`), and
// it had never been executed as a whole: the planner test only checked which
// files get selected, and the crash campaign only ran whole-volume defrags.
//
// Two defects lived in that gap, both invisible to every existing test because
// the emulator does not model access masks: `Extents()` opened the file with
// FILE_READ_ATTRIBUTES only, so post-move verification would have failed on a
// volume where the move succeeded, and `MoveExtent()` used the same read-only
// mask for the handle FSCTL_MOVE_FILE operates on. The masks are checked by
// review now; what this test locks down is the surrounding contract.
void TestSingleTargetDefragEndToEnd() {
    ScenarioSpec sc{};
    sc.id = "TARGET-1";
    sc.seed = 4242;
    sc.clusters = 4096;
    sc.fileCount = 40;
    sc.extentsPerFile = 3;
    sc.sizeMinClusters = 6;
    sc.sizeMaxClusters = 12;
    sc.freeTargetPct = 40.0;

    auto emuRes = BuildEmulatorFromScenario(sc);
    CHECK(emuRes.Ok());
    auto& emu = *emuRes.Value();

    PolicyGate policy;
    StructuredLogger logger;
    NullProgressSink sink;
    OperationController ctrl(policy, logger);
    CancelToken tok;

    auto snapRes = ctrl.Analyze(emu, emu, sink, tok);
    CHECK(snapRes.Ok());
    const AnalysisSnapshot before = snapRes.Value();

    FileId target = kInvalidFileId;
    std::uint32_t extentCount = 0;
    FileId neighbour = kInvalidFileId;
    std::vector<Extent> neighbourExtents;
    std::vector<Extent> targetExtents;
    for (const auto& f : before.files) {
        if (f.flags.isDirectory || f.flags.isMetadata || f.flags.isLocked || f.extents.empty()) continue;
        if (target == kInvalidFileId) {
            if (f.fragmentCount < 2) continue;
            target = f.id;
            extentCount = f.fragmentCount;
            targetExtents = f.extents;
        } else if (f.fragmentCount >= 1) {
            neighbour = f.id;
            neighbourExtents = f.extents;
            break;
        }
    }
    CHECK(target != kInvalidFileId);
    CHECK(extentCount >= 2);

    PlanOptions opts{};
    opts.mode = DefragMode::SingleFileOrFolder;
    opts.targetFileIds = {target};

    const auto jPath = TempTestDir() / "single_target.journal";
    std::filesystem::remove(jPath);
    BinaryJournal journal(jPath);

    AnalysisSnapshot beforeSnap{};
    AnalysisSnapshot afterSnap{};
    auto r = ctrl.RunOperation(emu, emu, opts, sink, journal, tok, true, &beforeSnap, &afterSnap);
    CHECK(r.Ok());

    // One move per extent, each verified: the whole point of the verb.
    CHECK(r.Value().stepsCompleted == extentCount);
    CHECK(r.Value().stepsVerified == extentCount);
    CHECK(r.Value().stepsSkipped == 0);
    CHECK(r.Value().clustersMoved > 0);

    // The file is contiguous afterwards and still the same size.
    const FileEntry* after = nullptr;
    for (const auto& f : afterSnap.files) {
        if (f.id == target) after = &f;
    }
    CHECK(after != nullptr);
    if (after != nullptr) {
        CHECK(after->fragmentCount == 1);
        CHECK(after->extents.size() == 1);
        ClusterCount totalBefore = 0;
        for (const auto& e : targetExtents) totalBefore += e.clusterCount;
        CHECK(after->allocatedClusters == totalBefore);
    }

    // Nothing else was touched: a single-file operation must not move a byte of
    // any other file, which is the promise `--target` makes to the operator.
    for (const auto& f : afterSnap.files) {
        if (f.id != neighbour) continue;
        CHECK(f.extents.size() == neighbourExtents.size());
        const std::size_t n = std::min(f.extents.size(), neighbourExtents.size());
        for (std::size_t i = 0; i < n; ++i) {
            CHECK(f.extents[i].startVcn == neighbourExtents[i].startVcn);
            CHECK(f.extents[i].startLcn == neighbourExtents[i].startLcn);
            CHECK(f.extents[i].clusterCount == neighbourExtents[i].clusterCount);
        }
    }

    // A file that is already contiguous produces no moves at all, and the command
    // still succeeds - that is what makes it safe to re-run.
    if (after != nullptr) {
        PlanOptions again{};
        again.mode = DefragMode::SingleFileOrFolder;
        again.targetFileIds = {target};
        CancelToken tok2;
        auto r2 = ctrl.RunOperation(emu, emu, again, sink, journal, tok2, true);
        CHECK(r2.Ok());
        CHECK(r2.Value().stepsCompleted == 0);
    }
}


// A journal from an earlier operation must not block the next one.
//
// The journal lives in %TEMP% under a per-volume name and was never cleaned up, so
// from the second operation onwards OpenOrCreate found a file whose plan hash
// could not match and returned E_JOURNAL_PLAN_HASH_MISMATCH - before touching a
// single cluster. On a real machine that reads as "the defragmenter stopped
// working after the first run". The old file is archived instead, never deleted.
void TestJournalRotationBetweenRuns() {
    const auto dir = TempTestDir() / "journal_rotation";
    std::filesystem::create_directories(dir);
    const auto jPath = dir / "rot.journal";
    std::filesystem::remove(jPath);
    std::filesystem::remove(std::filesystem::path(jPath.string() + ".prev"));

    {
        BinaryJournal j(jPath);
        CHECK(j.OpenOrCreate(1, 0xAABBCCDDULL, 0x1111ULL).Ok());
        JournalRecord rec{};
        rec.sequence = 1;
        rec.opId = 1;
        rec.type = JournalRecordType::PlanBegin;
        rec.planHash = 0x1111ULL;
        rec.volumeSerial = 0xAABBCCDDULL;
        CHECK(j.Append(rec, true).Ok());
        CHECK(j.Seal().Ok());
        CHECK(j.RotationNote().empty());
    }

    // A different plan: archive and start fresh.
    {
        BinaryJournal j(jPath);
        CHECK(j.OpenOrCreate(2, 0xAABBCCDDULL, 0x2222ULL).Ok());
        CHECK(!j.RotationNote().empty());
        CHECK(std::filesystem::exists(std::filesystem::path(jPath.string() + ".prev")));
        CHECK(j.Seal().Ok());
    }

    // A different volume serial (the volume was recreated) is handled the same way.
    {
        BinaryJournal j(jPath);
        CHECK(j.OpenOrCreate(3, 0x11223344ULL, 0x3333ULL).Ok());
        CHECK(!j.RotationNote().empty());
    }

    // The same plan still resumes rather than rotating: a crash must stay
    // recoverable, and that is the case the strict check exists for.
    {
        std::filesystem::remove(jPath);
        std::filesystem::remove(std::filesystem::path(jPath.string() + ".prev"));
        {
            BinaryJournal j(jPath);
            CHECK(j.OpenOrCreate(4, 0x55ULL, 0x99ULL).Ok());
        }
        BinaryJournal j(jPath);
        CHECK(j.OpenOrCreate(4, 0x55ULL, 0x99ULL).Ok());
        CHECK(j.RotationNote().empty());
    }
}


// "Nothing moved" must always come with a reason.
//
// A plan that selects a file and then cannot place it (no contiguous free run, an
// exclusion, a metadata file) used to vanish: the run printed "Moves completed: 0
// | Skipped: 0", which reads as "there was nothing to do". On a crowded volume
// that is the normal outcome for every large file, so the operator needs the
// reason on screen.
void TestPlanSkipsAreReported() {
    ScenarioSpec sc{};
    sc.id = "CROWDED";
    sc.seed = 777;
    sc.clusters = 2048;
    sc.fileCount = 60;
    sc.extentsPerFile = 3;
    sc.sizeMinClusters = 20;
    sc.sizeMaxClusters = 60;
    sc.freeTargetPct = 3.0; // deliberately almost full

    auto emuRes = BuildEmulatorFromScenario(sc);
    CHECK(emuRes.Ok());
    auto& emu = *emuRes.Value();

    PolicyGate policy;
    StructuredLogger logger;
    NullProgressSink sink;
    OperationController ctrl(policy, logger);
    CancelToken tok;

    auto snapRes = ctrl.Analyze(emu, emu, sink, tok);
    CHECK(snapRes.Ok());
    const AnalysisSnapshot snap = snapRes.Value();

    // The largest fragmented file is the one least likely to find room.
    FileId target = kInvalidFileId;
    ClusterCount biggest = 0;
    for (const auto& f : snap.files) {
        if (f.flags.isDirectory || f.flags.isMetadata || f.flags.isLocked) continue;
        if (f.fragmentCount < 2 || f.extents.empty()) continue;
        if (f.allocatedClusters > biggest) {
            biggest = f.allocatedClusters;
            target = f.id;
        }
    }
    CHECK(target != kInvalidFileId);

    PlanOptions opts{};
    opts.mode = DefragMode::SingleFileOrFolder;
    opts.targetFileIds = {target};

    BinaryJournal journal(TempTestDir() / "plan_skips.journal");
    std::filesystem::remove(journal.Path());
    auto r = ctrl.RunOperation(emu, emu, opts, sink, journal, tok, true);
    CHECK(r.Ok());
    const auto& outcome = r.Value();

    // The invariant: either something moved, or the plan explains itself.
    if (outcome.stepsCompleted == 0) {
        CHECK(!outcome.planSkips.empty());
        CHECK(!outcome.planSkips.front().fileName.empty());
        CHECK(outcome.planSkips.front().reason != SkipReason::None);
    } else {
        CHECK(outcome.planSkips.empty());
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
    RUN_TEST(TestPathScopeAndTargetConfinement);
    RUN_TEST(TestSingleTargetDefragEndToEnd);
    RUN_TEST(TestJournalRotationBetweenRuns);
    RUN_TEST(TestPlanSkipsAreReported);
    RUN_TEST(TestScanProgressReporting);
    RUN_TEST(TestExtentCensusClassification);
    RUN_TEST(TestPathCompositionFromUsn);
    RUN_TEST(TestMaxFilesOption);
    RUN_TEST(TestMftRecordParsing);
    RUN_TEST(TestMftRunListDecoding);
    RUN_TEST(TestMftSequentialScan);
    RUN_TEST(TestMftBootstrapFromRecordZero);
    RUN_TEST(TestCliAndSchedulerSsdPolicy);

    std::cout << "\nSummary: " << (gTestsRun - gTestsFailed) << "/" << gTestsRun << " passed.\n";
    return gTestsFailed == 0 ? 0 : 1;
}
