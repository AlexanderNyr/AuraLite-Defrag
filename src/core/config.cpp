#include "core/config.hpp"
#include "core/error_catalog.hpp"
#include "core/json.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace auralite {

namespace {
bool HasPathTraversal(std::string_view s) noexcept {
    return s.find("..") != std::string_view::npos;
}
} // namespace

Result<AppConfig> ParseConfigJson(std::string_view jsonText) {
    auto parsed = ParseJsonStrict(jsonText);
    if (!parsed) return Result<AppConfig>::Failure(parsed.Error());
    if (!parsed.Value().IsObject()) {
        return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Config root must be an object"));
    }

    const auto& obj = parsed.Value().AsObject();
    std::string unknownKey;
    const std::vector<std::string_view> allowed{
        "schemaVersion", "theme", "fullDefragThresholdPct", "largeFileThresholdBytes",
        "quickDefragMinFragments", "quickDefragMinSizeBytes", "mapCols", "mapRows",
        "logRetentionDays", "maxHistoryOperations", "logMaxFileBytes", "logMaxRotations",
        "verboseLogPaths", "minimizeToTray", "notifyOnComplete", "verifyChecksumsSample",
        "exclusions"
    };
    if (!ValidateAllowedKeys(obj, allowed, unknownKey)) {
        return Result<AppConfig>::Failure(
            MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Unknown config key: " + unknownKey));
    }

    AppConfig cfg{};
    const auto* ver = parsed.Value().Find("schemaVersion");
    if (!ver || !ver->IsInt() || ver->AsInt() != 1) {
        return Result<AppConfig>::Failure(
            MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Unsupported or missing schemaVersion"));
    }
    cfg.schemaVersion = 1;

    if (const auto* th = parsed.Value().Find("theme")) {
        if (!th->IsString()) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "theme must be string"));
        }
        if (th->AsString() == "light") cfg.theme = ThemeMode::Light;
        else if (th->AsString() == "dark") cfg.theme = ThemeMode::Dark;
        else {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Invalid theme enum"));
        }
    }

    if (const auto* ft = parsed.Value().Find("fullDefragThresholdPct")) {
        if (!ft->IsDouble()) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "fullDefragThresholdPct must be number"));
        }
        const double v = ft->AsDouble();
        if (v < 5.0 || v > 15.0) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "fullDefragThresholdPct must be in [5.0, 15.0]"));
        }
        cfg.fullDefragThresholdPct = v;
    }

    if (const auto* lf = parsed.Value().Find("largeFileThresholdBytes")) {
        if (!lf->IsInt() || lf->AsInt() < 1048576) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "largeFileThresholdBytes invalid"));
        }
        cfg.largeFileThresholdBytes = static_cast<std::uint64_t>(lf->AsInt());
    }

    if (const auto* qf = parsed.Value().Find("quickDefragMinFragments")) {
        if (!qf->IsInt() || qf->AsInt() < 2 || qf->AsInt() > 10000) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "quickDefragMinFragments out of range"));
        }
        cfg.quickDefragMinFragments = static_cast<std::uint32_t>(qf->AsInt());
    }

    if (const auto* qs = parsed.Value().Find("quickDefragMinSizeBytes")) {
        if (!qs->IsInt() || qs->AsInt() < 0) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "quickDefragMinSizeBytes out of range"));
        }
        cfg.quickDefragMinSizeBytes = static_cast<std::uint64_t>(qs->AsInt());
    }

    if (const auto* mc = parsed.Value().Find("mapCols")) {
        if (!mc->IsInt() || mc->AsInt() < 20 || mc->AsInt() > 400) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "mapCols must be in [20, 400]"));
        }
        cfg.mapCols = static_cast<std::uint32_t>(mc->AsInt());
    }
    if (const auto* mr = parsed.Value().Find("mapRows")) {
        if (!mr->IsInt() || mr->AsInt() < 10 || mr->AsInt() > 200) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "mapRows must be in [10, 200]"));
        }
        cfg.mapRows = static_cast<std::uint32_t>(mr->AsInt());
    }

    if (const auto* lr = parsed.Value().Find("logRetentionDays")) {
        if (!lr->IsInt() || lr->AsInt() < 1 || lr->AsInt() > 3650) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "logRetentionDays invalid"));
        }
        cfg.logRetentionDays = static_cast<std::uint32_t>(lr->AsInt());
    }
    if (const auto* mh = parsed.Value().Find("maxHistoryOperations")) {
        if (!mh->IsInt() || mh->AsInt() < 10 || mh->AsInt() > 50000) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "maxHistoryOperations invalid"));
        }
        cfg.maxHistoryOperations = static_cast<std::uint32_t>(mh->AsInt());
    }
    if (const auto* lb = parsed.Value().Find("logMaxFileBytes")) {
        if (!lb->IsInt() || lb->AsInt() < 4096 || lb->AsInt() > 104857600) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "logMaxFileBytes invalid"));
        }
        cfg.logMaxFileBytes = static_cast<std::uint64_t>(lb->AsInt());
    }
    if (const auto* lm = parsed.Value().Find("logMaxRotations")) {
        if (!lm->IsInt() || lm->AsInt() < 1 || lm->AsInt() > 50) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "logMaxRotations invalid"));
        }
        cfg.logMaxRotations = static_cast<std::uint32_t>(lm->AsInt());
    }

    if (const auto* vp = parsed.Value().Find("verboseLogPaths")) {
        if (!vp->IsBool()) return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
        cfg.verboseLogPaths = vp->AsBool();
    }
    if (const auto* mt = parsed.Value().Find("minimizeToTray")) {
        if (!mt->IsBool()) return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
        cfg.minimizeToTray = mt->AsBool();
    }
    if (const auto* nc = parsed.Value().Find("notifyOnComplete")) {
        if (!nc->IsBool()) return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
        cfg.notifyOnComplete = nc->AsBool();
    }
    if (const auto* vc = parsed.Value().Find("verifyChecksumsSample")) {
        if (!vc->IsBool()) return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
        cfg.verifyChecksumsSample = vc->AsBool();
    }

    if (const auto* ex = parsed.Value().Find("exclusions")) {
        if (!ex->IsArray()) {
            return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "exclusions must be array"));
        }
        for (const auto& item : ex->AsArray()) {
            if (!item.IsObject()) {
                return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "exclusion item must be object"));
            }
            std::string unk;
            if (!ValidateAllowedKeys(item.AsObject(), {"pattern", "minSizeBytes", "maxSizeBytes", "enabled"}, unk)) {
                return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Unknown exclusion key: " + unk));
            }
            const auto* pat = item.Find("pattern");
            if (!pat || !pat->IsString() || pat->AsString().empty()) {
                return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Exclusion pattern required"));
            }
            if (HasPathTraversal(pat->AsString())) {
                return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_PATH_TRAVERSAL, {}, 0, 0, "Path traversal in exclusion pattern"));
            }
            ExclusionRule r;
            r.pattern = pat->AsString();
            if (const auto* mn = item.Find("minSizeBytes")) {
                if (!mn->IsInt() || mn->AsInt() < 0) return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
                r.minSizeBytes = static_cast<std::uint64_t>(mn->AsInt());
            }
            if (const auto* mx = item.Find("maxSizeBytes")) {
                if (!mx->IsInt() || mx->AsInt() < 0) return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
                r.maxSizeBytes = static_cast<std::uint64_t>(mx->AsInt());
            }
            if (const auto* en = item.Find("enabled")) {
                if (!en->IsBool()) return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
                r.enabled = en->AsBool();
            }
            cfg.exclusions.push_back(std::move(r));
        }
    }

    return Result<AppConfig>::Success(std::move(cfg));
}

std::string SerializeConfigJson(const AppConfig& cfg) {
    JsonObject root;
    root["schemaVersion"] = JsonValue{static_cast<std::int64_t>(cfg.schemaVersion)};
    root["theme"] = JsonValue{std::string(cfg.theme == ThemeMode::Dark ? "dark" : "light")};
    root["fullDefragThresholdPct"] = JsonValue{cfg.fullDefragThresholdPct};
    root["largeFileThresholdBytes"] = JsonValue{static_cast<std::int64_t>(cfg.largeFileThresholdBytes)};
    root["quickDefragMinFragments"] = JsonValue{static_cast<std::int64_t>(cfg.quickDefragMinFragments)};
    root["quickDefragMinSizeBytes"] = JsonValue{static_cast<std::int64_t>(cfg.quickDefragMinSizeBytes)};
    root["mapCols"] = JsonValue{static_cast<std::int64_t>(cfg.mapCols)};
    root["mapRows"] = JsonValue{static_cast<std::int64_t>(cfg.mapRows)};
    root["logRetentionDays"] = JsonValue{static_cast<std::int64_t>(cfg.logRetentionDays)};
    root["maxHistoryOperations"] = JsonValue{static_cast<std::int64_t>(cfg.maxHistoryOperations)};
    root["logMaxFileBytes"] = JsonValue{static_cast<std::int64_t>(cfg.logMaxFileBytes)};
    root["logMaxRotations"] = JsonValue{static_cast<std::int64_t>(cfg.logMaxRotations)};
    root["verboseLogPaths"] = JsonValue{cfg.verboseLogPaths};
    root["minimizeToTray"] = JsonValue{cfg.minimizeToTray};
    root["notifyOnComplete"] = JsonValue{cfg.notifyOnComplete};
    root["verifyChecksumsSample"] = JsonValue{cfg.verifyChecksumsSample};

    JsonArray exArr;
    for (const auto& r : cfg.exclusions) {
        JsonObject ro;
        ro["pattern"] = JsonValue{r.pattern};
        ro["minSizeBytes"] = JsonValue{static_cast<std::int64_t>(r.minSizeBytes)};
        ro["maxSizeBytes"] = JsonValue{static_cast<std::int64_t>(r.maxSizeBytes)};
        ro["enabled"] = JsonValue{r.enabled};
        exArr.push_back(JsonValue{std::move(ro)});
    }
    root["exclusions"] = JsonValue{std::move(exArr)};
    return DumpJson(JsonValue{std::move(root)}, 2) + "\n";
}

Result<AppConfig> LoadConfigFile(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return Result<AppConfig>::Failure(MakeError(ErrorCode::E_CONFIG_IO, {}, 0, 0, "Cannot open config file"));
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ParseConfigJson(ss.str());
}

Result<void> SaveConfigFileAtomic(const std::filesystem::path& path, const AppConfig& cfg) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return Result<void>::Failure(MakeError(ErrorCode::E_CONFIG_IO, {}, 0, 0, "Failed to open temp config file"));
        }
        const std::string data = SerializeConfigJson(cfg);
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        ofs.flush();
        if (!ofs) {
            return Result<void>::Failure(MakeError(ErrorCode::E_CONFIG_IO, {}, 0, 0, "Failed to write temp config file"));
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return Result<void>::Failure(MakeError(ErrorCode::E_CONFIG_IO, {}, 0, 0, "Atomic config rename failed"));
    }
    return Result<void>::Success();
}

bool IsPortableMode(const std::filesystem::path& exeDir) {
    std::error_code ec;
    return std::filesystem::exists(exeDir / "auralite.portable", ec) ||
           std::filesystem::exists(exeDir / "AuraLite-Data", ec);
}

} // namespace auralite
