#pragma once
//
// P2-T06 — Differential oracle and golden traces (R-063).
//
// The oracle is the *only* place where two backends are compared. It never
// branches on backend identity: callers hand it two NormalizedRun values that
// were produced by driving whatever backend through the public interfaces
// (IVolumeProvider / IFileSystemAdapter / IDefragPlanner / IExecutionMode).
//
// Design rules encoded here (plan §8.6, S-07 contract):
//   * Comparison is *normalized*, never raw: identifiers are opaque and may
//     differ between backends (a real NTFS FileId is a 64-bit MFT reference,
//     the emulator's is a dense index), so files are matched by a canonical
//     path key, not by id.
//   * Volumes never appear in the comparison (they are the point of the test).
//   * Every metric has an explicit, documented tolerance. Numeric metrics are
//     compared with an absolute-or-relative rule; categorical metrics
//     (skip reasons, error codes) must agree exactly — DT-06 requires 100%.
//   * Divergence is classified so that a human can triage it: "explained"
//     divergences are recorded but do not fail the run; "unexplained" ones do.
//
#include "core/types.hpp"
#include "interfaces/execution_outcome.hpp"
#include "interfaces/snapshots.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace auralite::differential {

// ---------------------------------------------------------------------------
// Normalized model
// ---------------------------------------------------------------------------

// Stable, backend-independent identity for a file within one run.
// Derived from the canonical path (case-folded, separators normalized) so that
// two backends describing "the same" file produce the same key.
using PathKey = std::string;

struct NormalizedExtent {
    Vcn startVcn{0};
    Lcn startLcn{0};
    ClusterCount clusterCount{0};

    bool operator==(const NormalizedExtent&) const = default;
};

struct NormalizedFile {
    PathKey key;
    std::uint64_t sizeBytes{0};
    ClusterCount allocatedClusters{0};
    std::uint32_t fragmentCount{0};
    std::vector<NormalizedExtent> extents; // sorted by startVcn
    FileFlags flags{};
};

struct NormalizedMft {
    std::uint32_t fragmentCount{0};
    std::uint64_t totalRecords{0};
    std::uint64_t inUseRecords{0};
    Lcn mftStartLcn{0};
    Lcn zoneStartLcn{0};
    Lcn zoneEndLcn{0};
    ClusterCount totalZoneClusters{0};
    ClusterCount freeZoneClusters{0};
};

struct NormalizedMove {
    PathKey fileKey;
    Vcn startVcn{0};
    Lcn sourceLcn{0};
    Lcn targetLcn{0};
    ClusterCount clusterCount{0};

    bool operator==(const NormalizedMove&) const = default;
};

struct NormalizedSkip {
    PathKey fileKey;
    SkipReason reason{SkipReason::None};
};

struct NormalizedSummary {
    ClusterCount totalClusters{0};
    ClusterCount freeClusters{0};
    ClusterCount usedClusters{0};
    std::uint64_t totalFiles{0};
    std::uint64_t fragmentedFiles{0};
    std::uint64_t residentFiles{0};
    std::uint64_t unmovableFiles{0};
    ClusterCount largestFreeRunClusters{0};
    double volumeFragmentationPct{0.0};
    double fileFragmentationPct{0.0};
};

// One complete, comparable description of a backend run.
struct NormalizedRun {
    std::string label;          // human-readable origin ("emulator", "vhdx-01")
    std::string scenarioId;     // scenario that produced it
    NormalizedSummary summary;
    NormalizedMft mft;
    std::vector<NormalizedFile> files;   // sorted by key
    std::vector<NormalizedMove> moves;   // in execution order
    std::vector<NormalizedSkip> skips;   // sorted by (key, reason)
    std::uint64_t moveCallCount{0};      // backend-reported move invocations
    std::uint32_t errorCodeValue{0};     // non-zero when the run ended in error
};

// ---------------------------------------------------------------------------
// Normalization entry points
// ---------------------------------------------------------------------------

// Canonical, case-insensitive, separator-normalized path key.
// "C:\\Data\\A.TXT" and "c:/data/a.txt" produce the same key.
PathKey MakePathKey(std::string_view path);

// Build a NormalizedRun from what the public interfaces expose.
// `files` must come from IFileSystemAdapter::Enumerate; extents are read
// through Extents() so the oracle sees exactly what production code sees.
NormalizedRun NormalizeFromSnapshot(const AnalysisSnapshot& snapshot,
                                    std::string label);

// Normalize an executed plan (defrag side).
void AttachPlan(NormalizedRun& run, const DefragPlan& plan);
void AttachOutcome(NormalizedRun& run, const ExecutionOutcome& outcome);
void AttachPlanAndOutcome(NormalizedRun& run,
                          const DefragPlan& plan,
                          const ExecutionOutcome& outcome);

// ---------------------------------------------------------------------------
// Tolerances (S-07 / V-25)
// ---------------------------------------------------------------------------

struct Tolerances {
    // DT-01: fragmentation percentages agree within ±2 percentage points.
    double fragmentationPctAbsTolerance{2.0};
    // DT-01: extent counts agree within ±10% (relative), floor of 1 extent.
    double extentCountRelativeTolerance{0.10};
    // DT-04: fraction of plan moves that must match after normalization.
    double moveAgreementMinFraction{0.98};
    // DT-06: skip-reason agreement must be total.
    double skipReasonAgreementMinFraction{1.0};
    // Aggregate agreement gate from P2-T06 acceptance criteria.
    double overallAgreementMinPct{98.0};
    // Number of scenarios that must clear the gate (P2-T06: ≥18/20).
    std::uint32_t minScenariosMeetingGate{18};
};

// ---------------------------------------------------------------------------
// Divergence reporting
// ---------------------------------------------------------------------------

enum class DivergenceClass : std::uint8_t {
    Unexplained = 0, // fails the run
    ExplainedAllocatorPolicy, // NTFS placement policy differs by design
    ExplainedCapability,      // backend capability difference (e.g. T1 unsupported)
    ExplainedErrorPath,       // the two runs ended in different error paths
};

enum class DivergenceMetric : std::uint8_t {
    FileSet = 0,
    ExtentMap,
    FragmentCount,
    MftGeometry,
    MovePlan,
    SkipReason,
    Summary,
    ErrorCode,
};

struct Divergence {
    DivergenceMetric metric{DivergenceMetric::Summary};
    DivergenceClass kind{DivergenceClass::Unexplained};
    PathKey subject;        // file key, or empty for volume-level metrics
    std::string detail;     // human-readable explanation
    std::string leftValue;
    std::string rightValue;
};

struct MetricScore {
    DivergenceMetric metric{DivergenceMetric::Summary};
    std::string name;
    std::uint64_t comparedItems{0};
    std::uint64_t matchedItems{0};
    double agreementPct{100.0}; // 100 when comparedItems == 0
    bool meetsGate{true};
};

struct ComparisonReport {
    std::string scenarioId;
    std::string leftLabel;
    std::string rightLabel;
    std::vector<MetricScore> scores;
    std::vector<Divergence> divergences;
    double overallAgreementPct{100.0};
    bool passed{true};

    // Findings that must be triaged before the gate can pass.
    std::size_t UnexplainedCount() const noexcept;

    // Markdown triage report; suitable for `docs/differential/DT-xx.md`.
    std::string ToMarkdown() const;

    // Compact, stable machine form (one JSON object per report).
    std::string ToJson() const;
};

// Compare two normalized runs. Pure function, no I/O, deterministic output.
ComparisonReport Compare(const NormalizedRun& left,
                         const NormalizedRun& right,
                         const Tolerances& tolerances = {});

// ---------------------------------------------------------------------------
// Golden traces
// ---------------------------------------------------------------------------

// A golden trace is the frozen, canonical serialization of a NormalizedRun.
// It is the regression corpus artifact (R-064): if a change alters observable
// behaviour, the trace diff shows exactly where.
namespace golden {

inline constexpr std::uint32_t kTraceFormatVersion = 1;

// Deterministic serialization: files sorted by key, moves in order,
// skips sorted, no timestamps, no absolute paths, no host identifiers.
std::string Serialize(const NormalizedRun& run);

// Parse a trace back. Rejects unknown format versions.
Result<NormalizedRun> Parse(std::string_view text);

// Path convention: tests/golden/<scenarioId>.trace.json
std::string TraceFileName(std::string_view scenarioId);

} // namespace golden

} // namespace auralite::differential
