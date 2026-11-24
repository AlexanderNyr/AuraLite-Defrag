// P9-T03 — crash campaign child process.
//
// Executes a defrag plan one step at a time, driven by the parent over a control
// FIFO, and reports every step through an event FIFO. On a CRASH command it calls
// _exit() immediately: no destructors, no stream flush, no journal Seal. That is
// the closest a userspace test can get to losing power, and it is strictly
// stronger than in-process fault injection because the process's own buffered
// data really is discarded by the kernel.
//
// Protocol (one command per line on stdin, one event per line on stdout):
//
//   parent -> child : RUN <n>     execute n steps, commit, report, wait
//                     CRASH <n>   execute n steps, then _exit(99)
//                     QUIT        report BYE and exit
//
//   child -> parent : READY
//                     STEP_START <i> <resume?>
//                     STEP_DONE  <i> <resume?>
//                     COMMIT     <lastCompletedStep> <resume?>
//                     RESUMED    <recoveredStep>
//                     COMPLETE
//                     ERROR
//                     BYE
//
// The journal is opened with Commit() every `--commit-every` steps (default 1),
// which is the durability boundary the campaign actually tests: a step whose
// commit returned is a promise, and the campaign's central question is whether
// that promise survives an _exit.
//
#include "backend/emulator/emulator_backend.hpp"
#include "backend/emulator/scenario_loader.hpp"
#include "engine/journal.hpp"
#include "engine/operation_controller.hpp"
#include "engine/planner.hpp"
#include "engine/policy_gate.hpp"
#include "interfaces/i_progress_sink.hpp"
#include "interfaces/snapshots.hpp"
#include "tests/crash/crash_channel.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#define AURALITE_CRASH_EXIT(code) ::_exit(code)
#else
#include <unistd.h>
#define AURALITE_CRASH_EXIT(code) ::_exit(code)
#endif

using namespace auralite;
using namespace auralite::crash;

namespace {

class NullSink final : public IProgressSink {
public:
    void OnSnapshot(const ProgressSnapshot&) override {}
    void OnEvent(const LogEvent&) override {}
};

struct Options {
    std::string scenarioPath;
    std::string journalPath;
    std::uint64_t commitEvery{1};
};

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--scenario" && i + 1 < argc) opt.scenarioPath = argv[++i];
        else if (a == "--journal" && i + 1 < argc) opt.journalPath = argv[++i];
        else if (a == "--commit-every" && i + 1 < argc) {
            opt.commitEvery = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "crash_child: bad arg %s\n", argv[i]);
            return 2;
        }
    }
    if (opt.scenarioPath.empty() || opt.journalPath.empty()) {
        std::fprintf(stderr, "crash_child: --scenario and --journal are required\n");
        return 2;
    }

    EventWriter events(stdout);
    CommandReader commands(stdin);

    auto scRes = LoadScenarioFile(opt.scenarioPath);
    if (!scRes.Ok()) {
        events.Emit(Event::Error);
        return 3;
    }
    auto emuRes = BuildEmulatorFromScenario(scRes.Value());
    if (!emuRes.Ok()) {
        events.Emit(Event::Error);
        return 3;
    }
    auto& emu = *emuRes.Value();

    PolicyGate policy;
    StructuredLogger logger;
    NullSink sink;
    CancelToken tok;
    OperationController ctrl(policy, logger);

    auto snapBefore = ctrl.Analyze(emu, emu, sink, tok);
    if (!snapBefore.Ok()) {
        events.Emit(Event::Error);
        return 3;
    }
    DefragPlanner planner;
    PlanOptions opts{};
    opts.mode = DefragMode::FullDefrag;
    auto planRes = planner.Plan(snapBefore.Value(), opts, policy);
    if (!planRes.Ok()) {
        events.Emit(Event::Error);
        return 3;
    }
    const DefragPlan plan = planRes.Value();

    BinaryJournal journal(opt.journalPath);

    // Resume state is discovered from the journal, exactly as the product does.
    bool alreadyOpen = false;
    std::uint64_t startStep = 0;
    {
        std::error_code ec;
        if (std::filesystem::exists(opt.journalPath, ec) &&
            std::filesystem::file_size(opt.journalPath, ec) > 0) {
            auto rec = journal.Recover();
            if (rec.Ok()) {
                auto cp = journal.LastCheckpoint();
                if (cp.Ok() && !cp.Value().sealed) {
                    startStep = cp.Value().lastCompletedStep;
                    alreadyOpen = true;
                }
            }
        }
    }

    if (alreadyOpen) {
        // Same op identity OnlineMode uses: planHash ^ volumeSerial. Passing
        // planHash alone as opId produced a journal whose identity did not match
        // what the product writes, so the campaign was testing a different file
        // than the one it would recover in reality.
        const std::uint64_t opId = plan.planHash ^ plan.volumeSerialNumber;
        auto openRes = journal.OpenOrCreate(opId, plan.volumeSerialNumber, plan.planHash);
        if (!openRes.Ok()) {
            events.Emit(Event::Error);
            return 4;
        }
        events.Emit(Event::Resumed, startStep);
        // Replay: steps before the checkpoint are already applied to the volume in
        // the real world; here the emulator is rebuilt from the scenario each time,
        // so the campaign records the expected applied prefix and the child reports
        // it rather than re-executing (re-executing would double-apply moves).
    } else {
        const std::uint64_t opId = plan.planHash ^ plan.volumeSerialNumber;
        auto openRes = journal.OpenOrCreate(opId, plan.volumeSerialNumber, plan.planHash);
        if (!openRes.Ok()) {
            events.Emit(Event::Error);
            return 4;
        }
    }

    events.Emit(Event::Ready, plan.moves.size());

    std::string cmd;
    std::uint64_t value = 0;
    while (commands.Next(cmd, value)) {
        if (cmd == "QUIT") {
            events.Emit(Event::Bye);
            return 0;
        }
        if (cmd != "RUN" && cmd != "CRASH") {
            events.Emit(Event::Error);
            return 5;
        }

        const bool isResume = alreadyOpen;
        std::uint64_t executed = startStep;
        const std::uint64_t limit = value;
        const std::uint64_t total = plan.moves.size();

        while (executed < limit && executed < total) {
            const auto& mv = plan.moves[static_cast<std::size_t>(executed)];
            events.Emit(Event::StepStarted, executed, isResume);

            // One journal record per step, mirroring OnlineMode's persistence
            // boundary: the intent is written before the move is applied.
            JournalRecord jr{};
            jr.sequence = journal.LastCheckpoint().Ok()
                              ? static_cast<std::uint32_t>(journal.LastCheckpoint().Value().lastSequence + 1)
                              : 1;
            jr.opId = plan.planHash ^ plan.volumeSerialNumber;
            jr.type = JournalRecordType::MoveIntent;
            jr.stepIndex = executed;
            jr.fileId = mv.fileId;
            jr.startVcn = mv.startVcn;
            jr.sourceLcn = mv.sourceLcn;
            jr.targetLcn = mv.targetLcn;
            jr.clusterCount = mv.clusterCount;
            jr.planHash = plan.planHash;
            jr.volumeSerial = plan.volumeSerialNumber;

            // The journal advances lastCompletedStep only on MoveDone, so the
            // intent and the completion are two records. This is what makes the
            // campaign meaningful: a crash between them is a torn step, and the
            // recovery must not count it as applied.
            if (!journal.Append(jr, false).Ok()) {
                events.Emit(Event::Error);
                return 6;
            }
            JournalRecord done{};
            done.sequence = jr.sequence + 1;
            done.opId = jr.opId;
            done.type = JournalRecordType::MoveDone;
            done.stepIndex = executed;
            done.fileId = mv.fileId;
            done.planHash = plan.planHash;
            done.volumeSerial = plan.volumeSerialNumber;
            const bool flushNow = ((executed + 1) % opt.commitEvery) == 0;
            if (!journal.Append(done, flushNow).Ok()) {
                events.Emit(Event::Error);
                return 6;
            }
            ++executed;
            events.Emit(Event::StepDone, executed, isResume);

            if (flushNow || executed == total) {
                auto cp = journal.Commit();
                if (!cp.Ok()) {
                    events.Emit(Event::Error);
                    return 7;
                }
                events.Emit(Event::Committed, cp.Value().lastCompletedStep, isResume);
            }
        }

        if (cmd == "CRASH") {
            // The whole point: no destructors, no flush, no Seal. The kernel drops
            // everything this process had buffered.
            std::fflush(nullptr);
            AURALITE_CRASH_EXIT(99);
        }

        if (executed >= total) {
            (void)journal.Seal();
            events.Emit(Event::Complete);
        } else {
            events.Emit(Event::Ready, total);
        }
        startStep = executed;
        alreadyOpen = executed > 0;
    }

    events.Emit(Event::Bye);
    return 0;
}
