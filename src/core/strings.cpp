#include "core/strings.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace auralite {

namespace {

struct StringEntry {
    const char* key;
    const char* text;
};

constexpr auto kStrings = std::to_array<StringEntry>({
    {"IDS_APP_TITLE", "AuraLite-Defrag"},
    {"IDS_ELEVATION_OK", "Administrator privileges active."},
    {"IDS_ELEVATION_REQUIRED", "Administrator rights are required to move data. Analysis is available without elevation."},
    {"IDS_STATE_IDLE", "Ready"},
    {"IDS_STATE_ANALYZING", "Analyzing volume..."},
    {"IDS_STATE_DEFRAGMENTING", "Defragmenting volume..."},
    {"IDS_STATE_PAUSED", "Paused"},
    {"IDS_OP_CANCELLED", "Operation cancelled."},
    {"IDS_OP_STOPPED", "Operation stopped cleanly; journal sealed."},
    {"IDS_OP_COMPLETE", "Operation completed and verified."},
    {"IDS_RESUME_OK", "Interrupted operation resumed from verified checkpoint."},

    // SSD / Media policy (§7.6)
    {"IDS_SSD_BLOCKED_TITLE", "Defragmentation is not available for this drive"},
    {"IDS_SSD_BLOCKED_BODY", "This volume is on solid-state storage, or its storage type reports no seek penalty. AuraLite-Defrag will not move data on such volumes. You can still analyse the drive and export reports."},
    {"IDS_SSD_UNKNOWN_BODY", "The storage type could not be confirmed. To protect the volume, defragmentation is disabled; analysis is available."},
    {"IDS_SSD_ANALYZE_ONLY", "Analysis only — no data will be moved."},
    {"IDS_SSD_CLI_REFUSAL", "Defragmentation refused: solid-state or unknown media. Use the analyze command instead."},
    {"IDS_SSD_NO_OVERRIDE", "This policy cannot be overridden in this version."},

    // MFT Tiers (§7.6)
    {"IDS_MFT_TIER1_OK", "The master file table was consolidated online. {0} extents were combined into one."},
    {"IDS_MFT_TIER1_UNAVAILABLE", "This Windows version does not permit consolidating the master file table online. A safer optimisation will be used instead."},
    {"IDS_MFT_TIER1_FAILED", "The online master file table operation could not be completed safely. No further metadata changes were made. See the report for details."},
    {"IDS_MFT_TIER2_APPLIED", "Space next to the master file table was cleared so future growth can stay contiguous. {0} files were relocated; the master file table itself was not moved."},
    {"IDS_MFT_TIER3_REPORT_ONLY", "The master file table could not be changed in this session. A detailed report is available, including fragmentation, zone usage, and recommended settings."},
    {"IDS_MFT_TIER4_UNAVAILABLE", "Consolidating these locked system files requires a boot-time operation, which is not available in this version. No changes were made."},
    {"IDS_MFT_CONFIRM", "Master file table operations affect critical file system metadata. A checkpoint will be created first, and a simulation report is available. Continue?"},
    {"IDS_MFT_ZONE_GUIDANCE", "Windows reserves an area for master file table growth. You can review the current reservation with fsutil behavior query mftzone. Changing it requires a restart and affects future growth. AuraLite-Defrag does not change this setting for you."},
    {"IDS_MFT_ZONE_EVACUATED", "{0} user files were moved out of the reserved area; {1} clusters are now available for master file table growth."},
    {"IDS_MFT_SKIPPED", "Metadata changes were skipped: {0}."},

    // Pre-flight & Errors
    {"IDS_PREFLIGHT_FAILED_GENERIC", "Pre-flight safety checks prevented writes on this volume."},
    {"IDS_PREFLIGHT_READ_ONLY", "This volume is read-only. Analysis is available; writes are disabled."},
    {"IDS_PREFLIGHT_BITLOCKER", "This volume is locked. Unlock it before analyzing or defragmenting."},
    {"IDS_PREFLIGHT_UNSUPPORTED_FS", "Only NTFS volumes are supported in this version."},
    {"IDS_DIRTY_VOLUME", "This volume is marked dirty. Run the Windows disk check tool, then try again."},
    {"IDS_DRY_RUN_HEADER", "Simulation — no data will be moved."},
    {"IDS_JOURNAL_RECOVERY", "An interrupted operation was found. Its checkpoint was verified; you can resume or discard it."},
    {"IDS_DEMO_BANNER", "Simulation mode — sample data, no disks are accessed."},
    {"IDS_ERR_ANALYSIS", "An error occurred while reading volume metadata."},
    {"IDS_ERR_ACCESS", "Access was denied to one or more locked files; affected items were skipped."},
    {"IDS_ERR_STALE_BITMAP", "Some clusters changed during the operation; affected files were skipped."},
    {"IDS_ERR_MOVE_GENERIC", "A file extent could not be relocated and was skipped."},
    {"IDS_ERR_UNSUPPORTED_STREAM", "This stream type cannot be moved safely and was skipped."},
    {"IDS_ERR_FILE_VANISHED", "A file was deleted or moved externally during the operation and was skipped."},
    {"IDS_ERR_VERIFY", "Post-move integrity verification failed. Writes have been aborted and the journal has been sealed."},
    {"IDS_ERR_JOURNAL", "The operation journal could not be verified. Writes were stopped to protect volume integrity."},
    {"IDS_ERR_CONFIG", "The configuration or scenario input failed strict schema validation."},
    {"IDS_ERR_EXPORT", "Export failed before completing; no partial file was committed."},
    {"IDS_ERR_PATH", "A path failed target-volume confinement checks and was rejected."},
    {"IDS_ERR_VOLUME_REMOVED", "The target volume was dismounted or disconnected. Operation aborted."},
    {"IDS_ERR_INTERNAL", "An unexpected internal state error occurred. Operation stopped."},
});

} // namespace

std::string_view StringTable::Lookup(std::string_view key) noexcept {
    for (const auto& e : kStrings) {
        if (key == e.key) return e.text;
    }
    return key;
}

std::string StringTable::Format(std::string_view key, const std::vector<std::string>& args) {
    std::string out{Lookup(key)};
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string token = "{" + std::to_string(i) + "}";
        std::size_t pos = 0;
        while ((pos = out.find(token, pos)) != std::string::npos) {
            out.replace(pos, token.size(), args[i]);
            pos += args[i].size();
        }
    }
    return out;
}

std::vector<std::string_view> StringTable::AllKeys() {
    std::vector<std::string_view> keys;
    keys.reserve(kStrings.size());
    for (const auto& e : kStrings) {
        keys.emplace_back(e.key);
    }
    return keys;
}

std::string FormatInteger(std::uint64_t value, char thousandsSep) {
    std::string raw = std::to_string(value);
    std::string out;
    out.reserve(raw.size() + raw.size() / 3);
    int count = 0;
    for (auto it = raw.rbegin(); it != raw.rend(); ++it) {
        if (count > 0 && (count % 3) == 0) {
            out.push_back(thousandsSep);
        }
        out.push_back(*it);
        ++count;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::string FormatByteSize(std::uint64_t bytes, char decimalSep) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double val = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (val >= 1024.0 && unit < 4) {
        val /= 1024.0;
        ++unit;
    }
    char buf[64]{};
    if (unit == 0) {
        std::snprintf(buf, sizeof(buf), "%llu %s", static_cast<unsigned long long>(bytes), kUnits[unit]);
        return std::string(buf);
    }
    std::snprintf(buf, sizeof(buf), "%.2f %s", val, kUnits[unit]);
    std::string s(buf);
    if (decimalSep != '.') {
        for (char& c : s) {
            if (c == '.') c = decimalSep;
        }
    }
    return s;
}

std::string FormatPercent(double pct, char decimalSep) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%.2f%%", pct);
    std::string s(buf);
    if (decimalSep != '.') {
        for (char& c : s) {
            if (c == '.') c = decimalSep;
        }
    }
    return s;
}

} // namespace auralite
