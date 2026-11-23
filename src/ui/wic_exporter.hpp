#pragma once

#include "core/result.hpp"
#include "reporting/cluster_aggregator.hpp"
#include "ui/theme.hpp"

#include <filesystem>

namespace auralite {

class WicPngExporter {
public:
    static Result<void> ExportClusterMapPng(const std::filesystem::path& targetPath,
                                            const ClusterGrid& grid,
                                            const ThemePalette& palette,
                                            std::uint32_t widthPx,
                                            std::uint32_t heightPx,
                                            double dpi = 96.0);
};

} // namespace auralite
