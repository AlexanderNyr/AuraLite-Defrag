// P2-T06 — differential oracle and golden-trace tests (DT-01…DT-06).
//
// These tests run without a real NTFS volume: they exercise the oracle
// contract itself (normalization, tolerance, classification, golden
// round-trip). DT-05 (emulator ⇄ VHDX) lands with the VHDX harness (P3-T13);
// this file owns everything the harness will reuse.
#include "backend/emulator/emulator_backend.hpp"
#include "backend/emulator/scenario_loader.hpp"
#include "core/error_catalog.hpp"
#include "differential/differential_oracle.hpp"
#include "engine/journal.hpp"
#include "engine/operation_controller.hpp"
#include "engine/planner.hpp"
#include "engine/policy_gate.hpp"
#include "interfaces/i_progress_sink.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace auralite;
using namespace auralite::differential;

namespace {

int gRun = 0;
int gFailed = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "]: " #cond "\n"; \
            ++gFailed;                                                               \
            return;                                                                  \
        }                                                                            \
    } while (false)

#define RUN_TEST(fn)                                    \
    do {                                                \
        ++gRun;                                         \
        const int before = gFailed;                     \
        fn();                                           \
        std::cout << (gFailed == before ? "  [PASS] " : "  [FAIL] ") << #fn "\n"; \
    } while (false)

class NullSink final : public IProgressSink {
public:
    void OnSnapshot(const ProgressSnapshot&) override {}
    void OnEvent(const LogEvent&) override {}
};

std::filesystem::path TempDir() {
    auto p = std::filesystem::temp_directory_path() / "auralite_differential";
    std::filesystem::create_directories(p);
    return p;
}

// ---------------------------------------------------------------------------
// DT-01: normalization
// ---------------------------------------------------------------------------

void TestPathKeyNormalization() {
    CHECK(MakePathKey("C:\\Data\\A.TXT") == MakePathKey("c:/data/a.txt"));
    CHECK(MakePathKey("\\\\?\\C:\\Data\\File.bin") == MakePathKey("c:\\data\\file.bin"));
    CHECK(MakePathKey("C:\\A\\\\B") == MakePathKey("C:/A/B"));
    // Distinct files must not collapse.
    CHECK(MakePathKey("C:\\data\\a.txt") != MakePathKey("C:\\data\\b.txt"));
    // A leading separator must not create a phantom key.
    CHECK(MakePathKey("\\data\\a.txt") == MakePathKey("data/a.txt"));
    CHECK(MakePathKey("") == "");
}

void TestNormalizeFromSnapshotIsTotal() {
    ScenarioSpec spec;
    spec.clusters = 2048;
    spec.fileCount = 40;
    spec.extentsPerFile = 3;
    spec.seed = 7;

    auto emuRes = BuildEmulatorFromScenario(spec);
    CHECK(emuRes.Ok());
    auto& emu = *emuRes.Value();

    PolicyGate policy;
    StructuredLogger logger;
    OperationController ctrl(policy, logger);
    NullSink sink;
    CancelToken tok;

    auto snapRes = ctrl.Analyze(emu, emu, sink, tok);
    CHECK(snapRes.Ok());

    const auto run = NormalizeFromSnapshot(snapRes.Value(), "emulator");
    CHECK(!run.files.empty());
    // Sorted and unique keys: the oracle relies on both.
    for (std::size_t i = 1; i < run.files.size(); ++i) {
        CHECK(run.files[i - 1].key < run.files[i].key);
    }
    // Extents sorted by VCN.
    for (const auto& f : run.files) {
        for (std::size_t i = 1; i < f.extents.size(); ++i) {
            CHECK(f.extents[i - 1].startVcn <= f.extents[i].startVcn);
        }
    }
}

// ---------------------------------------------------------------------------
// DT-01 (b): identical runs must agree perfectly
// ---------------------------------------------------------------------------

void TestIdenticalRunsAgreePerfectly() {
    ScenarioSpec spec;
    spec.clusters = 4096;
    spec.fileCount = 60;
    spec.extentsPerFile = 4;
    spec.seed = 42;

    const auto build = [&]() {
        auto e = BuildEmulatorFromScenario(spec);
        return std::move(e).Value();
    };

    PolicyGate policy;
    StructuredLogger logger;

    const auto runOne = [&](const char* label) {
        auto emu = build();
        OperationController ctrl(policy, logger);
        NullSink sink;
        CancelToken tok;
        auto snap = ctrl.Analyze(*emu, *emu, sink, tok);
        return NormalizeFromSnapshot(snap.Value(), label);
    };

    const auto a = runOne("emulator-a");
    const auto b = runOne("emulator-b");

    const auto rep = Compare(a, b);
    CHECK(rep.passed);
    CHECK(rep.divergences.empty());
    CHECK(rep.overallAgreementPct >= 99.999);
}

// ---------------------------------------------------------------------------
// DT-01/DT-02: tolerance behaviour
// ---------------------------------------------------------------------------

void TestFragmentationToleranceIsHonoured() {
    NormalizedRun a;
    a.label = "a";
    NormalizedRun b;
    b.label = "b";
    a.summary.volumeFragmentationPct = 40.0;
    b.summary.volumeFragmentationPct = 41.5; // inside ±2 pp

    auto rep = Compare(a, b);
    for (const auto& s : rep.scores) {
        if (s.name == "volume-fragmentation-pct") CHECK(s.meetsGate);
    }

    b.summary.volumeFragmentationPct = 45.0; // outside
    rep = Compare(a, b);
    bool failed = false;
    for (const auto& s : rep.scores) {
        if (s.name == "volume-fragmentation-pct") failed = !s.meetsGate;
    }
    CHECK(failed);
    CHECK(rep.UnexplainedCount() >= 1);
}

void TestExtentCountTolerance() {
    NormalizedRun a;
    a.label = "a";
    a.files.push_back({"c:\\d\\f1", 100, 10, 10, {{0, 100, 5}, {5, 300, 5}}, {}});
    NormalizedRun b = a;
    b.label = "b";
    // Same file, 11 extents instead of 10 — within 10%.
    b.files[0].extents.push_back({10, 900, 1});

    auto rep = Compare(a, b);
    CHECK(rep.UnexplainedCount() == 0); // divergence recorded, but explained
    bool explained = false;
    for (const auto& d : rep.divergences) {
        if (d.metric == DivergenceMetric::ExtentMap) {
            explained = (d.kind == DivergenceClass::ExplainedAllocatorPolicy);
        }
    }
    CHECK(explained);
}

// ---------------------------------------------------------------------------
// DT-06: skip reasons must agree exactly
// ---------------------------------------------------------------------------

void TestSkipReasonDisagreementFailsGate() {
    NormalizedRun a;
    a.label = "a";
    a.skips = {{"c:\\d\\pagefile.sys", SkipReason::PageOrSystemFile},
               {"c:\\d\\locked.bin", SkipReason::LockedOrInUse}};
    NormalizedRun b = a;
    b.label = "b";
    b.skips[1].reason = SkipReason::AccessDenied; // same file, different reason

    const auto rep = Compare(a, b);
    bool skipGateFailed = false;
    for (const auto& s : rep.scores) {
        if (s.name == "skip-reason-agreement") skipGateFailed = !s.meetsGate;
    }
    CHECK(skipGateFailed);
    CHECK(!rep.passed);
}

void TestErrorCodeDivergenceIsClassified() {
    NormalizedRun a;
    a.label = "a";
    a.errorCodeValue = 0;
    NormalizedRun b = a;
    b.label = "b";
    b.errorCodeValue = static_cast<std::uint32_t>(ErrorCode::E_MOVE_INTERRUPTED);

    const auto rep = Compare(a, b);
    CHECK(rep.UnexplainedCount() == 0);
    bool seen = false;
    for (const auto& d : rep.divergences) {
        if (d.metric == DivergenceMetric::ErrorCode) {
            CHECK(d.kind == DivergenceClass::ExplainedErrorPath);
            seen = true;
        }
    }
    CHECK(seen);
}

// ---------------------------------------------------------------------------
// DT-04: plan comparison
// ---------------------------------------------------------------------------

void TestMovePlanComparison() {
    DefragPlan plan;
    plan.moves = {{0, 1, "c:\\d\\a.bin", 0, 100, 500, 4, false, 0},
                  {1, 2, "c:\\d\\b.bin", 0, 200, 600, 4, false, 0}};

    NormalizedRun left;
    left.label = "left";
    AttachPlan(left, plan);

    NormalizedRun right = left;
    right.label = "right";

    auto rep = Compare(left, right);
    for (const auto& s : rep.scores) {
        if (s.name == "move-plan-agreement") CHECK(s.meetsGate);
    }

    // Different target LCN: allocator policy, not a failure.
    right.moves[0].targetLcn = 999;
    rep = Compare(left, right);
    for (const auto& s : rep.scores) {
        if (s.name == "move-plan-agreement") CHECK(!s.meetsGate);
    }
    CHECK(rep.UnexplainedCount() == 0);
}

// ---------------------------------------------------------------------------
// Golden traces: determinism and round-trip
// ---------------------------------------------------------------------------

void TestGoldenTraceRoundTrip() {
    NormalizedRun run;
    run.scenarioId = "SC-01";
    run.label = "emulator";
    run.summary.totalClusters = 16384;
    run.summary.freeClusters = 12454;
    run.summary.totalFiles = 162;
    run.summary.fragmentedFiles = 160;
    run.summary.volumeFragmentationPct = 86.8741;
    run.summary.fileFragmentationPct = 86.87;
    run.mft.fragmentCount = 1;
    run.mft.zoneStartLcn = 16;
    run.mft.zoneEndLcn = 2064;
    run.mft.totalZoneClusters = 2048;
    run.mft.freeZoneClusters = 1536;
    run.files.push_back({"c:\\data\\a.bin", 4096, 12, 3, {{0, 100, 4}, {4, 500, 4}, {8, 900, 4}}, {}});
    run.moves.push_back({"c:\\data\\a.bin", 0, 100, 500, 4});
    run.skips.push_back({"c:\\pagefile.sys", SkipReason::PageOrSystemFile});
    run.moveCallCount = 1;

    const std::string t1 = golden::Serialize(run);
    const std::string t2 = golden::Serialize(run);
    CHECK(t1 == t2); // byte-stable

    auto parsed = golden::Parse(t1);
    CHECK(parsed.Ok());
    const auto& back = parsed.Value();
    CHECK(back.scenarioId == run.scenarioId);
    CHECK(back.summary.totalClusters == run.summary.totalClusters);
    CHECK(back.files.size() == 1);
    CHECK(back.files[0].key == "c:\\data\\a.bin");
    CHECK(back.files[0].extents.size() == 3);
    CHECK(back.moves.size() == 1);
    CHECK(back.skips.size() == 1);
    CHECK(back.skips[0].reason == SkipReason::PageOrSystemFile);

    // Round-trip is itself byte-stable.
    CHECK(golden::Serialize(back) == t1);
}

void TestGoldenTraceRejectsUnknownVersion() {
    auto bad = golden::Parse(R"({"traceFormatVersion": 999, "scenarioId": "X"})");
    CHECK(!bad.Ok());

    auto notObject = golden::Parse("[]");
    CHECK(!notObject.Ok());

    auto garbage = golden::Parse("{not json");
    CHECK(!garbage.Ok());
}

void TestTraceFileNameIsSafe() {
    CHECK(golden::TraceFileName("SC-01") == "SC-01.trace.json");
    CHECK(golden::TraceFileName("a/b:c") == "a_b_c.trace.json");
    CHECK(golden::TraceFileName("") == "unnamed.trace.json");
}

// ---------------------------------------------------------------------------
// Report rendering
// ---------------------------------------------------------------------------

void TestReportSerialization() {
    NormalizedRun a;
    a.label = "emulator";
    a.scenarioId = "SC-01";
    NormalizedRun b = a;
    b.label = "vhdx";
    b.summary.volumeFragmentationPct = 90.0; // divergence

    const auto rep = Compare(a, b);
    const std::string md = rep.ToMarkdown();
    const std::string js = rep.ToJson();
    CHECK(md.find("Differential report") != std::string::npos);
    CHECK(md.find("SC-01") != std::string::npos);
    CHECK(js.find("\"passed\"") != std::string::npos);
    CHECK(js.find("\"unexplained\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// DT-05 harness stand-in: a full analyse→defrag→compare cycle on the emulator
// ---------------------------------------------------------------------------

void TestEndToEndRunProducesComparableTraces() {
    auto scRes = LoadScenarioFile(std::filesystem::path("tests/scenarios/SC-01.json"));
    CHECK(scRes.Ok());

    PolicyGate policy;
    StructuredLogger logger;

    // CHECK() returns void, so the callback produces the trace through an
    // out-parameter instead of a lambda return value.
    NormalizedRun left, right;
    const auto execute = [&](const char* label, const std::string& journalName,
                             NormalizedRun* out) -> void {
        auto emu = BuildEmulatorFromScenario(scRes.Value());
        CHECK(emu.Ok());
        auto& e = *emu.Value();

        OperationController ctrl(policy, logger);
        NullSink sink;
        CancelToken tok;
        auto jPath = TempDir() / journalName;
        std::filesystem::remove(jPath);
        BinaryJournal journal(jPath);

        PlanOptions opts;
        opts.mode = DefragMode::FullDefrag;
        AnalysisSnapshot before{}, after{};
        auto r = ctrl.RunOperation(e, e, opts, sink, journal, tok, true, &before, &after);
        CHECK(r.Ok());

        *out = NormalizeFromSnapshot(before, label);
        out->scenarioId = "SC-01";
        AttachOutcome(*out, r.Value());
    };

    execute("emulator-a", "dt05-a.journal", &left);
    execute("emulator-b", "dt05-b.journal", &right);

    const auto rep = Compare(left, right);
    CHECK(rep.passed);
    CHECK(rep.overallAgreementPct >= 98.0);

    // And the traces survive a save/load cycle unchanged.
    const auto text = golden::Serialize(left);
    auto parsed = golden::Parse(text);
    CHECK(parsed.Ok());
    CHECK(golden::Serialize(parsed.Value()) == text);
}

} // namespace

int main() {
    std::cout << "Running AuraLite-Defrag differential oracle suite (P2-T06)...\n";

    RUN_TEST(TestPathKeyNormalization);
    RUN_TEST(TestNormalizeFromSnapshotIsTotal);
    RUN_TEST(TestIdenticalRunsAgreePerfectly);
    RUN_TEST(TestFragmentationToleranceIsHonoured);
    RUN_TEST(TestExtentCountTolerance);
    RUN_TEST(TestSkipReasonDisagreementFailsGate);
    RUN_TEST(TestErrorCodeDivergenceIsClassified);
    RUN_TEST(TestMovePlanComparison);
    RUN_TEST(TestGoldenTraceRoundTrip);
    RUN_TEST(TestGoldenTraceRejectsUnknownVersion);
    RUN_TEST(TestTraceFileNameIsSafe);
    RUN_TEST(TestReportSerialization);
    RUN_TEST(TestEndToEndRunProducesComparableTraces);

    std::cout << "\nSummary: " << (gRun - gFailed) << "/" << gRun << " passed.\n";
    return gFailed == 0 ? 0 : 1;
}
