// Golden-trace generator / verifier (P2-T06, R-064).
//
//   auralite-golden --write            regenerate tests/golden/*.trace.json
//   auralite-golden --verify           compare current behaviour to the corpus
//   auralite-golden --write --out DIR  write to a different directory
//
// The corpus is the versioned regression baseline: a change in observable
// engine behaviour shows up as a trace diff, not as a vague "tests got worse".
#include "backend/emulator/scenario_loader.hpp"
#include "core/error_catalog.hpp"
#include "differential/differential_oracle.hpp"
#include "engine/journal.hpp"
#include "engine/operation_controller.hpp"
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

class NullSink final : public IProgressSink {
public:
    void OnSnapshot(const ProgressSnapshot&) override {}
    void OnEvent(const LogEvent&) override {}
};

struct CaseResult {
    std::string scenarioId;
    bool ok{false};
    std::string error;
    NormalizedRun run;
};

// Defrag mode per scenario, mirroring the engine's intended coverage.
DefragMode ModeForScenario(int index) {
    switch (index) {
        case 2: return DefragMode::DefragFreeSpace;
        case 3: return DefragMode::MftOptimize;
        case 4: return DefragMode::MftZoneEvacuate;
        case 6: return DefragMode::MoveLargeToEnd;
        default: return DefragMode::FullDefrag;
    }
}

CaseResult RunCase(const std::filesystem::path& scenarioPath, int index,
                   const std::filesystem::path& workDir) {
    CaseResult out;
    auto scRes = LoadScenarioFile(scenarioPath);
    if (!scRes.Ok()) {
        out.error = "scenario load failed";
        return out;
    }
    const auto& spec = scRes.Value();
    out.scenarioId = spec.id;

    auto emuRes = BuildEmulatorFromScenario(spec);
    if (!emuRes.Ok()) {
        out.error = "emulator build failed";
        return out;
    }
    auto& emu = *emuRes.Value();

    PolicyGate policy;
    StructuredLogger logger;
    OperationController ctrl(policy, logger);
    NullSink sink;
    CancelToken token;

    const auto journalPath = workDir / (spec.id + ".journal");
    std::filesystem::remove(journalPath);
    BinaryJournal journal(journalPath);

    PlanOptions opts;
    opts.mode = ModeForScenario(index);
    opts.mftTier = MftTier::Tier3ReportOnly;

    AnalysisSnapshot before{};
    AnalysisSnapshot after{};
    auto res = ctrl.RunOperation(emu, emu, opts, sink, journal, token, true, &before, &after);

    // A scenario that is *supposed* to be refused is still a valid golden case:
    // the trace records the refusal, which is exactly what we want to freeze.
    out.run = NormalizeFromSnapshot(before.summary.totalClusters ? before : after, "emulator");
    out.run.scenarioId = spec.id;
    if (res.Ok()) {
        AttachOutcome(out.run, res.Value());
        out.run.errorCodeValue = 0;
    } else {
        out.run.errorCodeValue = static_cast<std::uint32_t>(res.Error().code);
    }
    out.run.moveCallCount = emu.MoveCallCount();
    out.ok = true;
    return out;
}

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

int main(int argc, char** argv) {
    bool write = false;
    bool verify = false;
    std::filesystem::path outDir = "tests/golden";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--write") {
            write = true;
        } else if (arg == "--verify") {
            verify = true;
        } else if (arg == "--out" && i + 1 < argc) {
            outDir = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: auralite-golden [--write|--verify] [--out DIR]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }

    if (write == verify) {
        std::cerr << "specify exactly one of --write or --verify\n";
        return 2;
    }

    const auto workDir = std::filesystem::temp_directory_path() / "auralite_golden_work";
    std::filesystem::create_directories(workDir);

    std::vector<std::string> failures;
    int cases = 0;
    int updated = 0;
    int missing = 0;

    for (int i = 1; i <= 20; ++i) {
        char name[32]{};
        std::snprintf(name, sizeof(name), "SC-%02d.json", i);
        const auto path = std::filesystem::path("tests/scenarios") / name;
        if (!std::filesystem::exists(path)) continue;

        const auto result = RunCase(path, i, workDir);
        if (!result.ok) {
            failures.push_back(result.scenarioId + ": " + result.error);
            continue;
        }
        ++cases;

        const auto tracePath = outDir / golden::TraceFileName(result.scenarioId);
        const std::string serialized = golden::Serialize(result.run);

        if (write) {
            WriteFile(tracePath, serialized);
            ++updated;
            continue;
        }

        if (!std::filesystem::exists(tracePath)) {
            failures.push_back(result.scenarioId + ": golden trace missing (" +
                               tracePath.string() + ")");
            ++missing;
            continue;
        }

        auto parsed = golden::Parse(ReadFile(tracePath));
        if (!parsed.Ok()) {
            failures.push_back(result.scenarioId + ": golden trace unreadable");
            continue;
        }

        const auto rep = Compare(parsed.Value(), result.run);
        if (!rep.passed) {
            WriteFile(std::filesystem::path("build_linux") / "differential" /
                          (result.scenarioId + ".divergence.md"),
                      rep.ToMarkdown());
            failures.push_back(result.scenarioId + ": divergence (see build_linux/differential)");
        }
    }

    std::cout << "cases run: " << cases << "\n";
    if (write) {
        std::cout << "golden traces written: " << updated << " -> " << outDir.string() << "\n";
    } else {
        std::cout << "golden traces missing: " << missing << "\n";
    }

    if (!failures.empty()) {
        std::cout << "\nFAILURES (" << failures.size() << "):\n";
        for (const auto& f : failures) std::cout << "  " << f << "\n";
        return 1;
    }
    std::cout << (write ? "corpus regenerated cleanly.\n" : "corpus verified: no divergence.\n");
    return 0;
}
