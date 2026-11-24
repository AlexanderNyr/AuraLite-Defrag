#pragma once

#include "core/types.hpp"
#include "interfaces/snapshots.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace auralite {

// What a hit test reports about the file a cell belongs to. Stored once per
// distinct file in ClusterGrid::hitFiles; cells reference it by index, so
// copying a grid stays cheap and no cell carries a std::string.
struct FileHitInfo {
    FileId id{kInvalidFileId};
    std::string name;
    std::uint32_t fragmentCount{0};
};

constexpr std::uint32_t kInvalidHitInfo = 0xFFFFFFFFU;

struct ClusterCell {
    Lcn startLcn{0};
    Lcn endLcn{0}; // exclusive
    ClusterCount totalClusters{0};
    std::array<ClusterCount, kClusterCategoryCount> counts{};
    ClusterCategory dominant{ClusterCategory::Free};
    FileId dominantFileId{kInvalidFileId};
    std::uint32_t hitInfo{kInvalidHitInfo}; // index into ClusterGrid::hitFiles
    bool belongsToMftZone{false};
};

struct ClusterGrid {
    std::uint32_t cols{200};
    std::uint32_t rows{100};
    ClusterCount totalClusters{0};
    ClusterCount clustersPerCell{1};
    std::vector<ClusterCell> cells;

    // Hit-test data for the files that are dominant somewhere on the map.
    // Filled while the grid is painted, so no second pass over the file set is
    // needed; bounded by cells (<= 20,000), not by the 250,000 files on the
    // volume. The grid owns the strings: an earlier version held pointers into
    // the caller's snapshot and dangled as soon as that snapshot was replaced.
    std::vector<FileHitInfo> hitFiles;

    const ClusterCell* CellAt(std::uint32_t col, std::uint32_t row) const noexcept {
        if (col >= cols || row >= rows) return nullptr;
        const std::size_t idx = static_cast<std::size_t>(row) * cols + col;
        return idx < cells.size() ? &cells[idx] : nullptr;
    }
};

struct HitTestInfo {
    bool valid{false};
    std::uint32_t col{0};
    std::uint32_t row{0};
    Lcn startLcn{0};
    Lcn endLcn{0};
    ClusterCategory dominant{ClusterCategory::Free};
    FileId dominantFileId{kInvalidFileId};
    std::string dominantFileName;
    std::uint32_t dominantFragmentCount{0};
    bool belongsToMftZone{false};
};

struct IncrementalExtentUpdate {
    Lcn oldStartLcn{0};
    Lcn newStartLcn{0};
    ClusterCount clusterCount{0};
    ClusterCategory category{ClusterCategory::UsedContiguous};
};

class ClusterAggregator {
public:
    static ClusterCategory ResolveDominantCategory(
        const std::array<ClusterCount, kClusterCategoryCount>& counts) noexcept;

    static ClusterGrid BuildGrid(const AnalysisSnapshot& snapshot,
                                 std::uint32_t cols = 200,
                                 std::uint32_t rows = 100,
                                 FileId selectedFileId = kInvalidFileId,
                                 FileId activeFileId = kInvalidFileId);

    static void ApplyIncrementalUpdates(ClusterGrid& grid,
                                        const std::vector<IncrementalExtentUpdate>& updates);

    // One pixel per cell, 0xAARRGGBB, laid out row-major. The renderer uploads
    // this as a single bitmap and scales it, instead of issuing one FillRectangle
    // per cell: at 4K that was 90,000 draw calls per frame, which no amount of
    // aggregation tuning can hide. Kept here (L5) rather than in the UI so it can
    // be tested without Direct2D.
    static std::vector<std::uint32_t> BuildPixelBuffer(
        const ClusterGrid& grid,
        const std::array<std::uint32_t, kClusterCategoryCount>& categoryAbgr,
        std::uint32_t backgroundAbgr = 0xFF000000U);

    static HitTestInfo HitTest(const ClusterGrid& grid,
                               const AnalysisSnapshot& snapshot,
                               std::uint32_t col,
                               std::uint32_t row);
};

} // namespace auralite
