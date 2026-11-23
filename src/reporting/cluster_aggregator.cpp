#include "reporting/cluster_aggregator.hpp"

#include <algorithm>

namespace auralite {

namespace {

ClusterCategory ClassifyFileCategory(const FileEntry& f,
                                     FileId selectedFileId,
                                     FileId activeFileId) noexcept {
    if (f.id == activeFileId) return ClusterCategory::BeingProcessed;
    if (f.id == selectedFileId) return ClusterCategory::SelectedFile;
    if (f.flags.isExcluded || f.state == FileState::Excluded) return ClusterCategory::Excluded;
    if (f.id == kMftFileId || f.id == kMftMirrFileId) return ClusterCategory::Mft;
    if (f.flags.isMetadata || f.flags.isPageOrHiber || f.flags.isLocked) return ClusterCategory::UnmovableSystem;
    if (f.fragmentCount > 1) return ClusterCategory::Fragmented;
    return ClusterCategory::UsedContiguous;
}

void PaintRange(ClusterGrid& grid,
                Lcn rangeStart,
                ClusterCount count,
                ClusterCategory cat,
                FileId fileId,
                bool replaceFromFree) {
    if (count == 0 || grid.clustersPerCell == 0 || grid.cells.empty()) return;
    const Lcn rangeEnd = std::min(rangeStart + count, grid.totalClusters);
    if (rangeStart >= rangeEnd) return;

    const std::size_t firstCell = static_cast<std::size_t>(rangeStart / grid.clustersPerCell);
    const std::size_t lastCell = static_cast<std::size_t>((rangeEnd - 1) / grid.clustersPerCell);
    const std::size_t maxCell = grid.cells.size() - 1;

    for (std::size_t ci = firstCell; ci <= std::min(lastCell, maxCell); ++ci) {
        auto& cell = grid.cells[ci];
        const Lcn ovStart = std::max(rangeStart, cell.startLcn);
        const Lcn ovEnd = std::min(rangeEnd, cell.endLcn);
        if (ovEnd <= ovStart) continue;
        const ClusterCount ovLen = ovEnd - ovStart;

        if (replaceFromFree) {
            const ClusterCount take = std::min(cell.counts[static_cast<std::size_t>(ClusterCategory::Free)], ovLen);
            cell.counts[static_cast<std::size_t>(ClusterCategory::Free)] -= take;
            cell.counts[static_cast<std::size_t>(cat)] += take;
        } else {
            cell.counts[static_cast<std::size_t>(cat)] += ovLen;
        }
        if (fileId != kInvalidFileId && cell.dominantFileId == kInvalidFileId) {
            cell.dominantFileId = fileId;
        }
    }
}

} // namespace

ClusterCategory ClusterAggregator::ResolveDominantCategory(
    const std::array<ClusterCount, kClusterCategoryCount>& counts) noexcept {
    // Deterministic priority highest -> lowest (§7.2):
    // BeingProcessed(8) > SelectedFile(7) > Excluded(6) > Mft(5) > MftZone(4) >
    // UnmovableSystem(3) > Fragmented(2) > UsedContiguous(1) > Free(0)
    for (int c = static_cast<int>(kClusterCategoryCount) - 1; c >= 0; --c) {
        if (counts[static_cast<std::size_t>(c)] > 0) {
            return static_cast<ClusterCategory>(c);
        }
    }
    return ClusterCategory::Free;
}

ClusterGrid ClusterAggregator::BuildGrid(const AnalysisSnapshot& snapshot,
                                         std::uint32_t cols,
                                         std::uint32_t rows,
                                         FileId selectedFileId,
                                         FileId activeFileId) {
    ClusterGrid grid{};
    grid.cols = std::clamp(cols, 1U, 400U);
    grid.rows = std::clamp(rows, 1U, 200U);
    grid.totalClusters = snapshot.geometry.totalClusters;

    const std::size_t cellCount = static_cast<std::size_t>(grid.cols) * grid.rows;
    if (grid.totalClusters == 0 || cellCount == 0) {
        return grid;
    }

    grid.clustersPerCell = (grid.totalClusters + cellCount - 1ULL) / cellCount;
    if (grid.clustersPerCell == 0) grid.clustersPerCell = 1;

    grid.cells.resize(cellCount);
    for (std::size_t i = 0; i < cellCount; ++i) {
        auto& cell = grid.cells[i];
        const Lcn s = static_cast<Lcn>(i) * grid.clustersPerCell;
        if (s >= grid.totalClusters) {
            cell.startLcn = grid.totalClusters;
            cell.endLcn = grid.totalClusters;
            cell.totalClusters = 0;
            continue;
        }
        const Lcn e = std::min(grid.totalClusters, s + grid.clustersPerCell);
        cell.startLcn = s;
        cell.endLcn = e;
        cell.totalClusters = e - s;
        cell.counts[static_cast<std::size_t>(ClusterCategory::Free)] = cell.totalClusters;

        if (snapshot.mftZone.zoneEndLcn > snapshot.mftZone.zoneStartLcn &&
            !(e <= snapshot.mftZone.zoneStartLcn || s >= snapshot.mftZone.zoneEndLcn)) {
            cell.belongsToMftZone = true;
        }
    }

    // 1. Mark MFT Zone free space first (only free clusters inside MFT Zone get MftZone count;
    //    occupied user clusters inside the zone will overwrite or stay counted cleanly)
    if (snapshot.mftZone.zoneEndLcn > snapshot.mftZone.zoneStartLcn) {
        const Lcn zs = snapshot.mftZone.zoneStartLcn;
        const Lcn ze = std::min(snapshot.mftZone.zoneEndLcn, grid.totalClusters);
        if (ze > zs) {
            PaintRange(grid, zs, ze - zs, ClusterCategory::MftZone, kInvalidFileId, true);
        }
    }

    // 2. Paint every file's extents (subtracting from Free first, then MftZone if overlapping)
    for (const auto& f : snapshot.files) {
        if (f.flags.resident || f.extents.empty()) continue;
        const ClusterCategory cat = ClassifyFileCategory(f, selectedFileId, activeFileId);
        for (const auto& ext : f.extents) {
            const Lcn rStart = ext.startLcn;
            const Lcn rEnd = std::min(rStart + ext.clusterCount, grid.totalClusters);
            if (rStart >= rEnd) continue;

            const std::size_t firstCell = static_cast<std::size_t>(rStart / grid.clustersPerCell);
            const std::size_t lastCell = static_cast<std::size_t>((rEnd - 1) / grid.clustersPerCell);
            for (std::size_t ci = firstCell; ci <= std::min(lastCell, grid.cells.size() - 1); ++ci) {
                auto& cell = grid.cells[ci];
                const Lcn ovStart = std::max(rStart, cell.startLcn);
                const Lcn ovEnd = std::min(rEnd, cell.endLcn);
                if (ovEnd <= ovStart) continue;
                ClusterCount rem = ovEnd - ovStart;

                const ClusterCount fromFree = std::min(cell.counts[static_cast<std::size_t>(ClusterCategory::Free)], rem);
                cell.counts[static_cast<std::size_t>(ClusterCategory::Free)] -= fromFree;
                rem -= fromFree;

                if (rem > 0) {
                    const ClusterCount fromZone = std::min(cell.counts[static_cast<std::size_t>(ClusterCategory::MftZone)], rem);
                    cell.counts[static_cast<std::size_t>(ClusterCategory::MftZone)] -= fromZone;
                    rem -= fromZone;
                }

                const ClusterCount placed = (ovEnd - ovStart) - rem;
                cell.counts[static_cast<std::size_t>(cat)] += placed;
                if (cell.dominantFileId == kInvalidFileId || cat >= cell.dominant) {
                    cell.dominantFileId = f.id;
                }
            }
        }
    }

    // 3. Reconcile any bitmap-allocated clusters not covered by enumerated file extents
    if (snapshot.bitmap.totalClusters > 0) {
        for (std::size_t ci = 0; ci < grid.cells.size(); ++ci) {
            auto& cell = grid.cells[ci];
            if (cell.totalClusters == 0) continue;
            // Fast path: check sum of non-free categories vs cell total
            ClusterCount accountedUsed = 0;
            for (std::size_t k = 1; k < kClusterCategoryCount; ++k) {
                if (k != static_cast<std::size_t>(ClusterCategory::MftZone)) {
                    accountedUsed += cell.counts[k];
                }
            }
            if (accountedUsed == 0 && cell.counts[static_cast<std::size_t>(ClusterCategory::Free)] > 0) {
                // Sample bitmap for clusters in this cell if cell had no file extents
                ClusterCount bmpUsed = 0;
                for (Lcn l = cell.startLcn; l < cell.endLcn; ++l) {
                    if (snapshot.bitmap.IsAllocated(l)) ++bmpUsed;
                }
                if (bmpUsed > 0) {
                    const ClusterCount fromFree = std::min(cell.counts[static_cast<std::size_t>(ClusterCategory::Free)], bmpUsed);
                    cell.counts[static_cast<std::size_t>(ClusterCategory::Free)] -= fromFree;
                    ClusterCount rem = bmpUsed - fromFree;
                    if (rem > 0) {
                        const ClusterCount fromZone = std::min(cell.counts[static_cast<std::size_t>(ClusterCategory::MftZone)], rem);
                        cell.counts[static_cast<std::size_t>(ClusterCategory::MftZone)] -= fromZone;
                        rem -= fromZone;
                    }
                    cell.counts[static_cast<std::size_t>(ClusterCategory::UsedContiguous)] += (bmpUsed - rem);
                }
            }
            cell.dominant = ResolveDominantCategory(cell.counts);
        }
    } else {
        for (auto& cell : grid.cells) {
            cell.dominant = ResolveDominantCategory(cell.counts);
        }
    }

    return grid;
}

void ClusterAggregator::ApplyIncrementalUpdates(ClusterGrid& grid,
                                                const std::vector<IncrementalExtentUpdate>& updates) {
    if (grid.clustersPerCell == 0 || grid.cells.empty()) return;
    const std::size_t maxCell = grid.cells.size() - 1;

    for (const auto& u : updates) {
        if (u.clusterCount == 0) continue;

        // Clear old range -> Free (or MftZone if cell belongsToMftZone and old range was inside)
        {
            const Lcn s = u.oldStartLcn;
            const Lcn e = std::min(s + u.clusterCount, grid.totalClusters);
            if (s < e) {
                const std::size_t fc = static_cast<std::size_t>(s / grid.clustersPerCell);
                const std::size_t lc = static_cast<std::size_t>((e - 1) / grid.clustersPerCell);
                for (std::size_t ci = fc; ci <= std::min(lc, maxCell); ++ci) {
                    auto& cell = grid.cells[ci];
                    const Lcn ovS = std::max(s, cell.startLcn);
                    const Lcn ovE = std::min(e, cell.endLcn);
                    if (ovE <= ovS) continue;
                    ClusterCount rem = ovE - ovS;
                    for (std::size_t k = kClusterCategoryCount - 1; k >= 1 && rem > 0; --k) {
                        if (k == static_cast<std::size_t>(ClusterCategory::MftZone)) continue;
                        const ClusterCount take = std::min(cell.counts[k], rem);
                        cell.counts[k] -= take;
                        rem -= take;
                    }
                    const ClusterCount freed = (ovE - ovS) - rem;
                    cell.counts[static_cast<std::size_t>(ClusterCategory::Free)] += freed;
                    cell.dominant = ResolveDominantCategory(cell.counts);
                }
            }
        }

        // Paint new range
        {
            const Lcn s = u.newStartLcn;
            const Lcn e = std::min(s + u.clusterCount, grid.totalClusters);
            if (s < e) {
                const std::size_t fc = static_cast<std::size_t>(s / grid.clustersPerCell);
                const std::size_t lc = static_cast<std::size_t>((e - 1) / grid.clustersPerCell);
                for (std::size_t ci = fc; ci <= std::min(lc, maxCell); ++ci) {
                    auto& cell = grid.cells[ci];
                    const Lcn ovS = std::max(s, cell.startLcn);
                    const Lcn ovE = std::min(e, cell.endLcn);
                    if (ovE <= ovS) continue;
                    ClusterCount rem = ovE - ovS;
                    const ClusterCount fromFree = std::min(cell.counts[static_cast<std::size_t>(ClusterCategory::Free)], rem);
                    cell.counts[static_cast<std::size_t>(ClusterCategory::Free)] -= fromFree;
                    rem -= fromFree;
                    if (rem > 0) {
                        const ClusterCount fromZone = std::min(cell.counts[static_cast<std::size_t>(ClusterCategory::MftZone)], rem);
                        cell.counts[static_cast<std::size_t>(ClusterCategory::MftZone)] -= fromZone;
                        rem -= fromZone;
                    }
                    cell.counts[static_cast<std::size_t>(u.category)] += ((ovE - ovS) - rem);
                    cell.dominant = ResolveDominantCategory(cell.counts);
                }
            }
        }
    }
}

HitTestInfo ClusterAggregator::HitTest(const ClusterGrid& grid,
                                       const AnalysisSnapshot& snapshot,
                                       std::uint32_t col,
                                       std::uint32_t row) {
    HitTestInfo info{};
    const ClusterCell* cell = grid.CellAt(col, row);
    if (!cell || cell->totalClusters == 0) return info;

    info.valid = true;
    info.col = col;
    info.row = row;
    info.startLcn = cell->startLcn;
    info.endLcn = cell->endLcn;
    info.dominant = cell->dominant;
    info.dominantFileId = cell->dominantFileId;
    info.belongsToMftZone = cell->belongsToMftZone;

    if (cell->dominantFileId != kInvalidFileId) {
        for (const auto& f : snapshot.files) {
            if (f.id == cell->dominantFileId) {
                info.dominantFileName = f.name;
                info.dominantFragmentCount = f.fragmentCount;
                break;
            }
        }
    }
    return info;
}

} // namespace auralite
