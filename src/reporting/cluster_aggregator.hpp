#pragma once

#include "core/types.hpp"
#include "interfaces/snapshots.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace auralite {

struct ClusterCell {
    Lcn startLcn{0};
    Lcn endLcn{0}; // exclusive
    ClusterCount totalClusters{0};
    std::array<ClusterCount, kClusterCategoryCount> counts{};
    ClusterCategory dominant{ClusterCategory::Free};
    FileId dominantFileId{kInvalidFileId};
    bool belongsToMftZone{false};
};

struct ClusterGrid {
    std::uint32_t cols{200};
    std::uint32_t rows{100};
    ClusterCount totalClusters{0};
    ClusterCount clustersPerCell{1};
    std::vector<ClusterCell> cells;

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

    static HitTestInfo HitTest(const ClusterGrid& grid,
                               const AnalysisSnapshot& snapshot,
                               std::uint32_t col,
                               std::uint32_t row);
};

} // namespace auralite
