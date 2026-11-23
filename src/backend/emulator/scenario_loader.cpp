#include "backend/emulator/scenario_loader.hpp"
#include "core/error_catalog.hpp"
#include "core/json.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace auralite {

namespace {

bool IsValidGenerator(std::string_view g) noexcept {
    return g == "G-1" || g == "G-2" || g == "G-3" || g == "G-4" || g == "G-5" || g == "G-6";
}

bool IsPowerOfTwo(std::uint32_t v) noexcept {
    return v > 0 && (v & (v - 1U)) == 0;
}

FaultType ParseFaultType(std::string_view s, bool& ok) noexcept {
    ok = true;
    if (s == "IoError") return FaultType::IoError;
    if (s == "PartialFailure") return FaultType::PartialFailure;
    if (s == "InterruptionMidMove") return FaultType::InterruptionMidMove;
    if (s == "AccessDenied") return FaultType::AccessDenied;
    if (s == "LockedInUse") return FaultType::LockedInUse;
    if (s == "UnsupportedStream") return FaultType::UnsupportedStream;
    if (s == "FileVanished") return FaultType::FileVanished;
    ok = false;
    return FaultType::None;
}

MediaClass ParseMediaClass(std::string_view s, bool& ok) noexcept {
    ok = true;
    if (s == "Spinning") return MediaClass::Spinning;
    if (s == "SolidState") return MediaClass::SolidState;
    if (s == "Unknown") return MediaClass::Unknown;
    ok = false;
    return MediaClass::Unknown;
}

} // namespace

Result<ScenarioSpec> ParseScenarioJson(std::string_view jsonText) {
    auto parsed = ParseJsonStrict(jsonText, JsonLimits{1024U * 1024U, 8U});
    if (!parsed) return Result<ScenarioSpec>::Failure(parsed.Error());
    if (!parsed.Value().IsObject()) {
        return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Scenario root must be object"));
    }

    const auto& root = parsed.Value().AsObject();
    std::string unk;
    if (!ValidateAllowedKeys(root,
                             {"schemaVersion", "id", "seed", "description", "volume", "mft",
                              "generator", "files", "faults", "expected", "tolerances"},
                             unk)) {
        return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Unknown scenario key: " + unk));
    }

    ScenarioSpec spec{};

    const auto* sv = parsed.Value().Find("schemaVersion");
    if (!sv || !sv->IsInt() || sv->AsInt() != 1) {
        return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "schemaVersion must be 1"));
    }

    const auto* id = parsed.Value().Find("id");
    if (!id || !id->IsString() || id->AsString().empty() || id->AsString().find("..") != std::string::npos) {
        return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Invalid scenario id"));
    }
    spec.id = id->AsString();

    const auto* seed = parsed.Value().Find("seed");
    if (!seed || !seed->IsInt() || seed->AsInt() < 0) {
        return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "seed must be non-negative int"));
    }
    spec.seed = static_cast<std::uint64_t>(seed->AsInt());

    if (const auto* desc = parsed.Value().Find("description")) {
        if (!desc->IsString()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
        spec.description = desc->AsString();
    }

    if (const auto* gen = parsed.Value().Find("generator")) {
        if (!gen->IsString() || !IsValidGenerator(gen->AsString())) {
            return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Invalid generator"));
        }
        spec.generator = gen->AsString();
    }

    if (const auto* vol = parsed.Value().Find("volume")) {
        if (!vol->IsObject()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
        if (!ValidateAllowedKeys(vol->AsObject(),
                                 {"clusters", "bytesPerCluster", "freeTargetPct", "fs", "mediaClass",
                                  "dirty", "readOnly", "bitLockerLocked", "supportsDirectMftMove"},
                                 unk)) {
            return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Unknown volume key: " + unk));
        }
        if (const auto* cl = vol->Find("clusters")) {
            if (!cl->IsInt() || cl->AsInt() < 64 || cl->AsInt() > 100000000LL) {
                return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "clusters out of range"));
            }
            spec.clusters = static_cast<ClusterCount>(cl->AsInt());
        }
        if (const auto* bpc = vol->Find("bytesPerCluster")) {
            if (!bpc->IsInt() || bpc->AsInt() < 512 || bpc->AsInt() > 65536 ||
                !IsPowerOfTwo(static_cast<std::uint32_t>(bpc->AsInt()))) {
                return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "bytesPerCluster invalid"));
            }
            spec.bytesPerCluster = static_cast<std::uint32_t>(bpc->AsInt());
        }
        if (const auto* ft = vol->Find("freeTargetPct")) {
            if (!ft->IsDouble() || ft->AsDouble() < 0.0 || ft->AsDouble() > 100.0) {
                return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "freeTargetPct out of [0,100]"));
            }
            spec.freeTargetPct = ft->AsDouble();
        }
        if (const auto* fs = vol->Find("fs")) {
            if (!fs->IsString() || fs->AsString().empty()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.fsName = fs->AsString();
        }
        if (const auto* mc = vol->Find("mediaClass")) {
            bool ok = false;
            if (!mc->IsString()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.mediaClass = ParseMediaClass(mc->AsString(), ok);
            if (!ok) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Invalid mediaClass"));
        }
        if (const auto* d = vol->Find("dirty")) {
            if (!d->IsBool()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.dirty = d->AsBool();
        }
        if (const auto* ro = vol->Find("readOnly")) {
            if (!ro->IsBool()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.readOnly = ro->AsBool();
        }
        if (const auto* bl = vol->Find("bitLockerLocked")) {
            if (!bl->IsBool()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.bitLockerLocked = bl->AsBool();
        }
        if (const auto* sm = vol->Find("supportsDirectMftMove")) {
            if (!sm->IsBool()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.supportsDirectMftMove = sm->AsBool();
        }
    }

    if (const auto* mft = parsed.Value().Find("mft")) {
        if (!mft->IsObject()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
        if (!ValidateAllowedKeys(mft->AsObject(),
                                 {"fragments", "zonePct", "occupiedByUserFilesPct", "recordCount", "inUseRecords"},
                                 unk)) {
            return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Unknown mft key: " + unk));
        }
        if (const auto* fr = mft->Find("fragments")) {
            if (!fr->IsInt() || fr->AsInt() < 1 || fr->AsInt() > 1000) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.mftFragments = static_cast<std::uint32_t>(fr->AsInt());
        }
        if (const auto* zp = mft->Find("zonePct")) {
            if (!zp->IsDouble() || zp->AsDouble() < 1.0 || zp->AsDouble() > 50.0) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.zonePct = zp->AsDouble();
        }
        if (const auto* op = mft->Find("occupiedByUserFilesPct")) {
            if (!op->IsDouble() || op->AsDouble() < 0.0 || op->AsDouble() > 100.0) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.occupiedByUserFilesPct = op->AsDouble();
        }
        if (const auto* rc = mft->Find("recordCount")) {
            if (!rc->IsInt() || rc->AsInt() < 16) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.mftRecordCount = static_cast<std::uint64_t>(rc->AsInt());
        }
        if (const auto* ir = mft->Find("inUseRecords")) {
            if (!ir->IsInt() || ir->AsInt() < 0) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.mftInUseRecords = static_cast<std::uint64_t>(ir->AsInt());
        }
    }

    if (const auto* files = parsed.Value().Find("files")) {
        if (!files->IsObject()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
        if (!ValidateAllowedKeys(files->AsObject(),
                                 {"count", "extentsPerFile", "sizeMinClusters", "sizeMaxClusters",
                                  "residentPct", "compressedCount", "sparseCount", "encryptedCount",
                                  "reparseCount", "lockedCount", "adsCount", "longPaths",
                                  "unicodeNames", "includeSystemFiles"},
                                 unk)) {
            return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Unknown files key: " + unk));
        }
        auto getNonNeg = [&](const char* k, std::uint32_t& dst) -> bool {
            if (const auto* v = files->Find(k)) {
                if (!v->IsInt() || v->AsInt() < 0 || v->AsInt() > 2000000) return false;
                dst = static_cast<std::uint32_t>(v->AsInt());
            }
            return true;
        };
        if (!getNonNeg("count", spec.fileCount) ||
            !getNonNeg("extentsPerFile", spec.extentsPerFile) ||
            !getNonNeg("sizeMinClusters", spec.sizeMinClusters) ||
            !getNonNeg("sizeMaxClusters", spec.sizeMaxClusters) ||
            !getNonNeg("compressedCount", spec.compressedCount) ||
            !getNonNeg("sparseCount", spec.sparseCount) ||
            !getNonNeg("encryptedCount", spec.encryptedCount) ||
            !getNonNeg("reparseCount", spec.reparseCount) ||
            !getNonNeg("lockedCount", spec.lockedCount) ||
            !getNonNeg("adsCount", spec.adsCount)) {
            return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Invalid files numeric field"));
        }
        if (const auto* rp = files->Find("residentPct")) {
            if (!rp->IsDouble() || rp->AsDouble() < 0.0 || rp->AsDouble() > 100.0) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.residentPct = rp->AsDouble();
        }
        auto getBool = [&](const char* k, bool& dst) -> bool {
            if (const auto* v = files->Find(k)) {
                if (!v->IsBool()) return false;
                dst = v->AsBool();
            }
            return true;
        };
        if (!getBool("longPaths", spec.longPaths) ||
            !getBool("unicodeNames", spec.unicodeNames) ||
            !getBool("includeSystemFiles", spec.includeSystemFiles)) {
            return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
        }
    }

    if (const auto* faults = parsed.Value().Find("faults")) {
        if (!faults->IsArray()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
        for (const auto& item : faults->AsArray()) {
            if (!item.IsObject()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            if (!ValidateAllowedKeys(item.AsObject(), {"type", "atOperation", "targetFileId", "errorCode"}, unk)) {
                return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Unknown fault key: " + unk));
            }
            const auto* tp = item.Find("type");
            if (!tp || !tp->IsString()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            bool ok = false;
            ScheduledFault f{};
            f.type = ParseFaultType(tp->AsString(), ok);
            if (!ok) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Invalid fault type"));
            if (const auto* ao = item.Find("atOperation")) {
                if (!ao->IsInt() || ao->AsInt() < 0) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
                f.atOperation = static_cast<std::uint64_t>(ao->AsInt());
            }
            if (const auto* tf = item.Find("targetFileId")) {
                if (!tf->IsInt() || tf->AsInt() < 0) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
                f.targetFileId = static_cast<FileId>(tf->AsInt());
            }
            if (const auto* ec = item.Find("errorCode")) {
                if (!ec->IsInt() || ec->AsInt() < 0) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
                f.win32Error = static_cast<std::uint32_t>(ec->AsInt());
            }
            spec.faults.push_back(f);
        }
    }

    if (const auto* exp = parsed.Value().Find("expected")) {
        if (!exp->IsObject()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
        if (!ValidateAllowedKeys(exp->AsObject(),
                                 {"fragmentationBeforeMin", "fragmentationAfterMax",
                                  "expectPreflightFail", "expectZeroMoves", "mftTier"},
                                 unk)) {
            return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Unknown expected key: " + unk));
        }
        if (const auto* fb = exp->Find("fragmentationBeforeMin")) {
            if (!fb->IsDouble()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.fragmentationBeforeMin = fb->AsDouble();
        }
        if (const auto* fa = exp->Find("fragmentationAfterMax")) {
            if (!fa->IsDouble()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.fragmentationAfterMax = fa->AsDouble();
        }
        if (const auto* pf = exp->Find("expectPreflightFail")) {
            if (!pf->IsBool()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.expectPreflightFail = pf->AsBool();
        }
        if (const auto* zm = exp->Find("expectZeroMoves")) {
            if (!zm->IsBool()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.expectZeroMoves = zm->AsBool();
        }
        if (const auto* mt = exp->Find("mftTier")) {
            if (!mt->IsInt() || mt->AsInt() < 1 || mt->AsInt() > 4) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.expectedMftTier = static_cast<MftTier>(mt->AsInt());
        }
    }

    if (const auto* tol = parsed.Value().Find("tolerances")) {
        if (!tol->IsObject()) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
        if (!ValidateAllowedKeys(tol->AsObject(), {"agreementMinPct"}, unk)) {
            return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Unknown tolerances key: " + unk));
        }
        if (const auto* am = tol->Find("agreementMinPct")) {
            if (!am->IsDouble() || am->AsDouble() < 0.0 || am->AsDouble() > 100.0) return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_SCHEMA));
            spec.agreementMinPct = am->AsDouble();
        }
    }

    return Result<ScenarioSpec>::Success(std::move(spec));
}

Result<ScenarioSpec> LoadScenarioFile(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return Result<ScenarioSpec>::Failure(MakeError(ErrorCode::E_CONFIG_IO, {}, 0, 0, "Cannot open scenario file"));
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ParseScenarioJson(ss.str());
}

Result<std::unique_ptr<EmulatorBackend>> BuildEmulatorFromScenario(const ScenarioSpec& spec) {
    EmulatorConfig cfg{};
    cfg.volumeId = "SIM-" + spec.id;
    cfg.seed = spec.seed;
    cfg.mediaClass = spec.mediaClass;
    cfg.dirtyVolume = spec.dirty;
    cfg.readOnlyVolume = spec.readOnly;
    cfg.bitLockerLocked = spec.bitLockerLocked;
    cfg.isSimulationMode = true;

    cfg.mediaEvidence.seekPenaltyValid = true;
    cfg.mediaEvidence.incursSeekPenalty = (spec.mediaClass == MediaClass::Spinning);
    cfg.mediaEvidence.trimValid = true;
    cfg.mediaEvidence.trimEnabled = (spec.mediaClass == MediaClass::SolidState);
    cfg.mediaEvidence.isVirtualDisk = true;
    cfg.mediaEvidence.probeLog.push_back("EmulatorScenario=" + spec.id);

    cfg.capabilities.supportsMftAnalysis = true;
    cfg.capabilities.supportsDirectMftMove = spec.supportsDirectMftMove;
    cfg.capabilities.supportsOnlineMetadataMove = spec.supportsDirectMftMove;
    cfg.capabilities.supportsSparseAndCompressed = true;
    cfg.capabilities.moveGranularityClusters = 1;
    cfg.capabilities.sparseMoveGranularityClusters = 16;
    cfg.capabilities.maxMoveClustersPerCall = 65536;
    cfg.capabilities.bootTimeAvailable = false;

    auto& g = cfg.geometry;
    g.totalClusters = spec.clusters;
    g.bytesPerCluster = spec.bytesPerCluster;
    g.bytesPerSector = 512;
    g.bytesPerFileRecordSegment = 1024;
    g.clustersPerFileRecordSegment = 0;
    g.fsName = spec.fsName;
    g.label = "AuraLite-" + spec.id;
    g.driveLetter = "S:";
    g.volumeSerialNumber = spec.seed ^ 0x414C54303031ULL;

    const ClusterCount zoneLen = std::max<ClusterCount>(
        32, static_cast<ClusterCount>((static_cast<double>(spec.clusters) * spec.zonePct) / 100.0));
    const Lcn mftBase = std::min<Lcn>(16, spec.clusters / 16);
    const ClusterCount mftTotalClusters = std::max<ClusterCount>(
        spec.mftFragments * 2ULL, std::min<ClusterCount>(zoneLen / 4, std::max<ClusterCount>(4, spec.mftRecordCount / 4)));
    g.mftStartLcn = mftBase;
    g.mftZoneStart = mftBase;
    g.mftZoneEnd = std::min<Lcn>(spec.clusters - 1, mftBase + zoneLen);
    g.mft2StartLcn = std::min<Lcn>(spec.clusters - 4, spec.clusters / 2);
    g.mftValidDataLength = mftTotalClusters * spec.bytesPerCluster;

    auto emu = std::make_unique<EmulatorBackend>(cfg);
    emu->SetScheduledFaults(spec.faults);

    // Track allocated clusters locally so placement is O(1) per cluster without conflicts
    std::vector<bool> used(static_cast<std::size_t>(spec.clusters), false);
    auto claimRange = [&](Lcn start, ClusterCount len) -> bool {
        if (start + len > spec.clusters) return false;
        for (ClusterCount i = 0; i < len; ++i) {
            if (used[static_cast<std::size_t>(start + i)]) return false;
        }
        for (ClusterCount i = 0; i < len; ++i) {
            used[static_cast<std::size_t>(start + i)] = true;
        }
        return true;
    };

    // 1. Add $MFT (id=0) and $MFTMirr (id=1)
    {
        FileEntry mft{};
        mft.id = kMftFileId;
        mft.name = "$MFT";
        mft.path = "\\$MFT";
        mft.sizeBytes = g.mftValidDataLength;
        mft.flags.isMetadata = true;
        mft.category = FileCategory::System;
        const ClusterCount perFrag = std::max<ClusterCount>(1, mftTotalClusters / spec.mftFragments);
        Lcn cursor = mftBase;
        Vcn curVcn = 0;
        for (std::uint32_t i = 0; i < spec.mftFragments; ++i) {
            const ClusterCount len = (i + 1 == spec.mftFragments) ? (mftTotalClusters - curVcn) : perFrag;
            if (len == 0) break;
            while (cursor + len <= spec.clusters && !claimRange(cursor, len)) cursor += 3;
            if (cursor + len <= spec.clusters) {
                mft.extents.push_back({curVcn, cursor, len});
                curVcn += len;
                cursor += len + 4; // Gap forces fragmentation when mftFragments > 1
            }
        }
        emu->AddFile(std::move(mft));

        MftInfo minfo{};
        minfo.mftStartLcn = g.mftStartLcn;
        minfo.mft2StartLcn = g.mft2StartLcn;
        minfo.mftValidDataLength = g.mftValidDataLength;
        minfo.totalRecords = spec.mftRecordCount;
        minfo.inUseRecords = spec.mftInUseRecords;
        minfo.freeRecords = spec.mftRecordCount > spec.mftInUseRecords ? (spec.mftRecordCount - spec.mftInUseRecords) : 0;
        minfo.mftMirrMovable = false;
        if (auto mRes = emu->Mft()) {
            minfo.mftExtents = mRes.Value().mftExtents;
            minfo.fragmentCount = mRes.Value().fragmentCount;
        }
        emu->SetMftInfo(minfo);

        FileEntry mirr{};
        mirr.id = kMftMirrFileId;
        mirr.name = "$MFTMirr";
        mirr.path = "\\$MFTMirr";
        mirr.sizeBytes = 4ULL * spec.bytesPerCluster;
        mirr.flags.isMetadata = true;
        mirr.category = FileCategory::System;
        if (claimRange(g.mft2StartLcn, 4)) {
            mirr.extents.push_back({0, g.mft2StartLcn, 4});
        }
        emu->AddFile(std::move(mirr));
    }

    FileId nextId = 10;
    if (spec.includeSystemFiles) {
        const char* sysNames[] = {"$LogFile", "$Bitmap", "pagefile.sys", "hiberfil.sys", "swapfile.sys"};
        for (int i = 0; i < 5; ++i) {
            FileEntry sf{};
            sf.id = nextId++;
            sf.name = sysNames[i];
            sf.path = std::string("\\") + sysNames[i];
            sf.flags.isMetadata = (i < 2);
            sf.flags.isPageOrHiber = (i >= 2);
            sf.category = FileCategory::System;
            sf.sizeBytes = 16ULL * spec.bytesPerCluster;
            Lcn pos = g.mftZoneEnd + 8 + static_cast<Lcn>(i) * 20;
            if (claimRange(pos, 16)) {
                sf.extents.push_back({0, pos, 16});
            }
            emu->AddFile(std::move(sf));
        }
    }

    DeterministicRng rng(spec.seed);
    const ClusterCount targetUsed = static_cast<ClusterCount>(
        (static_cast<double>(spec.clusters) * (100.0 - spec.freeTargetPct)) / 100.0);

    // G-3: pre-populate user files inside MFT Zone if requested
    if (spec.generator == "G-3" || spec.occupiedByUserFilesPct > 0.0) {
        const ClusterCount zoneTotal = g.mftZoneEnd > g.mftZoneStart ? (g.mftZoneEnd - g.mftZoneStart) : 0;
        const ClusterCount wantUser = static_cast<ClusterCount>(
            (static_cast<double>(zoneTotal) * spec.occupiedByUserFilesPct) / 100.0);
        ClusterCount placedInZone = 0;
        Lcn zcur = g.mftZoneStart;
        while (placedInZone < wantUser && zcur < g.mftZoneEnd) {
            if (used[static_cast<std::size_t>(zcur)]) {
                ++zcur;
                continue;
            }
            ClusterCount chunk = std::min<ClusterCount>({8, wantUser - placedInZone, g.mftZoneEnd - zcur});
            while (chunk > 0 && !claimRange(zcur, chunk)) --chunk;
            if (chunk == 0) {
                ++zcur;
                continue;
            }
            FileEntry uf{};
            uf.id = nextId++;
            uf.name = "zone_user_" + std::to_string(uf.id) + ".dat";
            uf.path = "\\ZoneData\\" + uf.name;
            uf.sizeBytes = chunk * spec.bytesPerCluster;
            uf.category = FileCategory::Document;
            uf.flags.inMftZone = true;
            uf.extents.push_back({0, zcur, chunk});
            emu->AddFile(std::move(uf));
            placedInZone += chunk;
            zcur += chunk + 1;
        }
    }

    // Main file generation across G-1..G-6
    const Lcn dataStart = std::min<Lcn>(spec.clusters - 1, g.mftZoneEnd + 4);
    ClusterCount currentUsed = 0;
    for (std::size_t i = 0; i < used.size(); ++i) {
        if (used[i]) ++currentUsed;
    }

    std::uint32_t compRem = spec.compressedCount;
    std::uint32_t sparseRem = spec.sparseCount;
    std::uint32_t encRem = spec.encryptedCount;
    std::uint32_t repRem = spec.reparseCount;
    std::uint32_t lockRem = spec.lockedCount;
    std::uint32_t adsRem = spec.adsCount;

    Lcn scanCursor = dataStart;
    for (std::uint32_t fi = 0; fi < spec.fileCount; ++fi) {
        if (spec.generator == "G-2" && currentUsed >= targetUsed) {
            break;
        }
        FileEntry fe{};
        fe.id = nextId++;

        if (spec.unicodeNames && (fi % 5 == 0)) {
            fe.name = "data_\xE6\x97\xA5\xE6\x9C\xAC_\xF0\x9F\x93\x81_" + std::to_string(fi) + ".bin";
        } else {
            fe.name = "file_" + std::to_string(fi) + (fi % 3 == 0 ? ".zip" : (fi % 3 == 1 ? ".log" : ".dat"));
        }

        if (spec.longPaths && (fi % 7 == 0)) {
            std::string deep = "\\deep";
            for (int d = 0; d < 20; ++d) deep += "\\sub_directory_segment_" + std::to_string(d);
            fe.path = deep + "\\" + fe.name;
        } else {
            fe.path = "\\Users\\Data\\" + fe.name;
        }

        if (compRem > 0) { fe.flags.compressed = true; --compRem; }
        else if (sparseRem > 0) { fe.flags.sparse = true; --sparseRem; }
        else if (encRem > 0) { fe.flags.encrypted = true; --encRem; }
        else if (repRem > 0) { fe.flags.reparsePoint = true; --repRem; }
        else if (lockRem > 0) { fe.flags.isLocked = true; --lockRem; }
        else if (adsRem > 0) { fe.flags.hasAds = true; --adsRem; }

        const bool isResident = (spec.residentPct > 0.0) &&
                                (static_cast<double>(rng.Range(0, 99)) < spec.residentPct);
        if (isResident) {
            fe.flags.resident = true;
            fe.sizeBytes = rng.Range(64, 700);
            fe.category = FileCategory::Document;
            emu->AddFile(std::move(fe));
            continue;
        }

        ClusterCount clustersWanted = rng.Range(spec.sizeMinClusters,
                                                std::max(spec.sizeMinClusters, spec.sizeMaxClusters));
        if (fe.flags.compressed || fe.flags.sparse) {
            clustersWanted = std::max<ClusterCount>(16, ((clustersWanted + 15) / 16) * 16);
        }
        if (spec.generator == "G-2" && currentUsed + clustersWanted > targetUsed) {
            clustersWanted = targetUsed > currentUsed ? (targetUsed - currentUsed) : 1;
        }

        const std::uint32_t exts = std::max<std::uint32_t>(1, spec.extentsPerFile);
        ClusterCount perExt = std::max<ClusterCount>(1, clustersWanted / exts);
        if (fe.flags.compressed || fe.flags.sparse) {
            perExt = std::max<ClusterCount>(16, ((perExt + 15) / 16) * 16);
        }

        Vcn curVcn = 0;
        for (std::uint32_t ei = 0; ei < exts; ++ei) {
            const ClusterCount len = perExt;
            const std::uint32_t align = (fe.flags.compressed || fe.flags.sparse) ? 16U : 1U;
            // Interleave with small gaps to create real fragmentation
            Lcn tryPos = scanCursor;
            if (align > 1) {
                tryPos = ((tryPos + align - 1) / align) * align;
            }
            bool placed = false;
            for (int wrap = 0; wrap < 2 && !placed; ++wrap) {
                while (tryPos + len <= spec.clusters) {
                    if (claimRange(tryPos, len)) {
                        fe.extents.push_back({curVcn, tryPos, len});
                        curVcn += len;
                        currentUsed += len;
                        const ClusterCount gap = (exts > 1) ? rng.Range(2, 6) : 0;
                        scanCursor = tryPos + len + gap;
                        if (scanCursor >= spec.clusters) scanCursor = dataStart;
                        placed = true;
                        break;
                    }
                    tryPos += align;
                }
                if (!placed) {
                    tryPos = dataStart;
                    if (align > 1) tryPos = ((tryPos + align - 1) / align) * align;
                }
            }
        }
        if (!fe.extents.empty()) {
            fe.sizeBytes = curVcn * spec.bytesPerCluster;
            fe.category = (fi % 3 == 0) ? FileCategory::Archive : FileCategory::Document;
            emu->AddFile(std::move(fe));
        }
    }

    // For near-full generators (G-2), pack remaining scattered gaps until targetUsed is reached
    if (spec.generator == "G-2" && currentUsed < targetUsed) {
        Lcn p = dataStart;
        std::uint32_t fillerIdx = 0;
        while (currentUsed < targetUsed && p < spec.clusters) {
            if (used[static_cast<std::size_t>(p)]) {
                ++p;
                continue;
            }
            ClusterCount run = 0;
            while (p + run < spec.clusters && !used[static_cast<std::size_t>(p + run)] &&
                   currentUsed + run < targetUsed) {
                ++run;
            }
            if (run > 0 && claimRange(p, run)) {
                FileEntry ff{};
                ff.id = nextId++;
                ff.name = "filler_" + std::to_string(fillerIdx++) + ".dat";
                ff.path = "\\Users\\Data\\" + ff.name;
                ff.sizeBytes = run * spec.bytesPerCluster;
                ff.category = FileCategory::Archive;
                ff.extents.push_back({0, p, run});
                emu->AddFile(std::move(ff));
                currentUsed += run;
            }
            p += std::max<ClusterCount>(1, run);
        }
    }

    return Result<std::unique_ptr<EmulatorBackend>>::Success(std::move(emu));
}

} // namespace auralite
