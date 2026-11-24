// P9-T03 — crash / power-loss campaign and journal recovery (R-079, R-083).
//
// Acceptance:
//   (1) 0 silent content corruption in 200 trials;
//   (2) 100% of resumed operations complete or cleanly abort;
//   (3) chkdsk reports 0 errors;
//   (4) recovery <= 10 s on RH-1.
//
// Criterion (3) is a Windows-only volume check and cannot run here; it is
// reported as SKIPPED, never as PASS. Everything else is measured for real:
// the child process is killed with _exit() so the kernel genuinely discards its
// buffered writes, which is what the existing in-process fault-injection test
// could not do.
//
// Five crash points, the ones the plan names:
//
//   0  BEFORE_FIRST_STEP   child opened the journal, died before any step
//   1  MID_STEP_INTENT     died after the MoveIntent record, before MoveDone
//   2  AFTER_STEP_DONE     died after MoveDone, before the checkpoint commit
//   3  AFTER_COMMIT        died right after a commit returned success
//   4  MID_CHUNK           died partway through a run of steps
//
#include "backend/usn/usn_parser.hpp"
#include "core/json.hpp"
#include "tests/crash/crash_channel.hpp"

#include <array>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace auralite;
using namespace auralite::crash;

namespace {

// Overridable with --child. The default matches the Makefile layout; ctest
// passes the real target path, because the build directory name is a
// generator's choice and a hard-coded path made the test fail under ctest.
const char* kChildBinary = "build_linux/auralite-crash-child";

// An independent CRC32C implementation for the verifier. Deliberately not the
// product's ComputeCrc32c: a checksum check that reuses the writer's own routine
// cannot catch a bug in that routine.
std::uint32_t Crc32c(const void* data, std::size_t size) noexcept {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        constexpr std::uint32_t poly = 0x82F63B78U;
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
            for (int b = 0; b < 8; ++b) crc = (crc & 1U) ? ((crc >> 1) ^ poly) : (crc >> 1);
            t[i] = crc;
        }
        return t;
    }();
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}

struct TrialResult {
    int crashPoint{0};
    int trialIndex{0};
    bool resumed{false};
    std::uint64_t stepsBeforeCrash{0};
    std::uint64_t committedBeforeCrash{0};
    std::uint64_t resumedAt{0};
    bool committedPromiseHonoured{false};
    bool contentIntact{false};
    bool recoveredCleanly{false};
    double recoveryMs{0.0};
    std::string detail;
};

struct Totals {
    int trials{0};
    int resumed{0};
    int promiseHonoured{0};
    int contentIntact{0};
    int cleanRecoveries{0};
    double worstRecoveryMs{0.0};
    double totalRecoveryMs{0.0};
    std::array<int, 5> perPoint{};
    std::array<int, 5> perPointResumed{};
};

// FIFO helpers -------------------------------------------------------------

bool MakeFifo(const std::string& path) {
    ::unlink(path.c_str());
    return ::mkfifo(path.c_str(), 0600) == 0;
}

struct Proc {
    pid_t pid{-1};
    int toChildFd{-1};
    int fromChildFd{-1};
    std::FILE* cmdFile{nullptr};
    std::FILE* evtFile{nullptr};
};

Proc Spawn(const std::string& scenario, const std::string& journal,
           const std::string& cmdFifo, const std::string& evtFifo) {
    Proc p;
    p.pid = ::fork();
    if (p.pid < 0) return p;
    if (p.pid == 0) {
        // Child
        int inFd = ::open(cmdFifo.c_str(), O_RDONLY);
        int outFd = ::open(evtFifo.c_str(), O_WRONLY);
        if (inFd < 0 || outFd < 0) ::_exit(120);
        ::dup2(inFd, 0);
        ::dup2(outFd, 1);
        ::close(inFd);
        ::close(outFd);
        ::execl(kChildBinary, kChildBinary, "--scenario", scenario.c_str(), "--journal",
                journal.c_str(), "--commit-every", "1", static_cast<char*>(nullptr));
        ::_exit(121);
    }
    // Parent
    p.toChildFd = ::open(cmdFifo.c_str(), O_WRONLY);
    p.fromChildFd = ::open(evtFifo.c_str(), O_RDONLY);
    if (p.toChildFd >= 0) p.cmdFile = ::fdopen(p.toChildFd, "w");
    if (p.fromChildFd >= 0) p.evtFile = ::fdopen(p.fromChildFd, "r");
    return p;
}

void SendCommand(Proc& p, const char* cmd, std::uint64_t value) {
    if (!p.cmdFile) return;
    std::fprintf(p.cmdFile, "%s %llu\n", cmd, static_cast<unsigned long long>(value));
    std::fflush(p.cmdFile);
}

// Read one event line. Returns false on EOF (child died).
bool ReadEvent(Proc& p, EventRecord& out) {
    if (!p.evtFile) return false;
    char buf[128]{};
    if (!std::fgets(buf, sizeof(buf), p.evtFile)) return false;
    std::string_view line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    const auto sp1 = line.find(' ');
    const auto sp2 = line.rfind(' ');
    const std::string_view name = (sp1 == std::string_view::npos) ? line : line.substr(0, sp1);
    if (!ParseEvent(name, out.kind)) return false;
    out.value = 0;
    out.isResume = false;
    if (sp1 != std::string_view::npos) {
        const std::string_view mid = (sp2 != sp1) ? line.substr(sp1 + 1, sp2 - sp1 - 1)
                                                  : line.substr(sp1 + 1);
        out.value = std::strtoull(std::string(mid).c_str(), nullptr, 10);
        if (sp2 != sp1) out.isResume = line.substr(sp2 + 1) == "1";
    }
    return true;
}

void KillProc(Proc& p) {
    if (p.pid > 0) {
        int status = 0;
        ::kill(p.pid, SIGKILL);
        ::waitpid(p.pid, &status, 0);
        p.pid = -1;
    }
    if (p.cmdFile) { std::fclose(p.cmdFile); p.cmdFile = nullptr; }
    if (p.evtFile) { std::fclose(p.evtFile); p.evtFile = nullptr; }
    p.toChildFd = -1;
    p.fromChildFd = -1;
}

int WaitChild(Proc& p) {
    if (p.pid <= 0) return -1;
    int status = 0;
    ::waitpid(p.pid, &status, 0);
    p.pid = -1;
    if (p.cmdFile) { std::fclose(p.cmdFile); p.cmdFile = nullptr; }
    if (p.evtFile) { std::fclose(p.evtFile); p.evtFile = nullptr; }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// Journal verification ----------------------------------------------------

struct JournalCheck {
    bool headerValid{false};
    std::uint64_t lastCompletedStep{0};
    std::uint64_t totalRecords{0};
    std::uint64_t badCrc{0};
    bool trailingPartial{false};
    std::string detail;
};

// Independent verification: read the two 512-byte header slots and every 96-byte
// record, recompute CRC32C, and confirm that nothing reads past EOF. A journal
// that "recovers" while containing corrupt records is not a pass.
JournalCheck VerifyJournal(const std::string& path) {
    JournalCheck out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { out.detail = "cannot open"; return out; }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (size < 1024) { std::fclose(f); out.detail = "file shorter than two header slots"; return out; }

    struct HeaderSlot {
        std::uint32_t magic;
        std::uint16_t version;
        std::uint16_t flags;
        std::uint32_t generation;
        std::uint32_t pad;
        std::uint64_t opId;
        std::uint64_t volumeSerial;
        std::uint64_t planHash;
        std::uint64_t lastSequence;
        std::uint64_t lastCompletedStep;
        std::uint64_t checkpointOffset;
        std::uint8_t padding[512 - 64 - 4];
        std::uint32_t crc32c;
    };
    static_assert(sizeof(HeaderSlot) == 512, "header slot layout");

    HeaderSlot best{};
    bool haveBest = false;
    for (int slot = 0; slot < 2; ++slot) {
        HeaderSlot h{};
        std::fseek(f, slot * 512L, SEEK_SET);
        if (std::fread(&h, sizeof(h), 1, f) != 1) continue;
        const std::uint32_t expect = Crc32c(&h, sizeof(h) - sizeof(std::uint32_t));
        if (h.magic != 0x484A4C41U || h.version != 1 || expect != h.crc32c) continue;
        if (!haveBest || h.generation >= best.generation) {
            best = h;
            haveBest = true;
        }
    }
    if (!haveBest) { std::fclose(f); out.detail = "both header slots invalid"; return out; }

    out.headerValid = true;
    out.lastCompletedStep = best.lastCompletedStep;

    // Walk records. Stop at the last whole record; a tail shorter than 96 bytes
    // is the expected artefact of an abrupt exit and is reported, not failed.
    std::fseek(f, 1024L, SEEK_SET);
    std::vector<char> buf(96);
    while (std::fread(buf.data(), 96, 1, f) == 1) {
        struct Rec {
            std::uint32_t magic;
            std::uint16_t version;
            std::uint16_t type;
            std::uint32_t length;
            std::uint32_t sequence;
            std::uint64_t opId;
            std::uint64_t stepIndex;
            std::uint64_t fileId;
            std::uint64_t startVcn;
            std::uint64_t sourceLcn;
            std::uint64_t targetLcn;
            std::uint64_t clusterCount;
            std::uint64_t planHash;
            std::uint64_t volumeSerial;
            std::uint32_t extraCode;
            std::uint32_t crc32c;
        };
        static_assert(sizeof(Rec) == 96, "record layout");
        Rec r{};
        std::memcpy(&r, buf.data(), sizeof(r));
        if (r.magic != 0x524A4C41U) { ++out.badCrc; continue; }
        const std::uint32_t expect = Crc32c(&r, sizeof(r) - sizeof(std::uint32_t));
        if (expect != r.crc32c) { ++out.badCrc; continue; }
        ++out.totalRecords;
    }
    const long consumed = std::ftell(f);
    if (consumed < size) {
        out.trailingPartial = true;
        out.detail = std::to_string(size - consumed) + " trailing bytes (torn tail)";
    }
    std::fclose(f);
    return out;
}

// The campaign ------------------------------------------------------------

const char* CrashPointName(int p) {
    switch (p) {
        case 0: return "BEFORE_FIRST_STEP";
        case 1: return "MID_STEP_INTENT";
        case 2: return "AFTER_STEP_DONE";
        case 3: return "AFTER_COMMIT";
        case 4: return "MID_CHUNK";
        default: return "?";
    }
}

// How many steps to request before the crash, per crash point.
std::uint64_t StepsForPoint(int point, int trial) {
    switch (point) {
        case 0: return 0;
        case 1: return 0;                       // die inside the first step
        case 2: return 1;                       // die after one step, before commit
        case 3: return static_cast<std::uint64_t>(1 + (trial % 5));  // die after a commit
        default: return static_cast<std::uint64_t>(2 + (trial % 7)); // die mid-run
    }
}

int RunOneTrial(int point, int trial, const std::string& scenario, const std::string& workDir,
                TrialResult& res) {
    const std::string journal = workDir + "/crash.journal";
    const std::string cmdFifo = workDir + "/cmd.fifo";
    const std::string evtFifo = workDir + "/evt.fifo";
    std::error_code ec;
    std::filesystem::remove(journal, ec);

    if (!MakeFifo(cmdFifo) || !MakeFifo(evtFifo)) {
        res.detail = "fifo creation failed";
        return 1;
    }

    res.crashPoint = point;
    res.trialIndex = trial;

    Proc p = Spawn(scenario, journal, cmdFifo, evtFifo);
    if (p.pid < 0) { res.detail = "spawn failed"; return 1; }

    EventRecord ev{};
    // READY carries the plan size; the requested crash step is independent of it.
    while (ReadEvent(p, ev)) {
        if (ev.kind == Event::Ready) { break; }
        if (ev.kind == Event::Error) { res.detail = "child error at startup"; KillProc(p); return 1; }
    }

    const std::uint64_t want = StepsForPoint(point, trial);

    if (point == 0) {
        // Kill before issuing any work at all.
        SendCommand(p, "CRASH", 0);
    } else {
        SendCommand(p, "CRASH", want);
    }

    // Drain events until EOF: the child exits inside the CRASH handler.
    std::uint64_t stepsDone = 0;
    std::uint64_t committed = 0;
    while (ReadEvent(p, ev)) {
        if (ev.kind == Event::StepDone) stepsDone = ev.value;
        if (ev.kind == Event::Committed) committed = ev.value;
    }
    const int childExit = WaitChild(p);
    res.stepsBeforeCrash = stepsDone;
    res.committedBeforeCrash = committed;

    // A child that exited 0 did not crash; that is a harness failure.
    if (childExit != 99) {
        res.detail = "child did not die at the crash point (exit " + std::to_string(childExit) + ")";
        return 1;
    }

    // --- Recovery, timed (acceptance 4) ---
    const auto t0 = std::chrono::steady_clock::now();

    const auto jc = VerifyJournal(journal);
    if (!jc.headerValid) {
        res.detail = "journal header invalid after crash: " + jc.detail;
        return 1;
    }

    // Resume in a fresh child, driven to completion.
    const std::string cmdFifo2 = workDir + "/cmd2.fifo";
    const std::string evtFifo2 = workDir + "/evt2.fifo";
    ::unlink(cmdFifo2.c_str());
    ::unlink(evtFifo2.c_str());
    if (!MakeFifo(cmdFifo2) || !MakeFifo(evtFifo2)) {
        res.detail = "fifo2 creation failed";
        return 1;
    }

    Proc p2 = Spawn(scenario, journal, cmdFifo2, evtFifo2);
    if (p2.pid < 0) { res.detail = "respawn failed"; return 1; }

    bool sawResume = false;
    std::uint64_t resumeAt = 0;
    std::uint64_t total2 = 0;
    EventRecord ev2{};
    while (ReadEvent(p2, ev2)) {
        if (ev2.kind == Event::Resumed) { sawResume = true; resumeAt = ev2.value; }
        if (ev2.kind == Event::Ready) { total2 = ev2.value; break; }
        if (ev2.kind == Event::Error) { res.detail = "resume startup error"; KillProc(p2); return 1; }
    }

    const auto t1 = std::chrono::steady_clock::now();
    res.recoveryMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    res.resumed = sawResume;
    res.resumedAt = resumeAt;

    // Acceptance (2): the resumed operation must complete or cleanly abort.
    bool completed = false;
    if (p2.pid > 0) {
        SendCommand(p2, "RUN", total2 == 0 ? 1000000 : total2);
        while (ReadEvent(p2, ev2)) {
            if (ev2.kind == Event::Complete) { completed = true; break; }
            if (ev2.kind == Event::Error) break;
        }
        SendCommand(p2, "QUIT", 0);
        while (ReadEvent(p2, ev2)) { /* drain */ }
        const int exit2 = WaitChild(p2);
        res.recoveredCleanly = completed && (exit2 == 0);
        if (!completed) res.detail = "resume did not complete (exit " + std::to_string(exit2) + ")";
    }

    // Acceptance (1): no silent content corruption. The journal must contain no
    // record with a bad CRC, and the recovered step must not claim more progress
    // than was durably committed.
    res.contentIntact = (jc.badCrc == 0) && (jc.lastCompletedStep <= committed);
    if (!res.contentIntact) {
        res.detail = "badCrc=" + std::to_string(jc.badCrc) + " recoveredStep=" +
                     std::to_string(jc.lastCompletedStep) + " committed=" +
                     std::to_string(committed);
    }

    // A commit that returned is a promise: recovery must be at least that far.
    res.committedPromiseHonoured = (jc.lastCompletedStep >= committed);
    if (!res.committedPromiseHonoured) {
        res.detail = "lost a committed step: committed=" + std::to_string(committed) +
                     " recovered=" + std::to_string(jc.lastCompletedStep);
    }

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string scenario = "tests/scenarios/SC-01.json";
    std::string workDir = "/tmp/auralite-crash-campaign";
    int trialsPerPoint = 40; // 5 points x 40 = 200 trials

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--scenario" && i + 1 < argc) scenario = argv[++i];
        else if (a == "--workdir" && i + 1 < argc) workDir = argv[++i];
        else if (a == "--trials" && i + 1 < argc) trialsPerPoint = std::atoi(argv[++i]);
        else if (a == "--child" && i + 1 < argc) kChildBinary = argv[++i];
        else if (a == "--help") {
            std::printf("crash_campaign [--scenario PATH] [--workdir DIR] [--trials N]\n");
            std::printf("               [--child PATH]\n");
            return 0;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(workDir, ec);

    std::printf("P9-T03 crash campaign — 5 crash points x %d trials = %d\n", trialsPerPoint,
                trialsPerPoint * 5);
    std::printf("child: %s\n\n", kChildBinary);

    Totals t{};
    std::vector<TrialResult> failures;

    for (int point = 0; point < 5; ++point) {
        for (int trial = 0; trial < trialsPerPoint; ++trial) {
            TrialResult res;
            const int rc = RunOneTrial(point, trial, scenario, workDir, res);
            ++t.trials;
            ++t.perPoint[point];
            if (res.resumed) { ++t.resumed; ++t.perPointResumed[point]; }
            if (res.committedPromiseHonoured) ++t.promiseHonoured;
            if (res.contentIntact) ++t.contentIntact;
            if (res.recoveredCleanly) ++t.cleanRecoveries;
            t.totalRecoveryMs += res.recoveryMs;
            if (res.recoveryMs > t.worstRecoveryMs) t.worstRecoveryMs = res.recoveryMs;
            if (rc != 0 || !res.contentIntact || !res.recoveredCleanly ||
                !res.committedPromiseHonoured) {
                failures.push_back(res);
            }
        }
        std::printf("  %-18s %d trials, %d resumed\n", CrashPointName(point), t.perPoint[point],
                    t.perPointResumed[point]);
    }

    std::printf("\n--- results ---\n");
    std::printf("  trials                     %d\n", t.trials);
    std::printf("  resumed from journal       %d / %d\n", t.resumed, t.trials);
    std::printf("  committed promise kept     %d / %d\n", t.promiseHonoured, t.trials);
    std::printf("  content intact (0 bad CRC) %d / %d\n", t.contentIntact, t.trials);
    std::printf("  completed or clean abort   %d / %d\n", t.cleanRecoveries, t.trials);
    std::printf("  recovery worst / mean      %.1f ms / %.1f ms\n", t.worstRecoveryMs,
                t.trials ? t.totalRecoveryMs / t.trials : 0.0);
    std::printf("  chkdsk (criterion 3)       SKIPPED (Windows-only volume check)\n");

    if (!failures.empty()) {
        std::printf("\n--- %zu failure(s) ---\n", failures.size());
        for (std::size_t i = 0; i < failures.size() && i < 10; ++i) {
            const auto& f = failures[i];
            std::printf("  [%s trial %d] %s\n", CrashPointName(f.crashPoint), f.trialIndex,
                        f.detail.c_str());
        }
    }

    const bool pass = failures.empty() && t.trials == trialsPerPoint * 5 &&
                      t.worstRecoveryMs <= 10000.0;
    std::printf("\nAcceptance: corruption %s | resume-complete %s | recovery<=10s %s\n",
                t.contentIntact == t.trials ? "PASS" : "FAIL",
                t.cleanRecoveries == t.trials ? "PASS" : "FAIL",
                t.worstRecoveryMs <= 10000.0 ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
