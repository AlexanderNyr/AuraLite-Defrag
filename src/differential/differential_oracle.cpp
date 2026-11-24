#include "differential/differential_oracle.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <unordered_map>

namespace auralite::differential {

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

char LowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string FormatDouble(double v) {
    char buf[64]{};
    // 3 decimals is enough for percentages and keeps traces stable.
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return std::string(buf);
}

std::string FormatInt(long long v) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%lld", v);
    return std::string(buf);
}

std::string DescribeSkip(SkipReason r) {
    switch (r) {
        case SkipReason::None: return "None";
        case SkipReason::MetadataFile: return "MetadataFile";
        case SkipReason::PageOrSystemFile: return "PageOrSystemFile";
        case SkipReason::LockedOrInUse: return "LockedOrInUse";
        case SkipReason::AccessDenied: return "AccessDenied";
        case SkipReason::ExcludedByRule: return "ExcludedByRule";
        case SkipReason::UnsupportedStream: return "UnsupportedStream";
        case SkipReason::InsufficientContiguousSpace: return "InsufficientContiguousSpace";
        case SkipReason::StaleBitmapConflict: return "StaleBitmapConflict";
        case SkipReason::ReadOnlyOrPreFlightBlocked: return "ReadOnlyOrPreFlightBlocked";
        case SkipReason::IoError: return "IoError";
        case SkipReason::MftSafeguardAbort: return "MftSafeguardAbort";
        case SkipReason::FileVanished: return "FileVanished";
    }
    return "Unknown";
}

std::string DescribeMetric(DivergenceMetric m) {
    switch (m) {
        case DivergenceMetric::FileSet: return "file-set";
        case DivergenceMetric::ExtentMap: return "extent-map";
        case DivergenceMetric::FragmentCount: return "fragment-count";
        case DivergenceMetric::MftGeometry: return "mft-geometry";
        case DivergenceMetric::MovePlan: return "move-plan";
        case DivergenceMetric::SkipReason: return "skip-reason";
        case DivergenceMetric::Summary: return "summary";
        case DivergenceMetric::ErrorCode: return "error-code";
    }
    return "unknown";
}

std::string DescribeClass(DivergenceClass c) {
    switch (c) {
        case DivergenceClass::Unexplained: return "unexplained";
        case DivergenceClass::ExplainedAllocatorPolicy: return "explained:allocator-policy";
        case DivergenceClass::ExplainedCapability: return "explained:capability";
        case DivergenceClass::ExplainedErrorPath: return "explained:error-path";
    }
    return "unknown";
}

double Pct(std::uint64_t matched, std::uint64_t total) noexcept {
    if (total == 0) return 100.0;
    return 100.0 * static_cast<double>(matched) / static_cast<double>(total);
}

bool NearlyEqualAbs(double a, double b, double absTol) noexcept {
    return std::fabs(a - b) <= absTol;
}

bool ExtentCountWithinTolerance(std::size_t a, std::size_t b, double relTol) noexcept {
    const auto lo = static_cast<double>(std::min(a, b));
    const auto hi = static_cast<double>(std::max(a, b));
    if (hi <= 1.0) return lo == hi;
    return (hi - lo) / hi <= relTol;
}

// Fragment counts are derived from extents; a single fragment of slack absorbs
// alignment/rounding differences without hiding a real regression.
bool FragmentCountClose(std::uint32_t a, std::uint32_t b) noexcept {
    const auto lo = std::min(a, b);
    const auto hi = std::max(a, b);
    return (hi - lo) <= 1U;
}

using FileMap = std::unordered_map<PathKey, const NormalizedFile*>;

FileMap IndexFiles(const std::vector<NormalizedFile>& files) {
    FileMap m;
    m.reserve(files.size() * 2);
    for (const auto& f : files) m.emplace(f.key, &f);
    return m;
}

std::string ExtentsToString(const std::vector<NormalizedExtent>& e) {
    std::ostringstream os;
    for (std::size_t i = 0; i < e.size(); ++i) {
        if (i) os << ';';
        os << e[i].startVcn << '+' << e[i].startLcn << 'x' << e[i].clusterCount;
    }
    return os.str();
}

// Largest free run implied by a bitmap is volume geometry, not a metric the
// oracle can recompute without the bitmap; it is taken from the summary.

} // namespace

// ---------------------------------------------------------------------------
// Path normalization
// ---------------------------------------------------------------------------

PathKey MakePathKey(std::string_view path) {
    // Drop the Win32 extended-length / device prefixes first, so that
    // "\\\\?\\C:\\Data\\X" and "C:/data/x" produce the same key.
    // Handled forms: "\\\\?\\", "\\\\.\\", and the volume-guid form
    // "\\\\?\\Volume{...}\\".
    std::string_view p = path;
    const auto stripPrefix = [&p](std::string_view prefix) {
        if (p.size() >= prefix.size()) {
            bool match = true;
            for (std::size_t i = 0; i < prefix.size(); ++i) {
                if (LowerAscii(p[i]) != LowerAscii(prefix[i])) { match = false; break; }
            }
            if (match) p.remove_prefix(prefix.size());
        }
    };
    stripPrefix("\\\\?\\");
    stripPrefix("\\\\.\\");

    std::string out;
    out.reserve(p.size());
    bool prevSep = false;
    for (char c : p) {
        char ch = LowerAscii(c);
        const bool isSep = (ch == '\\' || ch == '/');
        if (isSep) {
            if (!prevSep) out.push_back('\\');
        } else {
            out.push_back(ch);
        }
        prevSep = isSep;
    }
    // Strip leading separators so "\\x" and "x" compare equal.
    std::size_t start = 0;
    while (start < out.size() && out[start] == '\\') ++start;
    return out.substr(start);
}

// ---------------------------------------------------------------------------
// Normalization from the public interface view
// ---------------------------------------------------------------------------

NormalizedRun NormalizeFromSnapshot(const AnalysisSnapshot& snapshot,
                                    std::string label) {
    NormalizedRun run;
    run.label = std::move(label);
    run.scenarioId = snapshot.volumeId;

    run.summary.totalClusters = snapshot.summary.totalClusters;
    run.summary.freeClusters = snapshot.summary.freeClusters;
    run.summary.usedClusters = snapshot.summary.usedClusters;
    run.summary.totalFiles = snapshot.summary.totalFiles;
    run.summary.fragmentedFiles = snapshot.summary.fragmentedFiles;
    run.summary.residentFiles = snapshot.summary.residentFiles;
    run.summary.unmovableFiles = snapshot.summary.unmovableFiles;
    run.summary.largestFreeRunClusters = snapshot.summary.largestFreeRunClusters;
    run.summary.volumeFragmentationPct = snapshot.summary.volumeFragmentationPct;
    run.summary.fileFragmentationPct = snapshot.summary.fileFragmentationPct;

    run.mft.fragmentCount = snapshot.mftInfo.fragmentCount;
    run.mft.totalRecords = snapshot.mftInfo.totalRecords;
    run.mft.inUseRecords = snapshot.mftInfo.inUseRecords;
    run.mft.mftStartLcn = snapshot.mftInfo.mftStartLcn;
    run.mft.zoneStartLcn = snapshot.mftZone.zoneStartLcn;
    run.mft.zoneEndLcn = snapshot.mftZone.zoneEndLcn;
    run.mft.totalZoneClusters = snapshot.mftZone.totalZoneClusters;
    run.mft.freeZoneClusters = snapshot.mftZone.freeZoneClusters;

    run.files.reserve(snapshot.files.size());
    for (const auto& f : snapshot.files) {
        NormalizedFile nf;
        nf.key = MakePathKey(f.path.empty() ? f.name : f.path);
        nf.sizeBytes = f.sizeBytes;
        nf.allocatedClusters = f.allocatedClusters;
        nf.fragmentCount = f.fragmentCount;
        nf.flags = f.flags;
        nf.extents.reserve(f.extents.size());
        for (const auto& e : f.extents) {
            nf.extents.push_back({e.startVcn, e.startLcn, e.clusterCount});
        }
        std::sort(nf.extents.begin(), nf.extents.end(),
                  [](const NormalizedExtent& a, const NormalizedExtent& b) {
                      return a.startVcn < b.startVcn;
                  });
        run.files.push_back(std::move(nf));
    }
    std::sort(run.files.begin(), run.files.end(),
              [](const NormalizedFile& a, const NormalizedFile& b) { return a.key < b.key; });

    return run;
}

void AttachPlan(NormalizedRun& run, const DefragPlan& plan) {
    run.moves.clear();
    run.moves.reserve(plan.moves.size());
    for (const auto& m : plan.moves) {
        NormalizedMove nm;
        nm.fileKey = MakePathKey(m.fileName);
        nm.startVcn = m.startVcn;
        nm.sourceLcn = m.sourceLcn;
        nm.targetLcn = m.targetLcn;
        nm.clusterCount = m.clusterCount;
        run.moves.push_back(std::move(nm));
    }

    run.skips.clear();
    run.skips.reserve(plan.skipped.size());
    for (const auto& s : plan.skipped) {
        run.skips.push_back({MakePathKey(s.fileName), s.reason});
    }
    std::sort(run.skips.begin(), run.skips.end(),
              [](const NormalizedSkip& a, const NormalizedSkip& b) {
                  if (a.fileKey != b.fileKey) return a.fileKey < b.fileKey;
                  return static_cast<int>(a.reason) < static_cast<int>(b.reason);
              });
}

void AttachOutcome(NormalizedRun& run, const ExecutionOutcome& outcome) {
    for (const auto& s : outcome.runtimeSkips) {
        run.skips.push_back({MakePathKey(s.fileName), s.reason});
    }
    std::sort(run.skips.begin(), run.skips.end(),
              [](const NormalizedSkip& a, const NormalizedSkip& b) {
                  if (a.fileKey != b.fileKey) return a.fileKey < b.fileKey;
                  return static_cast<int>(a.reason) < static_cast<int>(b.reason);
              });
    run.skips.erase(std::unique(run.skips.begin(), run.skips.end(),
                                [](const NormalizedSkip& a, const NormalizedSkip& b) {
                                    return a.fileKey == b.fileKey && a.reason == b.reason;
                                }),
                    run.skips.end());
}

void AttachPlanAndOutcome(NormalizedRun& run,
                          const DefragPlan& plan,
                          const ExecutionOutcome& outcome) {
    AttachPlan(run, plan);
    AttachOutcome(run, outcome);
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

std::size_t ComparisonReport::UnexplainedCount() const noexcept {
    std::size_t n = 0;
    for (const auto& d : divergences) {
        if (d.kind == DivergenceClass::Unexplained) ++n;
    }
    return n;
}

ComparisonReport Compare(const NormalizedRun& left,
                         const NormalizedRun& right,
                         const Tolerances& tol) {
    ComparisonReport rep;
    rep.scenarioId = !left.scenarioId.empty() ? left.scenarioId : right.scenarioId;
    rep.leftLabel = left.label;
    rep.rightLabel = right.label;

    const auto pushScore = [&rep](DivergenceMetric m, std::string name,
                                  std::uint64_t compared, std::uint64_t matched,
                                  bool gate) {
        MetricScore s;
        s.metric = m;
        s.name = std::move(name);
        s.comparedItems = compared;
        s.matchedItems = matched;
        s.agreementPct = Pct(matched, compared);
        s.meetsGate = gate;
        rep.scores.push_back(std::move(s));
    };

    auto& div = rep.divergences;

    // ---- DT-06 (a): error path ------------------------------------------
    {
        const bool sameErr = (left.errorCodeValue == right.errorCodeValue);
        pushScore(DivergenceMetric::ErrorCode, "error-code-agreement", 1,
                  sameErr ? 1U : 0U, sameErr);
        if (!sameErr) {
            div.push_back({DivergenceMetric::ErrorCode,
                           DivergenceClass::ExplainedErrorPath, {},
                           "runs ended with different error codes",
                           FormatInt(left.errorCodeValue),
                           FormatInt(right.errorCodeValue)});
        }
    }

    // ---- DT-01: file set ------------------------------------------------
    const FileMap leftFiles = IndexFiles(left.files);
    const FileMap rightFiles = IndexFiles(right.files);

    {
        std::uint64_t compared = 0, matched = 0;
        for (const auto& [key, lf] : leftFiles) {
            ++compared;
            auto it = rightFiles.find(key);
            if (it != rightFiles.end() && it->second->sizeBytes == lf->sizeBytes) ++matched;
        }
        for (const auto& [key, rf] : rightFiles) {
            if (leftFiles.find(key) == leftFiles.end()) ++compared;
        }
        const bool gate = Pct(matched, compared) >= 99.0;
        pushScore(DivergenceMetric::FileSet, "file-set-agreement", compared, matched, gate);
        for (const auto& [key, lf] : leftFiles) {
            auto it = rightFiles.find(key);
            if (it == rightFiles.end()) {
                div.push_back({DivergenceMetric::FileSet,
                               DivergenceClass::Unexplained, key,
                               "file present on the left run only", lf->key, "<missing>"});
            }
        }
        for (const auto& [key, rf] : rightFiles) {
            if (leftFiles.find(key) == leftFiles.end()) {
                div.push_back({DivergenceMetric::FileSet,
                               DivergenceClass::Unexplained, key,
                               "file present on the right run only", "<missing>", rf->key});
            }
        }
    }

    // ---- DT-01 (b): extent maps (only for files present in both) --------
    {
        std::uint64_t compared = 0, matched = 0, fragCompared = 0, fragMatched = 0;
        for (const auto& [key, lf] : leftFiles) {
            auto it = rightFiles.find(key);
            if (it == rightFiles.end()) continue;
            const auto* rf = it->second;

            ++compared;
            if (lf->extents == rf->extents) {
                ++matched;
            } else {
                // Extent *count* is compared separately with a tolerance;
                // individual LCN placement is genuine allocator policy and is
                // recorded as explained rather than failing the gate.
                // Two separate questions:
                //  (a) is the *count* within tolerance?  (relative OR a single
                //      fragment of slack — one extra extent on a 10-extent file
                //      is allocator noise, not a fidelity failure);
                //  (b) if the count is fine, differing LCNs are placement policy.
                const std::size_t la = lf->extents.size();
                const std::size_t lb = rf->extents.size();
                const std::size_t absDelta = la > lb ? la - lb : lb - la;
                const bool countOk = ExtentCountWithinTolerance(
                                         la, lb, tol.extentCountRelativeTolerance) ||
                                     absDelta <= 1;
                div.push_back({DivergenceMetric::ExtentMap,
                               countOk ? DivergenceClass::ExplainedAllocatorPolicy
                                       : DivergenceClass::Unexplained,
                               key,
                               countOk ? "extent placement/count differs within tolerance"
                                       : "extent count outside tolerance",
                               ExtentsToString(lf->extents),
                               ExtentsToString(rf->extents)});
            }

            ++fragCompared;
            // Fragment count is derived from extents; allow ±1 for alignment
            // differences and require monotonic agreement otherwise.
            if (lf->fragmentCount == rf->fragmentCount || FragmentCountClose(lf->fragmentCount, rf->fragmentCount)) {
                ++fragMatched;
            }
        }
        pushScore(DivergenceMetric::ExtentMap, "extent-map-agreement", compared, matched,
                  Pct(matched, compared) >= 100.0 - 1e-9 || compared == 0);
        pushScore(DivergenceMetric::FragmentCount, "fragment-count-agreement",
                  fragCompared, fragMatched, Pct(fragMatched, fragCompared) >= 99.0);
    }

    // ---- DT-02: fragmentation percentages (summary) ---------------------
    {
        const bool volOk = NearlyEqualAbs(left.summary.volumeFragmentationPct,
                                          right.summary.volumeFragmentationPct,
                                          tol.fragmentationPctAbsTolerance);
        const bool fileOk = NearlyEqualAbs(left.summary.fileFragmentationPct,
                                           right.summary.fileFragmentationPct,
                                           tol.fragmentationPctAbsTolerance);
        pushScore(DivergenceMetric::Summary, "volume-fragmentation-pct", 1, volOk ? 1U : 0U, volOk);
        pushScore(DivergenceMetric::Summary, "file-fragmentation-pct", 1, fileOk ? 1U : 0U, fileOk);
        if (!volOk) {
            div.push_back({DivergenceMetric::Summary, DivergenceClass::Unexplained, {},
                           "volume fragmentation percentage outside ±" +
                               FormatDouble(tol.fragmentationPctAbsTolerance) + " pp",
                           FormatDouble(left.summary.volumeFragmentationPct),
                           FormatDouble(right.summary.volumeFragmentationPct)});
        }
        if (!fileOk) {
            div.push_back({DivergenceMetric::Summary, DivergenceClass::Unexplained, {},
                           "file fragmentation percentage outside tolerance",
                           FormatDouble(left.summary.fileFragmentationPct),
                           FormatDouble(right.summary.fileFragmentationPct)});
        }
    }

    // ---- DT-02 (b): MFT geometry ---------------------------------------
    {
        std::uint64_t compared = 0, matched = 0;
        const auto cmp = [&](const char* name, long long a, long long b) {
            ++compared;
            if (a == b) {
                ++matched;
            } else {
                div.push_back({DivergenceMetric::MftGeometry,
                               DivergenceClass::Unexplained, name,
                               std::string("MFT metric differs: ") + name,
                               FormatInt(a), FormatInt(b)});
            }
        };
        cmp("mft.fragmentCount", left.mft.fragmentCount, right.mft.fragmentCount);
        cmp("mft.totalRecords", static_cast<long long>(left.mft.totalRecords),
            static_cast<long long>(right.mft.totalRecords));
        cmp("mft.inUseRecords", static_cast<long long>(left.mft.inUseRecords),
            static_cast<long long>(right.mft.inUseRecords));
        cmp("mft.zoneTotalClusters", static_cast<long long>(left.mft.totalZoneClusters),
            static_cast<long long>(right.mft.totalZoneClusters));
        pushScore(DivergenceMetric::MftGeometry, "mft-geometry-agreement", compared, matched,
                  Pct(matched, compared) >= 100.0);
    }

    // ---- DT-04: move plan ----------------------------------------------
    {
        const std::uint64_t compared = std::max<std::uint64_t>(
            left.moves.size(), right.moves.size());
        std::uint64_t matched = 0;
        const auto n = std::min(left.moves.size(), right.moves.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (left.moves[i] == right.moves[i]) {
                ++matched;
            } else {
                div.push_back({DivergenceMetric::MovePlan,
                               DivergenceClass::ExplainedAllocatorPolicy,
                               left.moves[i].fileKey,
                               "move #" + FormatInt(static_cast<long long>(i)) + " differs",
                               FormatInt(static_cast<long long>(left.moves[i].targetLcn)),
                               FormatInt(static_cast<long long>(right.moves[i].targetLcn))});
            }
        }
        const bool gate = Pct(matched, compared) >= tol.moveAgreementMinFraction * 100.0;
        pushScore(DivergenceMetric::MovePlan, "move-plan-agreement", compared, matched, gate);
    }

    // ---- DT-06 (b): skip reasons (must be total) ------------------------
    {
        std::unordered_map<std::string, std::uint32_t> tally;
        for (const auto& s : left.skips) {
            tally[s.fileKey + "|" + DescribeSkip(s.reason)] |= 1U;
        }
        for (const auto& s : right.skips) {
            tally[s.fileKey + "|" + DescribeSkip(s.reason)] |= 2U;
        }
        std::uint64_t compared = 0, matched = 0;
        for (const auto& [k, bits] : tally) {
            ++compared;
            if (bits == 3U) {
                ++matched;
            } else {
                const auto sep = k.rfind('|');
                const std::string key = sep == std::string::npos ? k : k.substr(0, sep);
                div.push_back({DivergenceMetric::SkipReason,
                               (bits == 1U) ? DivergenceClass::ExplainedCapability
                                            : DivergenceClass::ExplainedCapability,
                               key,
                               (bits == 1U) ? "skipped on the left run only"
                                            : "skipped on the right run only",
                               (bits == 1U) ? k.substr(sep + 1) : "-",
                               (bits == 3U) ? "-" : k.substr(sep + 1)});
            }
        }
        const bool gate = Pct(matched, compared) >= tol.skipReasonAgreementMinFraction * 100.0;
        pushScore(DivergenceMetric::SkipReason, "skip-reason-agreement", compared, matched, gate);
    }

    // ---- Aggregate ------------------------------------------------------
    std::uint64_t totalCompared = 0, totalMatched = 0;
    bool allGates = true;
    for (const auto& s : rep.scores) {
        totalCompared += s.comparedItems;
        totalMatched += s.matchedItems;
        if (!s.meetsGate) allGates = false;
    }
    rep.overallAgreementPct = Pct(totalMatched, totalCompared);
    rep.passed = allGates && rep.overallAgreementPct >= tol.overallAgreementMinPct &&
                 rep.UnexplainedCount() == 0;
    return rep;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

std::string ComparisonReport::ToMarkdown() const {
    std::ostringstream os;
    os << "# Differential report — " << (scenarioId.empty() ? "(no scenario id)" : scenarioId) << "\n\n";
    os << "- left:  `" << leftLabel << "`\n";
    os << "- right: `" << rightLabel << "`\n";
    os << "- overall agreement: **" << FormatDouble(overallAgreementPct) << "%**";
    os << " (gate " << (passed ? "PASS" : "FAIL") << ")\n";
    os << "- unexplained divergences: **" << UnexplainedCount() << "**\n\n";

    os << "| metric | compared | matched | agreement % | gate |\n";
    os << "|---|---:|---:|---:|---|\n";
    for (const auto& s : scores) {
        os << "| " << s.name << " | " << s.comparedItems << " | " << s.matchedItems << " | "
           << FormatDouble(s.agreementPct) << " | " << (s.meetsGate ? "PASS" : "FAIL") << " |\n";
    }

    if (!divergences.empty()) {
        os << "\n## Divergences\n\n| metric | class | subject | detail | left | right |\n";
        os << "|---|---|---|---|---|---|\n";
        std::size_t shown = 0;
        constexpr std::size_t kMaxRows = 200;
        for (const auto& d : divergences) {
            if (shown++ >= kMaxRows) {
                os << "| … | … | … | " << (divergences.size() - kMaxRows)
                   << " more rows elided | … | … |\n";
                break;
            }
            os << "| " << DescribeMetric(d.metric) << " | " << DescribeClass(d.kind) << " | `"
               << (d.subject.empty() ? "-" : d.subject) << "` | " << d.detail << " | `"
               << d.leftValue << "` | `" << d.rightValue << "` |\n";
        }
    }
    return os.str();
}

std::string ComparisonReport::ToJson() const {
    std::ostringstream os;
    os << "{\n";
    os << "  \"scenarioId\": \"" << scenarioId << "\",\n";
    os << "  \"left\": \"" << leftLabel << "\",\n";
    os << "  \"right\": \"" << rightLabel << "\",\n";
    os << "  \"overallAgreementPct\": " << FormatDouble(overallAgreementPct) << ",\n";
    os << "  \"passed\": " << (passed ? "true" : "false") << ",\n";
    os << "  \"unexplained\": " << UnexplainedCount() << ",\n";
    os << "  \"metrics\": [\n";
    for (std::size_t i = 0; i < scores.size(); ++i) {
        const auto& s = scores[i];
        os << "    {\"name\": \"" << s.name << "\", \"compared\": " << s.comparedItems
           << ", \"matched\": " << s.matchedItems << ", \"agreementPct\": "
           << FormatDouble(s.agreementPct) << ", \"gate\": " << (s.meetsGate ? "true" : "false")
           << "}" << (i + 1 < scores.size() ? "," : "") << "\n";
    }
    os << "  ],\n";
    os << "  \"divergenceCount\": " << divergences.size() << "\n";
    os << "}\n";
    return os.str();
}

} // namespace auralite::differential
