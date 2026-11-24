#include "differential/differential_oracle.hpp"

#include "core/error_catalog.hpp"
#include "core/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace auralite::differential::golden {

namespace {

const char* SkipName(SkipReason r) {
    switch (r) {
        case SkipReason::None: return "None";
        case SkipReason::MetadataFile: return "MetadataFile";
        case SkipReason::PageOrSystemFile: return "PageOrSystemFile";
        case SkipReason::LockedOrInUse: return "LockedOrInUse";
        case SkipReason::AccessDenied: return "AccessDenied";
        case SkipReason::ExcludedByRule: return "ExcludedByRule";
        case SkipReason::UnsupportedStream: return "UnsupportedStream";
        case SkipReason::InsufficientContiguousSpace: return "InsufficientContiguousSpace";
        case SkipReason::StaleBitmapConflict: return "StaleBitmapConflict";
        case SkipReason::ReadOnlyOrPreFlightBlocked: return "ReadOnlyOrPreFlightBlocked";
        case SkipReason::IoError: return "IoError";
        case SkipReason::MftSafeguardAbort: return "MftSafeguardAbort";
        case SkipReason::FileVanished: return "FileVanished";
    }
    return "Unknown";
}

SkipReason ParseSkip(const std::string& s) {
    for (int i = 0; i <= static_cast<int>(SkipReason::FileVanished); ++i) {
        const auto r = static_cast<SkipReason>(i);
        if (s == SkipName(r)) return r;
    }
    return SkipReason::None;
}

JsonValue Num(std::int64_t v) {
    JsonValue j;
    j.data = v;
    return j;
}

JsonValue Dbl(double v) { return Num(static_cast<std::int64_t>(std::llround(v * 1000.0))); }

JsonValue Str(std::string s) {
    JsonValue j;
    j.data = std::move(s);
    return j;
}

JsonValue Arr(JsonArray a) {
    JsonValue j;
    j.data = std::move(a);
    return j;
}

JsonValue Obj(JsonObject o) {
    JsonValue j;
    j.data = std::move(o);
    return j;
}

} // namespace

std::string TraceFileName(std::string_view scenarioId) {
    std::string out(scenarioId);
    if (out.empty()) out = "unnamed";
    // Scenario ids are already filesystem-safe (SC-01, DT-02, …) but be strict.
    for (char& c : out) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) c = '_';
    }
    return out + ".trace.json";
}

std::string Serialize(const NormalizedRun& run) {
    JsonObject root;
    root["traceFormatVersion"] = Num(kTraceFormatVersion);
    root["scenarioId"] = Str(run.scenarioId);
    root["label"] = Str(run.label);

    JsonObject summary;
    summary["totalClusters"] = Num(static_cast<std::int64_t>(run.summary.totalClusters));
    summary["freeClusters"] = Num(static_cast<std::int64_t>(run.summary.freeClusters));
    summary["usedClusters"] = Num(static_cast<std::int64_t>(run.summary.usedClusters));
    summary["totalFiles"] = Num(static_cast<std::int64_t>(run.summary.totalFiles));
    summary["fragmentedFiles"] = Num(static_cast<std::int64_t>(run.summary.fragmentedFiles));
    summary["residentFiles"] = Num(static_cast<std::int64_t>(run.summary.residentFiles));
    summary["unmovableFiles"] = Num(static_cast<std::int64_t>(run.summary.unmovableFiles));
    summary["largestFreeRunClusters"] = Num(static_cast<std::int64_t>(run.summary.largestFreeRunClusters));
    summary["volumeFragmentationPct"] = Dbl(run.summary.volumeFragmentationPct);
    summary["fileFragmentationPct"] = Dbl(run.summary.fileFragmentationPct);
    root["summary"] = Obj(std::move(summary));

    JsonObject mft;
    mft["fragmentCount"] = Num(run.mft.fragmentCount);
    mft["totalRecords"] = Num(static_cast<std::int64_t>(run.mft.totalRecords));
    mft["inUseRecords"] = Num(static_cast<std::int64_t>(run.mft.inUseRecords));
    mft["mftStartLcn"] = Num(static_cast<std::int64_t>(run.mft.mftStartLcn));
    mft["zoneStartLcn"] = Num(static_cast<std::int64_t>(run.mft.zoneStartLcn));
    mft["zoneEndLcn"] = Num(static_cast<std::int64_t>(run.mft.zoneEndLcn));
    mft["totalZoneClusters"] = Num(static_cast<std::int64_t>(run.mft.totalZoneClusters));
    mft["freeZoneClusters"] = Num(static_cast<std::int64_t>(run.mft.freeZoneClusters));
    root["mft"] = Obj(std::move(mft));

    JsonArray files;
    files.reserve(run.files.size());
    for (const auto& f : run.files) {
        JsonObject fo;
        fo["key"] = Str(f.key);
        fo["sizeBytes"] = Num(static_cast<std::int64_t>(f.sizeBytes));
        fo["allocatedClusters"] = Num(static_cast<std::int64_t>(f.allocatedClusters));
        fo["fragmentCount"] = Num(f.fragmentCount);
        JsonArray ex;
        ex.reserve(f.extents.size());
        for (const auto& e : f.extents) {
            JsonObject eo;
            eo["vcn"] = Num(static_cast<std::int64_t>(e.startVcn));
            eo["lcn"] = Num(static_cast<std::int64_t>(e.startLcn));
            eo["count"] = Num(static_cast<std::int64_t>(e.clusterCount));
            ex.push_back(Obj(std::move(eo)));
        }
        fo["extents"] = Arr(std::move(ex));
        files.push_back(Obj(std::move(fo)));
    }
    root["files"] = Arr(std::move(files));

    JsonArray moves;
    moves.reserve(run.moves.size());
    for (const auto& m : run.moves) {
        JsonObject mo;
        mo["file"] = Str(m.fileKey);
        mo["vcn"] = Num(static_cast<std::int64_t>(m.startVcn));
        mo["sourceLcn"] = Num(static_cast<std::int64_t>(m.sourceLcn));
        mo["targetLcn"] = Num(static_cast<std::int64_t>(m.targetLcn));
        mo["count"] = Num(static_cast<std::int64_t>(m.clusterCount));
        moves.push_back(Obj(std::move(mo)));
    }
    root["moves"] = Arr(std::move(moves));

    JsonArray skips;
    skips.reserve(run.skips.size());
    for (const auto& s : run.skips) {
        JsonObject so;
        so["file"] = Str(s.fileKey);
        so["reason"] = Str(SkipName(s.reason));
        skips.push_back(Obj(std::move(so)));
    }
    root["skips"] = Arr(std::move(skips));

    root["moveCallCount"] = Num(static_cast<std::int64_t>(run.moveCallCount));
    root["errorCodeValue"] = Num(static_cast<std::int64_t>(run.errorCodeValue));

    return DumpJson(Obj(std::move(root)), 2) + "\n";
}

Result<NormalizedRun> Parse(std::string_view text) {
    auto parsed = ParseJsonStrict(text);
    if (!parsed) {
        return Result<NormalizedRun>::Failure(parsed.Error());
    }
    const JsonValue& root = parsed.Value();
    if (!root.IsObject()) {
        return Result<NormalizedRun>::Failure(
            MakeError(ErrorCode::E_CONFIG_PARSE, "trace", 0, 0, "trace root must be an object"));
    }
    const JsonValue* ver = root.Find("traceFormatVersion");
    if (ver == nullptr || ver->AsInt(-1) != static_cast<std::int64_t>(kTraceFormatVersion)) {
        return Result<NormalizedRun>::Failure(MakeError(
            ErrorCode::E_CONFIG_PARSE, "trace", 0, 0, "unsupported traceFormatVersion"));
    }

    NormalizedRun run;
    if (const JsonValue* v = root.Find("scenarioId")) run.scenarioId = v->AsString();
    if (const JsonValue* v = root.Find("label")) run.label = v->AsString();

    if (const JsonValue* s = root.Find("summary")) {
        run.summary.totalClusters = static_cast<ClusterCount>(s->Find("totalClusters")->AsInt());
        run.summary.freeClusters = static_cast<ClusterCount>(s->Find("freeClusters")->AsInt());
        run.summary.usedClusters = static_cast<ClusterCount>(s->Find("usedClusters")->AsInt());
        run.summary.totalFiles = static_cast<std::uint64_t>(s->Find("totalFiles")->AsInt());
        run.summary.fragmentedFiles = static_cast<std::uint64_t>(s->Find("fragmentedFiles")->AsInt());
        run.summary.residentFiles = static_cast<std::uint64_t>(s->Find("residentFiles")->AsInt());
        run.summary.unmovableFiles = static_cast<std::uint64_t>(s->Find("unmovableFiles")->AsInt());
        run.summary.largestFreeRunClusters =
            static_cast<ClusterCount>(s->Find("largestFreeRunClusters")->AsInt());
        run.summary.volumeFragmentationPct = s->Find("volumeFragmentationPct")->AsDouble() / 1000.0;
        run.summary.fileFragmentationPct = s->Find("fileFragmentationPct")->AsDouble() / 1000.0;
    }
    if (const JsonValue* m = root.Find("mft")) {
        run.mft.fragmentCount = static_cast<std::uint32_t>(m->Find("fragmentCount")->AsInt());
        run.mft.totalRecords = static_cast<std::uint64_t>(m->Find("totalRecords")->AsInt());
        run.mft.inUseRecords = static_cast<std::uint64_t>(m->Find("inUseRecords")->AsInt());
        run.mft.mftStartLcn = static_cast<Lcn>(m->Find("mftStartLcn")->AsInt());
        run.mft.zoneStartLcn = static_cast<Lcn>(m->Find("zoneStartLcn")->AsInt());
        run.mft.zoneEndLcn = static_cast<Lcn>(m->Find("zoneEndLcn")->AsInt());
        run.mft.totalZoneClusters = static_cast<ClusterCount>(m->Find("totalZoneClusters")->AsInt());
        run.mft.freeZoneClusters = static_cast<ClusterCount>(m->Find("freeZoneClusters")->AsInt());
    }
    if (const JsonValue* fs = root.Find("files")) {
        for (const auto& fo : fs->AsArray()) {
            NormalizedFile f;
            f.key = fo.Find("key")->AsString();
            f.sizeBytes = static_cast<std::uint64_t>(fo.Find("sizeBytes")->AsInt());
            f.allocatedClusters = static_cast<ClusterCount>(fo.Find("allocatedClusters")->AsInt());
            f.fragmentCount = static_cast<std::uint32_t>(fo.Find("fragmentCount")->AsInt());
            if (const JsonValue* ex = fo.Find("extents")) {
                for (const auto& eo : ex->AsArray()) {
                    f.extents.push_back({static_cast<Vcn>(eo.Find("vcn")->AsInt()),
                                         static_cast<Lcn>(eo.Find("lcn")->AsInt()),
                                         static_cast<ClusterCount>(eo.Find("count")->AsInt())});
                }
            }
            run.files.push_back(std::move(f));
        }
    }
    if (const JsonValue* ms = root.Find("moves")) {
        for (const auto& mo : ms->AsArray()) {
            NormalizedMove m;
            m.fileKey = mo.Find("file")->AsString();
            m.startVcn = static_cast<Vcn>(mo.Find("vcn")->AsInt());
            m.sourceLcn = static_cast<Lcn>(mo.Find("sourceLcn")->AsInt());
            m.targetLcn = static_cast<Lcn>(mo.Find("targetLcn")->AsInt());
            m.clusterCount = static_cast<ClusterCount>(mo.Find("count")->AsInt());
            run.moves.push_back(std::move(m));
        }
    }
    if (const JsonValue* ss = root.Find("skips")) {
        for (const auto& so : ss->AsArray()) {
            run.skips.push_back({so.Find("file")->AsString(),
                                 ParseSkip(so.Find("reason")->AsString())});
        }
    }
    if (const JsonValue* v = root.Find("moveCallCount")) {
        run.moveCallCount = static_cast<std::uint64_t>(v->AsInt());
    }
    if (const JsonValue* v = root.Find("errorCodeValue")) {
        run.errorCodeValue = static_cast<std::uint32_t>(v->AsInt());
    }

    std::sort(run.files.begin(), run.files.end(),
              [](const NormalizedFile& a, const NormalizedFile& b) { return a.key < b.key; });
    return Result<NormalizedRun>::Success(std::move(run));
}

} // namespace auralite::differential::golden
