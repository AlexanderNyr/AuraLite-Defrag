#include "engine/policy_gate.hpp"
#include "core/error_catalog.hpp"

#include <algorithm>
#include <cctype>

namespace auralite {

namespace {
char ToLowerAscii(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c;
}

bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) return false;
    }
    return true;
}

bool IsProtectedSystemFileName(std::string_view name) noexcept {
    return IEquals(name, "pagefile.sys") ||
           IEquals(name, "hiberfil.sys") ||
           IEquals(name, "swapfile.sys");
}
} // namespace

PolicyGate::PolicyGate(AppConfig config) : config_(std::move(config)) {}

void PolicyGate::UpdateConfig(AppConfig config) {
    config_ = std::move(config);
}

Result<void> PolicyGate::CanWriteToVolume(const PreflightResult& preflight,
                                          DefragMode mode,
                                          bool isElevated,
                                          bool isSimulation) const {
    if (mode == DefragMode::Analyze) {
        return Result<void>::Success();
    }

    // Rule 1: SSD / Unknown media is NEVER defragmented (no override in v1, A-16)
    if (IsDefragBlockedMedia(preflight.mediaClass)) {
        return Result<void>::Failure(MakeError(ErrorCode::E_PREFLIGHT_SSD_BLOCKED));
    }

    // Rule 2: Must be elevated on real hardware (simulation bypasses OS elevation)
    if (!isSimulation && !isElevated) {
        return Result<void>::Failure(MakeError(ErrorCode::E_PREFLIGHT_NOT_ELEVATED));
    }

    // Rule 3: Pre-flight status checks
    switch (preflight.status) {
        case PreflightStatus::Clean:
            break;
        case PreflightStatus::QuickOnlyLowSpace:
            if (mode == DefragMode::FullDefrag) {
                return Result<void>::Failure(MakeError(
                    ErrorCode::E_PREFLIGHT_INSUFFICIENT_SPACE, {}, 0, 0,
                    "Free space is between 5% and full-defrag threshold; only Quick Defrag or Free Space Consolidation allowed"));
            }
            break;
        case PreflightStatus::InsufficientFreeSpace:
            return Result<void>::Failure(MakeError(ErrorCode::E_PREFLIGHT_INSUFFICIENT_SPACE));
        case PreflightStatus::DirtyVolume:
            return Result<void>::Failure(MakeError(ErrorCode::E_PREFLIGHT_DIRTY));
        case PreflightStatus::ReadOnlyVolume:
            return Result<void>::Failure(MakeError(ErrorCode::E_PREFLIGHT_READ_ONLY));
        case PreflightStatus::BitLockerLocked:
            return Result<void>::Failure(MakeError(ErrorCode::E_PREFLIGHT_LOCKED_BITLOCKER));
        case PreflightStatus::UnsupportedFileSystem:
            return Result<void>::Failure(MakeError(ErrorCode::E_PREFLIGHT_UNSUPPORTED_FS));
        case PreflightStatus::SolidStateBlocked:
            return Result<void>::Failure(MakeError(ErrorCode::E_PREFLIGHT_SSD_BLOCKED));
        case PreflightStatus::NotElevated:
            return Result<void>::Failure(MakeError(ErrorCode::E_PREFLIGHT_NOT_ELEVATED));
        case PreflightStatus::VolumeInaccessible:
            return Result<void>::Failure(MakeError(ErrorCode::E_PREFLIGHT_VOLUME_INACCESSIBLE));
    }

    // Rule 4: Explicit A-14 free-space floor (<5% free -> analysis-only)
    if (preflight.freePercent < config_.minFreeSpaceAnalysisOnlyPct) {
        return Result<void>::Failure(MakeError(
            ErrorCode::E_PREFLIGHT_INSUFFICIENT_SPACE, {}, 0, 0,
            "Volume free space below 5% safety floor"));
    }

    if (preflight.freePercent < config_.fullDefragThresholdPct && mode == DefragMode::FullDefrag) {
        return Result<void>::Failure(MakeError(
            ErrorCode::E_PREFLIGHT_INSUFFICIENT_SPACE, {}, 0, 0,
            "Volume free space below full-defrag threshold"));
    }

    return Result<void>::Success();
}

bool PolicyGate::GlobMatchCaseInsensitive(std::string_view pattern, std::string_view text) noexcept {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t starIdx = std::string_view::npos;
    std::size_t matchIdx = 0;

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || ToLowerAscii(pattern[p]) == ToLowerAscii(text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            starIdx = p++;
            matchIdx = t;
        } else if (starIdx != std::string_view::npos) {
            p = starIdx + 1;
            t = ++matchIdx;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

bool PolicyGate::IsFileExcluded(const FileEntry& file) const {
    if (file.flags.isExcluded) return true;
    for (const auto& rule : config_.exclusions) {
        if (!rule.enabled) continue;
        if (rule.minSizeBytes > 0 && file.sizeBytes < rule.minSizeBytes) continue;
        if (rule.maxSizeBytes > 0 && file.sizeBytes > rule.maxSizeBytes) continue;

        // Match either full path, filename, or prefix
        if (GlobMatchCaseInsensitive(rule.pattern, file.path) ||
            GlobMatchCaseInsensitive(rule.pattern, file.name)) {
            return true;
        }
        // Prefix directory match if pattern ends with '/' or '\'
        if (!rule.pattern.empty() && (rule.pattern.back() == '\\' || rule.pattern.back() == '/')) {
            if (file.path.size() >= rule.pattern.size() &&
                IEquals(std::string_view(file.path).substr(0, rule.pattern.size()), rule.pattern)) {
                return true;
            }
        }
    }
    return false;
}

SkipReason PolicyGate::ClassifySkipReason(const FileEntry& file,
                                          const CapabilityBits& caps,
                                          bool allowMftTier1Direct) const {
    if (file.flags.isPageOrHiber || IsProtectedSystemFileName(file.name)) {
        return SkipReason::PageOrSystemFile;
    }

    if (file.flags.isMetadata || file.id == kMftMirrFileId ||
        (!file.name.empty() && file.name[0] == '$')) {
        if (file.id == kMftFileId && allowMftTier1Direct && caps.supportsDirectMftMove) {
            return SkipReason::None;
        }
        return SkipReason::MetadataFile;
    }

    if (IsFileExcluded(file)) {
        return SkipReason::ExcludedByRule;
    }

    if (file.flags.isLocked) {
        return SkipReason::LockedOrInUse;
    }

    if (file.flags.reparsePoint || file.flags.encrypted || file.flags.hasAds) {
        return SkipReason::UnsupportedStream;
    }

    if ((file.flags.compressed || file.flags.sparse) && !caps.supportsSparseAndCompressed) {
        return SkipReason::UnsupportedStream;
    }

    if (file.flags.resident || file.extents.empty() || file.allocatedClusters == 0) {
        return SkipReason::None; // Nothing to move (resident), handled gracefully
    }

    return SkipReason::None;
}

} // namespace auralite
