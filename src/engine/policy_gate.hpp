#pragma once

#include "core/config.hpp"
#include "core/result.hpp"
#include "core/types.hpp"
#include "interfaces/snapshots.hpp"

#include <string_view>
#include <vector>

namespace auralite {

class PolicyGate {
public:
    explicit PolicyGate(AppConfig config = {});

    void UpdateConfig(AppConfig config);
    const AppConfig& Config() const noexcept { return config_; }

    // Pre-flight & media gating (no SSD override exists anywhere in v1)
    Result<void> CanWriteToVolume(const PreflightResult& preflight,
                                  DefragMode mode,
                                  bool isElevated,
                                  bool isSimulation) const;

    // Exclusion matching (wildcards *, ?, prefix, size bounds)
    bool IsFileExcluded(const FileEntry& file) const;

    // System & unsafe file protection (R-080)
    SkipReason ClassifySkipReason(const FileEntry& file,
                                  const CapabilityBits& caps,
                                  bool allowMftTier1Direct = false) const;

    static bool GlobMatchCaseInsensitive(std::string_view pattern, std::string_view text) noexcept;

private:
    AppConfig config_;
};

} // namespace auralite
