#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>

namespace auralite {

namespace {
double LinearizeChannel(std::uint8_t v) noexcept {
    const double s = static_cast<double>(v) / 255.0;
    return s <= 0.03928 ? (s / 12.92) : std::pow((s + 0.055) / 1.055, 2.4);
}

double RelativeLuminance(RgbColor c) noexcept {
    return 0.2126 * LinearizeChannel(c.r) +
           0.7152 * LinearizeChannel(c.g) +
           0.0722 * LinearizeChannel(c.b);
}
} // namespace

double ComputeContrastRatio(RgbColor a, RgbColor b) noexcept {
    const double l1 = RelativeLuminance(a);
    const double l2 = RelativeLuminance(b);
    const double hi = std::max(l1, l2);
    const double lo = std::min(l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}

ThemePalette GetThemePalette(ThemeMode mode) noexcept {
    ThemePalette p{};
    p.mode = mode;
    if (mode == ThemeMode::Dark) {
        p.windowBg = {15, 23, 42};       // #0F172A
        p.panelBg = {30, 41, 59};        // #1E293B
        p.textPrimary = {241, 245, 249}; // #F1F5F9 (contrast > 13:1)
        p.textSecondary = {148, 163, 184}; // #94A3B8 (contrast > 5.5:1)
        p.border = {51, 65, 85};
        p.accent = {56, 189, 248};
        p.warningBg = {120, 53, 15};
        p.warningText = {254, 243, 199};
        p.clusterColors = {{
            {30, 41, 59},    // Free
            {56, 189, 248},  // UsedContiguous
            {239, 68, 68},   // Fragmented
            {100, 116, 139}, // UnmovableSystem
            {167, 139, 250}, // MftZone
            {124, 58, 237},  // Mft
            {71, 85, 105},   // Excluded
            {250, 204, 21},  // SelectedFile
            {16, 185, 129},  // BeingProcessed
        }};
    } else {
        p.windowBg = {248, 250, 252};    // #F8FAFC
        p.panelBg = {255, 255, 255};     // #FFFFFF
        p.textPrimary = {15, 23, 42};    // #0F172A (contrast > 15:1)
        p.textSecondary = {71, 85, 105}; // #475569 (contrast > 6.5:1)
        p.border = {203, 213, 225};
        p.accent = {2, 132, 199};
        p.warningBg = {254, 243, 199};
        p.warningText = {120, 53, 15};
        p.clusterColors = {{
            {241, 245, 249}, // Free
            {14, 165, 233},  // UsedContiguous
            {220, 38, 38},   // Fragmented
            {100, 116, 139}, // UnmovableSystem
            {196, 181, 253}, // MftZone
            {109, 40, 217},  // Mft
            {148, 163, 184}, // Excluded
            {234, 179, 8},   // SelectedFile
            {5, 150, 105},   // BeingProcessed
        }};
    }
    return p;
}

} // namespace auralite
