#include "engine/planner.hpp"
#include "core/error_catalog.hpp"
#include "core/exclusions.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_set>

namespace auralite {

namespace {

struct FreeRun {
    Lcn start{0};
    ClusterCount count{0};
};

Lcn AlignUp(Lcn val, std::uint32_t align) noexcept {
    if (align <= 1) return val;
    const std::uint64_t rem = val % align;
    return rem == 0 ? val : val + (align - rem);
}

std::vector<FreeRun> ExtractFreeRuns(const BitmapSnapshot& bmp) {
    std::vector<FreeRun> runs;
    Lcn curStart = 0;
    ClusterCount curLen = 0;
    for (ClusterCount i = 0; i < bmp.totalClusters; ++i) {
        const Lcn lcn = bmp.startLcn + i;
        if (!bmp.IsAllocated(lcn)) {
            if (curLen == 0) curStart = lcn;
            ++curLen;
        } else if (curLen > 0) {
            runs.push_back({curStart, curLen});
            curLen = 0;
        }
    }
    if (curLen > 0) {
        runs.push_back({curStart, curLen});
    }
    return runs;
}

bool OverlapsZone(Lcn start, ClusterCount count, Lcn zoneStart, Lcn zoneEnd) noexcept {
    if (zoneEnd <= zoneStart || count == 0) return false;
    const Lcn end = start + count;
    return !(end <= zoneStart || start >= zoneEnd);
}

Lcn AllocateFromRuns(std::vector<FreeRun>& runs,
                     ClusterCount needed,
                     std::uint32_t alignment,
                     Lcn avoidStart,
                     Lcn avoidEnd,
                     Lcn minStartLcn,
                     bool preferHighest) {
    if (needed == 0) return kInvalidLcn;

    auto tryPick = [&](std::size_t idx, bool allowZoneOverlap) -> Lcn {
        auto& r = runs[idx];
        Lcn effectiveStart = std::max(r.start, minStartLcn);
        Lcn aligned = AlignUp(effectiveStart, alignment);
        if (aligned < r.start || aligned >= r.start + r.count) return kInvalidLcn;
        ClusterCount avail = (r.start + r.count) - aligned;
        if (avail < needed) return kInvalidLcn;

        if (!allowZoneOverlap && OverlapsZone(aligned, needed, avoidStart, avoidEnd)) {
            // Try after avoidEnd within same run
            if (r.start + r.count > avoidEnd) {
                Lcn afterZone = AlignUp(std::max(avoidEnd, minStartLcn), alignment);
                if (afterZone >= r.start && afterZone + needed <= r.start + r.count) {
                    aligned = afterZone;
                } else {
                    return kInvalidLcn;
                }
            } else {
                return kInvalidLcn;
            }
        }

        const Lcn chosen = aligned;
        const Lcn origEnd = r.start + r.count;
        const Lcn chosenEnd = chosen + needed;
        if (chosen == r.start && chosenEnd == origEnd) {
            runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(idx));
        } else if (chosen == r.start) {
            r.start = chosenEnd;
            r.count = origEnd - chosenEnd;
        } else if (chosenEnd == origEnd) {
            r.count = chosen - r.start;
        } else {
            r.count = chosen - r.start;
            runs.insert(runs.begin() + static_cast<std::ptrdiff_t>(idx) + 1, FreeRun{chosenEnd, origEnd - chosenEnd});
        }
        return chosen;
    };

    if (preferHighest) {
        for (std::size_t i = runs.size(); i > 0; --i) {
            Lcn lcn = tryPick(i - 1, false);
            if (lcn != kInvalidLcn) return lcn;
        }
        return kInvalidLcn;
    }

    // Best-fit / first-fit outside zone
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (runs[i].count >= needed) {
            Lcn lcn = tryPick(i, false);
            if (lcn != kInvalidLcn) return lcn;
        }
    }
    return kInvalidLcn;
}

void EmitFileMovesToTarget(const FileEntry& f,
                           Lcn targetBaseLcn,
                           std::uint32_t maxChunkClusters,
                           bool isMftMove,
                           DefragPlan& plan) {
    Vcn curVcn = 0;
    Lcn curTarget = targetBaseLcn;
    for (const auto& ext : f.extents) {
        ClusterCount rem = ext.clusterCount;
        Vcn extVcn = ext.startVcn;
        Lcn extSrc = ext.startLcn;
        while (rem > 0) {
            const std::uint32_t chunk = static_cast<std::uint32_t>(
                std::min<ClusterCount>(rem, maxChunkClusters == 0 ? 65536U : maxChunkClusters));
            PlannedMove m{};
            m.stepIndex = plan.moves.size();
            m.fileId = f.id;
            m.fileName = f.name;
            m.startVcn = extVcn;
            m.sourceLcn = extSrc;
            m.targetLcn = curTarget;
            m.clusterCount = chunk;
            m.isMftMove = isMftMove;
            m.expectedChecksum = f.contentChecksum;
            plan.moves.push_back(std::move(m));
            plan.totalClustersToMove += chunk;

            extVcn += chunk;
            extSrc += chunk;
            curTarget += chunk;
            curVcn += chunk;
            rem -= chunk;
        }
    }
    (void)curVcn;
}

} // namespace

std::uint64_t DefragPlanner::ComputePlanHash(const DefragPlan& plan) noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    auto mix = [&](std::uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            h ^= (v & 0xFFU);
            h *= 1099511628211ULL;
            v >>= 8;
        }
    };
    mix(plan.volumeSerialNumber);
    mix(static_cast<std::uint64_t>(plan.mode));
    mix(static_cast<std::uint64_t>(plan.moves.size()));
    for (const auto& m : plan.moves) {
        mix(m.stepIndex);
        mix(m.fileId);
        mix(m.startVcn);
        mix(m.targetLcn);
        mix(m.clusterCount);
    }
    return h;
}

Result<DefragPlan> DefragPlanner::Plan(const AnalysisSnapshot& snapshot,
                                       const PlanOptions& options,
                                       const PolicyGate& policy) {
    DefragPlan plan{};
    plan.volumeId = snapshot.volumeId;
    plan.volumeSerialNumber = snapshot.geometry.volumeSerialNumber;
    plan.mode = options.mode;
    plan.dryRun = options.dryRun;

    char idBuf[48]{};
    std::snprintf(idBuf, sizeof(idBuf), "plan-%llx-%llu",
                  static_cast<unsigned long long>(snapshot.geometry.volumeSerialNumber),
                  static_cast<unsigned long long>(snapshot.snapshotGeneration));
    plan.planId = idBuf;

    if (options.mode == DefragMode::Analyze) {
        plan.planHash = ComputePlanHash(plan);
        return Result<DefragPlan>::Success(std::move(plan));
    }

    // Tier 3 MFT report-only yields 0 moves explicitly
    if (options.mode == DefragMode::MftOptimize && options.mftTier == MftTier::Tier3ReportOnly) {
        plan.planHash = ComputePlanHash(plan);
        return Result<DefragPlan>::Success(std::move(plan));
    }

    auto freeRuns = ExtractFreeRuns(snapshot.bitmap);
    const Lcn zoneStart = snapshot.mftZone.zoneStartLcn;
    const Lcn zoneEnd = snapshot.mftZone.zoneEndLcn;
    const Lcn totalClusters = snapshot.geometry.totalClusters;

    // Sort candidate file indices deterministically by FileId
    std::vector<const FileEntry*> candidates;
    candidates.reserve(snapshot.files.size());
    for (const auto& f : snapshot.files) {
        candidates.push_back(&f);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const FileEntry* a, const FileEntry* b) { return a->id < b->id; });

    std::unordered_set<FileId> targetSet(options.targetFileIds.begin(), options.targetFileIds.end());

    const bool allowMftTier1 = (options.mode == DefragMode::MftOptimize &&
                                options.mftTier == MftTier::Tier1Direct &&
                                options.mftConfirmed &&
                                snapshot.capabilities.supportsDirectMftMove);

    // Move budget for QuickDefrag (caps at ~25% of moves/clusters where appropriate)
    ClusterCount quickBudget = std::max<ClusterCount>(1024, snapshot.summary.usedClusters / 4);
    ClusterCount quickUsed = 0;

    // For DefragFreeSpace, avoid eating into the largest existing free run so it only grows
    ClusterCount largestInitialRun = 0;
    if (options.mode == DefragMode::DefragFreeSpace) {
        for (const auto& r : freeRuns) {
            if (r.count > largestInitialRun) largestInitialRun = r.count;
        }
    }

    for (const FileEntry* f : candidates) {
        if (f->flags.resident || f->extents.empty() || f->allocatedClusters == 0) {
            continue;
        }

        const SkipReason skip = policy.ClassifySkipReason(*f, snapshot.capabilities, allowMftTier1);
        if (skip != SkipReason::None) {
            plan.skipped.push_back({f->id, f->name, skip});
            continue;
        }

        const std::uint32_t align = (f->flags.compressed || f->flags.sparse)
                                        ? std::max<std::uint32_t>(16U, snapshot.capabilities.sparseMoveGranularityClusters)
                                        : std::max<std::uint32_t>(1U, snapshot.capabilities.moveGranularityClusters);

        bool selectFile = false;
        Lcn minTargetLcn = 0;
        bool preferHighest = false;
        bool isMftMove = false;

        switch (options.mode) {
            case DefragMode::Analyze:
                break;

            case DefragMode::FullDefrag:
                selectFile = (f->fragmentCount > 1);
                break;

            case DefragMode::QuickDefrag:
                if (f->fragmentCount >= options.quickMinFragments &&
                    f->sizeBytes >= options.quickMinSizeBytes &&
                    quickUsed + f->allocatedClusters <= quickBudget) {
                    selectFile = true;
                }
                break;

            case DefragMode::DefragFreeSpace: {
                // Consolidate files from scattered high/fragmented regions into lowest free runs
                const Lcn firstLcn = f->extents.front().startLcn;
                if (!freeRuns.empty() && firstLcn > freeRuns.front().start &&
                    (f->fragmentCount > 1 || firstLcn > totalClusters / 4)) {
                    selectFile = true;
                }
                break;
            }

            case DefragMode::MoveLargeToEnd: {
                if (f->sizeBytes >= options.largeFileThresholdBytes) {
                    const Lcn top10Start = (totalClusters * 9ULL) / 10ULL;
                    const bool alreadyInTop10 = !f->extents.empty() && f->extents.front().startLcn >= top10Start && f->fragmentCount == 1;
                    if (!alreadyInTop10) {
                        selectFile = true;
                        minTargetLcn = top10Start;
                        preferHighest = true;
                    }
                }
                break;
            }

            case DefragMode::SingleFileOrFolder:
                if (targetSet.count(f->id) > 0 ||
                    (!options.targetFolderPrefix.empty() &&
                     ExclusionEngine::IsWithinScope(options.targetFolderPrefix, f->path))) {
                    if (f->fragmentCount > 1) selectFile = true;
                }
                break;

            case DefragMode::MftOptimize:
                if (options.mftTier == MftTier::Tier1Direct) {
                    if (f->id == kMftFileId && f->fragmentCount > 1) {
                        selectFile = true;
                        isMftMove = true;
                    }
                } else if (options.mftTier == MftTier::Tier2Indirect) {
                    // Evacuate regular user files adjacent to MFT / inside MFT Zone
                    const Lcn adjStart = std::min(snapshot.mftInfo.mftStartLcn, zoneStart);
                    const Lcn adjEnd = zoneEnd + std::max<ClusterCount>(256, snapshot.mftZone.totalZoneClusters / 4);
                    for (const auto& ext : f->extents) {
                        if (OverlapsZone(ext.startLcn, ext.clusterCount, adjStart, adjEnd)) {
                            selectFile = true;
                            minTargetLcn = adjEnd;
                            break;
                        }
                    }
                }
                break;

            case DefragMode::MftZoneEvacuate:
                for (const auto& ext : f->extents) {
                    if (OverlapsZone(ext.startLcn, ext.clusterCount, zoneStart, zoneEnd)) {
                        selectFile = true;
                        minTargetLcn = zoneEnd;
                        break;
                    }
                }
                break;
        }

        if (!selectFile) continue;

        Lcn avoidS = isMftMove ? 0 : zoneStart;
        Lcn avoidE = isMftMove ? 0 : zoneEnd;
        if (options.mode == DefragMode::MftOptimize && options.mftTier == MftTier::Tier2Indirect) {
            avoidS = std::min(snapshot.mftInfo.mftStartLcn, zoneStart);
            avoidE = zoneEnd + std::max<ClusterCount>(256, snapshot.mftZone.totalZoneClusters / 4);
        }

        Lcn target = kInvalidLcn;
        if (options.mode == DefragMode::DefragFreeSpace) {
            // Only pack into free runs that do not overlap MFT Zone and are not the global largest free run
            for (const auto& r : freeRuns) {
                if (r.count >= f->allocatedClusters && r.count < largestInitialRun &&
                    !OverlapsZone(r.start, r.count, avoidS, avoidE) &&
                    !f->extents.empty() && r.start < f->extents.front().startLcn) {
                    target = AllocateFromRuns(freeRuns, f->allocatedClusters, align,
                                              avoidS, avoidE, r.start, false);
                    break;
                }
            }
            if (target == kInvalidLcn) {
                continue;
            }
        } else {
            target = AllocateFromRuns(freeRuns, f->allocatedClusters, align,
                                      avoidS, avoidE, minTargetLcn, preferHighest);
        }
        if (target == kInvalidLcn && options.mode == DefragMode::MoveLargeToEnd) {
            // Fall back to top 20% if top 10% doesn't have a single contiguous run large enough
            target = AllocateFromRuns(freeRuns, f->allocatedClusters, align,
                                      avoidS, avoidE, (totalClusters * 8ULL) / 10ULL, true);
        }
        if (target == kInvalidLcn) {
            plan.skipped.push_back({f->id, f->name, SkipReason::InsufficientContiguousSpace});
            continue;
        }

        EmitFileMovesToTarget(*f, target, snapshot.capabilities.maxMoveClustersPerCall, isMftMove, plan);
        if (options.mode == DefragMode::QuickDefrag) {
            quickUsed += f->allocatedClusters;
        }
    }

    plan.estimatedBytesToMove = plan.totalClustersToMove * snapshot.geometry.bytesPerCluster;
    plan.planHash = ComputePlanHash(plan);
    return Result<DefragPlan>::Success(std::move(plan));
}

Result<DefragPlan> DefragPlanner::Replan(const AnalysisSnapshot& snapshot,
                                         const DefragPlan& originalPlan,
                                         const Checkpoint& checkpoint,
                                         const PolicyGate& policy) {
    (void)snapshot;
    (void)policy;
    if (originalPlan.volumeSerialNumber != checkpoint.volumeSerial) {
        return Result<DefragPlan>::Failure(MakeError(ErrorCode::E_JOURNAL_SERIAL_MISMATCH));
    }
    if (originalPlan.planHash != checkpoint.planHash) {
        return Result<DefragPlan>::Failure(MakeError(ErrorCode::E_JOURNAL_PLAN_HASH_MISMATCH));
    }

    DefragPlan suffix = originalPlan;
    suffix.moves.clear();
    suffix.totalClustersToMove = 0;
    for (const auto& m : originalPlan.moves) {
        if (m.stepIndex > checkpoint.lastCompletedStep ||
            (checkpoint.lastCompletedStep == 0 && checkpoint.lastSequence <= 1)) {
            suffix.moves.push_back(m);
            suffix.totalClustersToMove += m.clusterCount;
        }
    }
    suffix.estimatedBytesToMove = suffix.totalClustersToMove * snapshot.geometry.bytesPerCluster;
    return Result<DefragPlan>::Success(std::move(suffix));
}

} // namespace auralite
