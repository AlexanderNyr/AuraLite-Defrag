#pragma once

#include "core/config.hpp"
#include "core/types.hpp"

#include <array>
#include <cstdint>

namespace auralite {

struct RgbColor {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
};

struct ThemePalette {
    ThemeMode mode{ThemeMode::Light};
    RgbColor windowBg;
    RgbColor panelBg;
    RgbColor textPrimary;
    RgbColor textSecondary;
    RgbColor border;
    RgbColor accent;
    RgbColor warningBg;
    RgbColor warningText;
    // 9 cluster map category colors ordered by ClusterCategory (0..8)
    std::array<RgbColor, kClusterCategoryCount> clusterColors;
};

ThemePalette GetThemePalette(ThemeMode mode) noexcept;
double ComputeContrastRatio(RgbColor a, RgbColor b) noexcept;

} // namespace auralite
