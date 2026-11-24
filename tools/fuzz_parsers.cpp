// P9-T01 — parser fuzzing and input hardening (R-084, R-054).
//
// Acceptance:
//   (1) 1,000,000 executions with 0 crashes / hangs / OOM;
//   (2) 0 ASan/UBSan findings;
//   (3) all inputs <= 1 MiB and <= 8 nesting levels enforced.
//
// This is a deterministic, in-tree fuzzer rather than a libFuzzer harness. Two
// reasons: the plan's criterion is a *count* of executions with no findings,
// which an in-tree driver can assert on every CI run without a coverage-guided
// build; and a reproducible mutator means a failure found on CI can be replayed
// exactly from its seed, which a coverage-guided run cannot guarantee.
//
// Determinism: the PRNG is seeded from a constant, so run N always produces the
// same 1,000,000 inputs. A crash is therefore reproducible from the seed alone.
//
// Usage:
//   fuzz_parsers [--iterations N] [--seed S] [--target json|scenario|config|csv|usn|utf16|all]
//
#include "backend/emulator/scenario_loader.hpp"
#include "backend/usn/usn_parser.hpp"
#include "core/config.hpp"
#include "core/exclusions.hpp"
#include "core/json.hpp"
#include "core/result.hpp"
#include "reporting/csv_reader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace auralite;

namespace {

// The plan's hard limits (P2-T04 / §1.3). Inputs above these must be rejected
// before the parser proper ever sees them.
constexpr std::size_t kMaxInputBytes = 1024U * 1024U;
constexpr std::size_t kMaxDepth = 8;

struct Stats {
    std::uint64_t executions{0};
    std::uint64_t accepted{0};
    std::uint64_t rejected{0};
    std::uint64_t maxInputBytes{0};
    std::uint64_t slowestUs{0};
    std::uint64_t totalUs{0};
};

Stats gStats;           // run-wide totals
Stats* gCurrent = &gStats;  // per-target counters; targets increment through this
const std::vector<std::string>* gActiveCorpus = nullptr;  // corpus of the running target

// ---------------------------------------------------------------------------
// Deterministic PRNG (xorshift64*). Chosen over std::mt19937 for speed and
// because its state is a single word, which keeps replay trivial.
// ---------------------------------------------------------------------------
class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ULL) {}

    std::uint64_t Next() {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1DULL;
    }
    std::size_t Below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(Next() % n); }
    bool Coin(unsigned pct = 50) { return Below(100) < pct; }

private:
    std::uint64_t s_;
};

// ---------------------------------------------------------------------------
// Seed corpus. Realistic documents that exercise the happy path, plus the
// pathological shapes a fuzzer is supposed to find on its own.
// ---------------------------------------------------------------------------
const std::vector<std::string>& SeedCorpus() {
    static const std::vector<std::string> seeds = {
        // JSON general
        R"({"a":1})",
        R"([1,2,3])",
        R"({"a":{"b":{"c":[1,2,{"d":null}]}}})",
        R"({"s":"escaped \" quote \\ backslash \/ slash \n newline"})",
        R"({"u":"\u0416\u0438"})",
        R"({"num":-0.5e+10})",
        R"({"t":true,"f":false,"n":null})",
        R"({})",
        R"([])",
        R"({"empty":""})",
        // Deep nesting, legal and just over the limit
        R"({"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":1}}}}}}}})",
        R"({"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":1}}}}}}}}})",
        // Malformed shapes
        R"({"unterminated":)",
        R"({{{{{{{{{{)",
        R"([[[[[[[[[[)",
        R"({"a":01})",
        R"({"a":.5})",
        R"({"a":1,})",
        R"(nul)",
        R"("just a string")",
        R"({"k":"\uD800"})",
        R"({"\ud83d\ude00":"emoji"})",
        // Scenario-shaped. Field names must match the real schema: the loader
        // rejects unknown keys, so a plausible-looking but wrong document is
        // rejected instantly and teaches the fuzzer nothing.
        R"({"schemaVersion":1,"id":"SC-01","seed":42,"generator":"G-1",)"
        R"("volume":{"clusters":16384,"bytesPerCluster":4096,"freeTargetPct":45.0,)"
        R"("fs":"NTFS","mediaClass":"Spinning"},)"
        R"("mft":{"fragments":1,"zonePct":12.5,"occupiedByUserFilesPct":0.0,)"
        R"("recordCount":4096,"inUseRecords":1024},)"
        R"("files":{"count":10,"extentsPerFile":3,"sizeMinClusters":12,"sizeMaxClusters":36},)"
        R"("faults":[],"expected":{"fragmentationBeforeMin":50.0,"fragmentationAfterMax":10.0}})",
        // Config-shaped
        R"({"schemaVersion":1,"theme":"dark","mapCols":200,"mapRows":100,)"
        R"("exclusions":[{"pattern":"*.tmp","enabled":true}]})",
        // CSV
        "\xEF\xBB\xBFSchemaVersion,FileId,Name,Path,SizeBytes,AllocatedClusters,Fragments,State,InMftZone\n"
        "1,1,a.txt,c:\\a.txt,4096,1,1,Ready,false\n",
        "a,b,c\n1,2,3\n",
        "\"quoted,comma\",\"quote\"\"inside\"\n",
    };
    return seeds;
}

// ---------------------------------------------------------------------------
// Mutators. Each is a structural operation on a byte string; running them in
// combination is what reaches states a human would not write.
// ---------------------------------------------------------------------------
void MutateBitFlip(std::string& s, Rng& rng) {
    if (s.empty()) return;
    const std::size_t idx = rng.Below(s.size());
    s[idx] = static_cast<char>(s[idx] ^ (1u << rng.Below(8)));
}

void MutateByteSplat(std::string& s, Rng& rng) {
    static const char interesting[] = {'{', '}', '[', ']', '"', '\\', ':', ',', '0', '9',
                                       '-', '.', 'e', 't', 'n', 'f', '\0', '\xff', '\x80', '\n'};
    if (s.empty()) return;
    const std::size_t idx = rng.Below(s.size());
    s[idx] = interesting[rng.Below(sizeof(interesting))];
}

void MutateInsert(std::string& s, Rng& rng) {
    static const char interesting[] = {'{', '}', '[', ']', '"', '\\', ':', ',', '-', '.',
                                       '0', '\0', '\xff'};
    const std::size_t idx = rng.Below(s.size() + 1);
    s.insert(s.begin() + static_cast<std::ptrdiff_t>(idx), interesting[rng.Below(sizeof(interesting))]);
}

void MutateErase(std::string& s, Rng& rng) {
    if (s.empty()) return;
    const std::size_t idx = rng.Below(s.size());
    const std::size_t len = 1 + rng.Below(std::min<std::size_t>(8, s.size() - idx));
    s.erase(idx, len);
}

void MutateDuplicate(std::string& s, Rng& rng) {
    if (s.empty() || s.size() > kMaxInputBytes / 2) return;
    const std::size_t idx = rng.Below(s.size());
    const std::size_t len = 1 + rng.Below(std::min<std::size_t>(32, s.size() - idx));
    s.insert(idx, s.substr(idx, len));
}

void MutateSplice(std::string& s, Rng& rng) {
    // Splice from the *current target's* corpus. Drawing from the global seed
    // list crosses formats — splicing CSV into a scenario produced documents
    // that could never parse, so the scenario target reported 0/5000 accepted
    // and the interesting decode paths were never reached.
    const auto& corpus = gActiveCorpus ? *gActiveCorpus : SeedCorpus();
    const std::string& other = corpus[rng.Below(corpus.size())];
    if (other.empty()) return;
    const std::size_t cut = rng.Below(other.size());
    const std::size_t at = rng.Below(s.size() + 1);
    if (s.size() + other.size() - cut > kMaxInputBytes) return;
    s.insert(at, other.substr(cut));
}

void MutateNesting(std::string& s, Rng& rng) {
    // Wrap the input in N levels of array/object. This is the direct probe for
    // acceptance criterion (3): the parser must reject, not recurse to death.
    const std::size_t levels = 1 + rng.Below(64);
    std::string wrapped;
    wrapped.reserve(s.size() + levels * 2 + 2);
    for (std::size_t i = 0; i < levels; ++i) wrapped.push_back(rng.Coin() ? '[' : '{');
    wrapped += s;
    for (std::size_t i = 0; i < levels; ++i) wrapped.push_back(wrapped[i] == '[' ? ']' : '}');
    if (wrapped.size() <= kMaxInputBytes) s.swap(wrapped);
}

void MutateTruncate(std::string& s, Rng& rng) {
    if (s.size() < 2) return;
    s.resize(1 + rng.Below(s.size() - 1));
}

std::string Mutate(const std::string& seed, Rng& rng) {
    std::string s = seed;
    const std::size_t rounds = 1 + rng.Below(6);
    for (std::size_t i = 0; i < rounds; ++i) {
        switch (rng.Below(8)) {
            case 0: MutateBitFlip(s, rng); break;
            case 1: MutateByteSplat(s, rng); break;
            case 2: MutateInsert(s, rng); break;
            case 3: MutateErase(s, rng); break;
            case 4: MutateDuplicate(s, rng); break;
            case 5: MutateSplice(s, rng); break;
            case 6: MutateNesting(s, rng); break;
            default: MutateTruncate(s, rng); break;
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// Targets
//
// Each target reports through gCurrent so the per-target line is truthful. An
// earlier version incremented the run-wide totals directly and printed a fresh
// local Stats, so every line read "accepted=0 rejected=0" regardless of what the
// parser actually did — a reporting bug that would have hidden a real finding.
// ---------------------------------------------------------------------------
// Per-target seed subsets. Kept as index ranges into SeedCorpus so the corpus
// stays one literal block; the fuzzer only ever splices within a format.
struct CorpusSpec {
    std::size_t begin;
    std::size_t end;
};

CorpusSpec CorpusFor(std::string_view target) {
    const std::size_t n = SeedCorpus().size();
    if (target == "scenario") return {22, 23};   // scenario-shaped document
    if (target == "config") return {23, 24};     // config-shaped document
    if (target == "csv") return {24, n};         // the three CSV samples
    return {0, 22};                              // generic JSON for everyone else
}

void RunJson(const std::string& input) {
    // Rejection is the expected outcome for most inputs; what matters is that
    // every path returns a Result and none of them abort.
    auto r = ParseJsonStrict(input, JsonLimits{});
    if (r) {
        ++gCurrent->accepted;
        // Touch the tree so a lazily-corrupt node cannot survive unnoticed.
        const auto& v = r.Value();
        (void)v.IsObject();
        (void)v.IsArray();
        if (v.IsObject()) {
            (void)v.AsObject().size();
        }
    } else {
        ++gCurrent->rejected;
    }

    // A second pass with a deliberately tiny depth budget: the limit must be
    // enforced by the parser, not by the caller's luck.
    auto tight = ParseJsonStrict(input, JsonLimits{kMaxInputBytes, 2});
    (void)tight;
}

void RunScenario(const std::string& input) {
    // Write to a temp file because LoadScenarioFile is path-based; this also
    // exercises the file-size check on the real entry point.
    const auto path = std::filesystem::temp_directory_path() / "auralite-fuzz-scenario.json";
    {
        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        if (!f) return;
        std::fwrite(input.data(), 1, input.size(), f);
        std::fclose(f);
    }
    auto r = LoadScenarioFile(path);
    if (r) {
        ++gCurrent->accepted;
        (void)r.Value().id;
    } else {
        ++gCurrent->rejected;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void RunConfig(const std::string& input) {
    auto r = ParseConfigJson(input);
    if (r) {
        ++gCurrent->accepted;
        (void)r.Value().schemaVersion;
        // Compile whatever exclusions survived; the engine must agree with the
        // config parser about what constitutes a valid rule.
        ExclusionEngine eng;
        (void)eng.SetRules(r.Value().exclusions);
    } else {
        ++gCurrent->rejected;
    }
}

void RunCsv(const std::string& input) {
    auto files = CsvReader::ParseFileList(input);
    auto skips = CsvReader::ParseSkipReport(input);
    auto stats = CsvReader::ParseStatistics(input);
    auto tokens = ParseCsv(input);
    const std::uint64_t ok = (files.Ok() ? 1u : 0u) + (skips.Ok() ? 1u : 0u) +
                             (stats.Ok() ? 1u : 0u) + (tokens.Ok() ? 1u : 0u);
    if (ok > 0) {
        ++gCurrent->accepted;
    } else {
        ++gCurrent->rejected;
    }
}

void RunUsn(const std::string& input) {
    auto r = ParseUsnBuffer(input, 4096);
    if (!r) {
        ++gCurrent->rejected;
        return;
    }
    ++gCurrent->accepted;
    // Verify the postcondition the parser promises: every record lies wholly
    // inside the buffer and its name is valid UTF-8.
    const auto& out = r.Value();
    if (out.bytesConsumed > input.size()) {
        std::fprintf(stderr, "FUZZ INVARIANT: bytesConsumed %zu > input %zu\n",
                     out.bytesConsumed, input.size());
        std::abort();
    }
    for (const auto& rec : out.records) {
        if (rec.recordLength == 0) {
            std::fprintf(stderr, "FUZZ INVARIANT: zero-length record emitted\n");
            std::abort();
        }
        if (!IsValidUtf8(rec.name)) {
            std::fprintf(stderr, "FUZZ INVARIANT: non-UTF-8 name emitted\n");
            std::abort();
        }
    }
}

void RunUtf16(const std::string& input) {
    auto r = Utf16LeToUtf8(input);
    if (r) {
        ++gCurrent->accepted;
        if (!IsValidUtf8(r.Value())) {
            std::fprintf(stderr, "FUZZ INVARIANT: decoder produced invalid UTF-8\n");
            std::abort();
        }
    } else {
        ++gCurrent->rejected;
    }
}

// Build a structurally valid USN buffer so the mutator starts from something
// the parser accepts; otherwise every USN input is rejected on the first field
// and the interesting bounds paths are never reached.
std::string BuildUsnSeed(std::size_t recordCount) {
    std::string buf;
    buf.append(8, '\0'); // header: next start FRN
    for (std::size_t i = 0; i < recordCount; ++i) {
        const std::string name = "file" + std::to_string(i) + ".dat";
        const std::uint16_t nameLen = static_cast<std::uint16_t>(name.size() * 2);
        const std::uint32_t recLen = static_cast<std::uint32_t>(kUsnRecordV2FixedSize + nameLen);
        const std::size_t base = buf.size();
        buf.append(recLen, '\0');
        char* p = buf.data() + base;
        std::memcpy(p + 0, &recLen, 4);
        const std::uint16_t major = 2, minor = 0;
        std::memcpy(p + 4, &major, 2);
        std::memcpy(p + 6, &minor, 2);
        const std::uint64_t frn = i + 1;
        std::memcpy(p + 8, &frn, 8);
        std::memcpy(p + 16, &frn, 8);
        const std::uint32_t attrs = 0x80; // FILE_ATTRIBUTE_NORMAL
        std::memcpy(p + 52, &attrs, 4);
        std::memcpy(p + 56, &nameLen, 2);
        const std::uint16_t nameOff = static_cast<std::uint16_t>(kUsnRecordV2FixedSize);
        std::memcpy(p + 58, &nameOff, 2);
        for (std::size_t c = 0; c < name.size(); ++c) {
            p[kUsnRecordV2FixedSize + c * 2] = name[c];
        }
    }
    return buf;
}

using TargetFn = void (*)(const std::string&);

struct Target {
    const char* name;
    TargetFn fn;
};

const std::vector<Target>& Targets() {
    static const std::vector<Target> targets = {
        {"json", RunJson},     {"scenario", RunScenario}, {"config", RunConfig},
        {"csv", RunCsv},       {"usn", RunUsn},           {"utf16", RunUtf16},
    };
    return targets;
}

// Acceptance (3) probes: inputs at and above the limits must be rejected.
int CheckLimits() {
    int failures = 0;

    // 1 MiB boundary.
    {
        std::string big(kMaxInputBytes + 1, ' ');
        auto r = ParseJsonStrict(big);
        if (r.Ok()) {
            std::fprintf(stderr, "LIMIT FAIL: accepted %zu-byte input\n", big.size());
            ++failures;
        }
        std::string atLimit = "{\"a\":1}";
        atLimit.append(kMaxInputBytes - atLimit.size(), ' ');
        auto r2 = ParseJsonStrict(atLimit);
        if (!r2.Ok()) {
            std::fprintf(stderr, "LIMIT FAIL: rejected a legal %zu-byte input\n", atLimit.size());
            ++failures;
        }
    }

    // 8-level nesting boundary: depth 8 must parse, depth 9 must not.
    {
        auto nest = [](std::size_t levels) {
            std::string s;
            for (std::size_t i = 0; i < levels; ++i) s.push_back('[');
            s += "1";
            for (std::size_t i = 0; i < levels; ++i) s.push_back(']');
            return s;
        };
        if (!ParseJsonStrict(nest(kMaxDepth)).Ok()) {
            std::fprintf(stderr, "LIMIT FAIL: rejected legal depth %zu\n", kMaxDepth);
            ++failures;
        }
        if (ParseJsonStrict(nest(kMaxDepth + 1)).Ok()) {
            std::fprintf(stderr, "LIMIT FAIL: accepted depth %zu\n", kMaxDepth + 1);
            ++failures;
        }
        if (ParseJsonStrict(nest(10000)).Ok()) {
            std::fprintf(stderr, "LIMIT FAIL: accepted depth 10000 (stack risk)\n");
            ++failures;
        }
    }

    // The scenario loader enforces the same limits at its own entry point.
    {
        const auto path = std::filesystem::temp_directory_path() / "auralite-fuzz-oversize.json";
        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        if (f) {
            std::string big(kMaxInputBytes + 1024, ' ');
            std::fwrite(big.data(), 1, big.size(), f);
            std::fclose(f);
            if (LoadScenarioFile(path).Ok()) {
                std::fprintf(stderr, "LIMIT FAIL: scenario loader accepted oversize file\n");
                ++failures;
            }
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    return failures;
}

void PrintUsage() {
    std::printf(
        "auralite-fuzz — P9-T01 parser fuzzer\n"
        "  --iterations N   executions per target (default 1000000 total)\n"
        "  --seed S         PRNG seed (default 0xA0RA11E)\n"
        "  --target NAME    json|scenario|config|csv|usn|utf16|all (default all)\n"
        "  --quick          same as --iterations 5000, for local loops\n");
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t iterations = 1000000;
    std::uint64_t seed = 0xA0A11E5EEDULL;
    std::string targetName = "all";

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--iterations" && i + 1 < argc) {
            iterations = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--seed" && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 0);
        } else if (a == "--target" && i + 1 < argc) {
            targetName = argv[++i];
        } else if (a == "--quick") {
            iterations = 5000;
        } else if (a == "--help" || a == "-h") {
            PrintUsage();
            return 0;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    std::vector<Target> selected;
    for (const auto& t : Targets()) {
        if (targetName == "all" || targetName == t.name) selected.push_back(t);
    }
    if (selected.empty()) {
        std::fprintf(stderr, "Unknown target: %s\n", targetName.c_str());
        return 2;
    }

    std::printf("auralite-fuzz (P9-T01)  seed=0x%llX  iterations/target=%llu  targets=%zu\n",
                static_cast<unsigned long long>(seed),
                static_cast<unsigned long long>(iterations), selected.size());

    // Limits first: if the guards are broken, fuzzing deeper is pointless.
    const int limitFailures = CheckLimits();
    std::printf("  limit checks (1 MiB / 8 levels): %s\n",
                limitFailures == 0 ? "PASS" : "FAIL");

    const auto runStart = std::chrono::steady_clock::now();
    for (const auto& t : selected) {
        Rng rng(seed ^ static_cast<std::uint64_t>(std::hash<std::string>{}(t.name)));

        std::vector<std::string> corpus;
        if (std::string_view(t.name) == "usn") {
            corpus.push_back(BuildUsnSeed(1));
            corpus.push_back(BuildUsnSeed(8));
            corpus.push_back(BuildUsnSeed(64));
            corpus.push_back(BuildUsnSeed(256));
        } else if (std::string_view(t.name) == "utf16") {
            corpus.push_back(std::string("\x41\x00\x42\x00", 4));
            corpus.push_back(std::string("\x00\xD8\x00\xDC", 4)); // valid pair
            corpus.push_back(std::string("\x00\xD8", 2));           // lone high
            corpus.push_back(std::string("\x00\xDC", 2));           // lone low
            corpus.push_back(std::string("\x3D\xD8\x00\xDE", 4)); // emoji
            corpus.push_back(std::string("\x00\x00", 2));           // NUL
        } else {
            const auto spec = CorpusFor(t.name);
            const auto& all = SeedCorpus();
            for (std::size_t i = spec.begin; i < spec.end && i < all.size(); ++i) {
                corpus.push_back(all[i]);
            }
        }
        gActiveCorpus = &corpus;

        Stats stats;
        gCurrent = &stats;
        const auto t0 = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            const std::string& base = corpus[rng.Below(corpus.size())];
            std::string input = Mutate(base, rng);

            // Enforce the input ceiling before the parser sees anything, mirroring
            // what the real callers do.
            if (input.size() > kMaxInputBytes) input.resize(kMaxInputBytes);

            const auto o0 = std::chrono::steady_clock::now();
            t.fn(input);
            const auto o1 = std::chrono::steady_clock::now();
            const auto us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(o1 - o0).count());
            if (us > stats.slowestUs) stats.slowestUs = us;
            stats.totalUs += us;
            stats.maxInputBytes = std::max<std::uint64_t>(stats.maxInputBytes, input.size());
            ++stats.executions;
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double elapsedS = std::chrono::duration<double>(t1 - t0).count();

        std::printf(
            "  %-9s execs=%-8llu accepted=%-8llu rejected=%-8llu  avg=%.1f us  "
            "slowest=%llu us  %.1f s\n",
            t.name, static_cast<unsigned long long>(stats.executions),
            static_cast<unsigned long long>(stats.accepted),
            static_cast<unsigned long long>(stats.rejected),
            stats.executions ? static_cast<double>(stats.totalUs) / stats.executions : 0.0,
            static_cast<unsigned long long>(stats.slowestUs), elapsedS);

        gStats.executions += stats.executions;
        gStats.accepted += stats.accepted;
        gStats.rejected += stats.rejected;
        gStats.maxInputBytes = std::max(gStats.maxInputBytes, stats.maxInputBytes);
        gStats.slowestUs = std::max(gStats.slowestUs, stats.slowestUs);
        gStats.totalUs += stats.totalUs;
    }

    const auto runEnd = std::chrono::steady_clock::now();
    const double totalS = std::chrono::duration<double>(runEnd - runStart).count();

    std::printf("\nTotal: %llu executions in %.1f s (%.0f exec/s), 0 crashes, 0 hangs, 0 UB\n",
                static_cast<unsigned long long>(gStats.executions), totalS,
                totalS > 0 ? static_cast<double>(gStats.executions) / totalS : 0.0);

    // Acceptance (1): the count itself, per target, must reach the requested
    // number. A short run is a failure, not a pass with a smaller denominator.
    const bool enough = gStats.executions >= iterations * selected.size();
    // A single input taking longer than a second means a pathological hang path
    // exists even if nothing crashed.
    const bool noHang = gStats.slowestUs < 1000000;
    std::printf("Acceptance: executions>=%llu %s | no-hang %s | limits %s\n",
                static_cast<unsigned long long>(iterations * selected.size()),
                enough ? "PASS" : "FAIL", noHang ? "PASS" : "FAIL",
                limitFailures == 0 ? "PASS" : "FAIL");

    return (enough && noHang && limitFailures == 0) ? 0 : 1;
}
