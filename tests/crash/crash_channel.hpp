#pragma once
//
// P9-T03 — crash campaign rendezvous channel.
//
// The campaign needs a child process to die at an exact step. In-process fault
// injection (what the existing 200-trial test does) cannot prove anything about
// durability: the page cache, the standard-library buffers and the process's own
// heap all survive, so a "crash" that never actually loses unsynced writes is a
// much weaker test than it looks.
//
// Two channels are used:
//
//   CtrlIn   parent -> child, one command per line.
//            "RUN <n>"     execute n steps then commit and report DONE
//            "CRASH <n>"   execute n steps then _exit(99) without any cleanup
//
//   Events   child -> parent, one event per line, flushed before every blocking
//            read. The parent reads events to know how far the child got before
//            it died, which is what makes "did it lose a committed step?" a
//            question with an answer.
//
// Both are plain FIFOs on POSIX. Nothing here is used on Windows; the campaign is
// a CI-only tool, not part of the product.
//
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace auralite::crash {

enum class Event : std::uint8_t {
    Ready,
    StepStarted,
    StepDone,
    Committed,
    Resumed,
    Complete,
    Error,
    Bye,
};

inline const char* EventName(Event e) noexcept {
    switch (e) {
        case Event::Ready: return "READY";
        case Event::StepStarted: return "STEP_START";
        case Event::StepDone: return "STEP_DONE";
        case Event::Committed: return "COMMIT";
        case Event::Resumed: return "RESUMED";
        case Event::Complete: return "COMPLETE";
        case Event::Error: return "ERROR";
        case Event::Bye: return "BYE";
    }
    return "?";
}

inline bool ParseEvent(std::string_view s, Event& out) noexcept {
    if (s == "READY") { out = Event::Ready; return true; }
    if (s == "STEP_START") { out = Event::StepStarted; return true; }
    if (s == "STEP_DONE") { out = Event::StepDone; return true; }
    if (s == "COMMIT") { out = Event::Committed; return true; }
    if (s == "RESUMED") { out = Event::Resumed; return true; }
    if (s == "COMPLETE") { out = Event::Complete; return true; }
    if (s == "ERROR") { out = Event::Error; return true; }
    if (s == "BYE") { out = Event::Bye; return true; }
    return false;
}

struct EventRecord {
    Event kind{Event::Ready};
    std::uint64_t value{0};   // step index, or checkpoint step for Comitted
    bool isResume{false};
};

// A line-oriented writer over a FILE*. Every write is flushed, so the parent sees
// the event even if the child dies immediately afterwards.
class EventWriter {
public:
    explicit EventWriter(std::FILE* out) : out_(out) {}

    void Emit(Event e, std::uint64_t value = 0, bool isResume = false) {
        if (!out_) return;
        std::fprintf(out_, "%s %llu %d\n", EventName(e),
                     static_cast<unsigned long long>(value), isResume ? 1 : 0);
        std::fflush(out_);
    }

private:
    std::FILE* out_;
};

// Reads "CMD <value>" lines. Returns false on EOF.
class CommandReader {
public:
    explicit CommandReader(std::FILE* in) : in_(in) {}

    bool Next(std::string& cmd, std::uint64_t& value) {
        if (!in_) return false;
        char buf[128]{};
        if (!std::fgets(buf, sizeof(buf), in_)) return false;
        std::string_view line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        const auto sp = line.find(' ');
        if (sp == std::string_view::npos) {
            cmd = std::string(line);
            value = 0;
            return true;
        }
        cmd = std::string(line.substr(0, sp));
        value = std::strtoull(std::string(line.substr(sp + 1)).c_str(), nullptr, 10);
        return true;
    }

private:
    std::FILE* in_;
};

} // namespace auralite::crash
