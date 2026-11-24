#include "cli/cli_app.hpp"
#include "backend/emulator/emulator_backend.hpp"
#include "backend/emulator/scenario_loader.hpp"
#include "core/error_catalog.hpp"
#include "core/json.hpp"
#include "core/strings.hpp"
#include "engine/journal.hpp"
#include "engine/operation_controller.hpp"
#include "engine/planner.hpp"
#include "engine/policy_gate.hpp"
#include "reporting/csv_exporter.hpp"
#include "reporting/statistics.hpp"

#ifdef _WIN32
#include "backend/ntfs/ntfs_backend.hpp"
#include "platform/win32_platform.hpp"
#endif

#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

// One line describing what the backend actually saw per file. On a healthy
// volume the three groups add up to the file count and "noextent" is zero:
// a non-empty file that Windows reports as having no extents could not have
// its layout read, and that is the case that used to hide as "not fragmented".
std::string ExtentCensus(const std::vector<auralite::FileEntry>& files) {
    std::size_t resident = 0;
    std::size_t single = 0;
    std::size_t multi = 0;
    std::size_t noExtent = 0;
    std::size_t locked = 0;
    std::uint32_t maxExtents = 0;
    for (const auto& f : files) {
        if (f.flags.isDirectory) continue;
        if (f.flags.isLocked) ++locked;
        const std::uint32_t n = static_cast<std::uint32_t>(f.extents.size());
        maxExtents = n > maxExtents ? n : maxExtents;
        if (n == 0) {
            if (f.sizeBytes > 0) {
                ++noExtent;
            } else {
                ++resident;
            }
        } else if (n == 1) {
            ++single;
        } else {
            ++multi;
        }
    }
    std::ostringstream os;
    os << "1ext=" << single << " 2plus=" << multi << " empty=" << resident
       << " noextent=" << noExtent << " locked=" << locked
       << " max=" << maxExtents;
    return os.str();
}

} // namespace

namespace auralite {

namespace {

class ConsoleProgressSink final : public IProgressSink {
public:
    explicit ConsoleProgressSink(std::ostream& out, bool quiet)
        : out_(out), quiet_(quiet) {}

    void OnSnapshot(const ProgressSnapshot& snapshot) override {
        lastSnapshot_ = snapshot;
        if (!quiet_ && snapshot.state == OperationState::Defragmenting) {
            out_ << "[" << FormatPercent(snapshot.percentComplete) << "] "
                 << snapshot.completedSteps << "/" << snapshot.totalSteps
                 << " moves, " << FormatInteger(snapshot.clustersMoved) << " clusters\n";
        }
    }

    void OnEvent(const LogEvent& event) override {
        events_.push_back(event);
    }

    const ProgressSnapshot& LastSnapshot() const noexcept { return lastSnapshot_; }
    const std::vector<LogEvent>& Events() const noexcept { return events_; }

private:
    std::ostream& out_;
    bool quiet_;
    ProgressSnapshot lastSnapshot_{};
    std::vector<LogEvent> events_;
};

class SimVolProxy final : public IVolumeProvider {
public:
    explicit SimVolProxy(std::shared_ptr<EmulatorBackend> p) : b_(std::move(p)) {}
    VolumeId Id() const override { return b_->Id(); }
    Result<VolumeGeometry> Geometry() override { return b_->Geometry(); }
    MediaClass Media() override { return b_->Media(); }
    MediaEvidence Evidence() override { return b_->Evidence(); }
    Result<PreflightResult> Preflight(bool el, double th) override { return b_->Preflight(el, th); }
    Result<BitmapSnapshot> ReadBitmap(Lcn s, ClusterCount c) override { return b_->ReadBitmap(s, c); }
    bool IsSimulation() const noexcept override { return b_->IsSimulation(); }

private:
    std::shared_ptr<EmulatorBackend> b_;
};

class SimFsProxy final : public IFileSystemAdapter {
public:
    explicit SimFsProxy(std::shared_ptr<EmulatorBackend> p) : b_(std::move(p)) {}
    CapabilityBits Capabilities() const override { return b_->Capabilities(); }
    Result<std::vector<FileEntry>> Enumerate(std::uint64_t m) override { return b_->Enumerate(m); }
    Result<std::vector<Extent>> Extents(FileId id) override { return b_->Extents(id); }
    Result<FileFlags> Flags(FileId id) override { return b_->Flags(id); }
    MoveResult MoveExtent(FileId id, Vcn v, Lcn l, std::uint32_t c) override { return b_->MoveExtent(id, v, l, c); }
    Result<MftInfo> Mft() override { return b_->Mft(); }
    Result<MftZoneInfo> Zone() override { return b_->Zone(); }
    Result<std::uint64_t> ComputeWindowChecksum(FileId id) override { return b_->ComputeWindowChecksum(id); }

private:
    std::shared_ptr<EmulatorBackend> b_;
};

bool IsValidVerb(std::string_view v) noexcept {
    return v == "analyze" || v == "defrag" || v == "quick" || v == "free-space" ||
           v == "move-large" || v == "file" || v == "folder" || v == "mft" ||
           v == "status" || v == "export";
}

DefragMode VerbToMode(std::string_view v) noexcept {
    if (v == "analyze" || v == "status" || v == "export") return DefragMode::Analyze;
    if (v == "defrag") return DefragMode::FullDefrag;
    if (v == "quick") return DefragMode::QuickDefrag;
    if (v == "free-space") return DefragMode::DefragFreeSpace;
    if (v == "move-large") return DefragMode::MoveLargeToEnd;
    if (v == "file" || v == "folder") return DefragMode::SingleFileOrFolder;
    if (v == "mft") return DefragMode::MftOptimize;
    return DefragMode::Analyze;
}

} // namespace

void CliApp::PrintHelp(std::ostream& out) {
    out << "AuraLite-Defrag CLI (Apache-2.0)\n"
        << "Usage: auralite-cli <verb> [options]\n\n"
        << "Verbs:\n"
        << "  analyze      Analyze volume fragmentation and MFT layout\n"
        << "  defrag       Full defragmentation (Spinning HDD only)\n"
        << "  quick        Quick defragmentation of multi-extent files\n"
        << "  free-space   Consolidate fragmented free space\n"
        << "  move-large   Relocate large files toward the end of the volume\n"
        << "  file         Defragment a single file (--target <path>)\n"
        << "  folder       Defragment files in a folder subtree (--target <path>)\n"
        << "  mft          Analyze/optimize $MFT via tiered strategy (--mft-tier 1|2|3)\n"
        << "  status       Display volume and MFT status summary\n"
        << "  export       Export analysis/file list to CSV (--export-csv <path>)\n\n"
        << "Options:\n"
        << "  --volume <vol>        Target volume (e.g. D: or \\\\?\\Volume{...})\n"
        << "  --sim <scenario.json> Run against deterministic emulator scenario (no real disk I/O)\n"
        << "  --dry-run             Plan operations and report without moving clusters\n"
        << "  --exclude <pattern>   Additional exclusion glob/prefix (repeatable)\n"
        << "  --target <path>       Target file or folder for 'file'/'folder' verbs\n"
        << "  --mft-tier <1|2|3>    MFT tier (default 3 report-only; tier 1 requires --confirm-mft)\n"
        << "  --confirm-mft         Confirm MFT metadata operation\n"
        << "  --export-csv <path>   Write RFC 4180 CSV report\n"
        << "  --config <path>       Load configuration JSON\n"
        << "  --journal <path>      Journal file path\n"
        << "  --format <text|json>  Output format (default: text)\n"
        << "  --log-level <lvl>     debug | info | warn | error\n"
        << "  --help                Show this help\n\n"
        << "Exit codes: 0=OK, 2=Config/Args, 3=Cancelled, 11=PreflightBlocked, 12=SSDRefused, 13=NotElevated, 20=VerifyFail, 21=JournalFail\n";
}

bool CliApp::ParseArgs(int argc, const char* const* argv, CliOptions& outOpts, std::string& outError) {
    if (argc < 2) {
        outOpts.showHelp = true;
        return true;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            outOpts.showHelp = true;
            return true;
        }
        // Explicitly reject any attempt to force/override SSD policy (A-16 / R-016)
        if (arg == "--force" || arg == "--override-ssd" || arg == "--force-ssd") {
            outError = std::string(StringTable::Lookup("IDS_SSD_NO_OVERRIDE"));
            return false;
        }
        if (arg == "--dry-run") {
            outOpts.dryRun = true;
            continue;
        }
        if (arg == "--confirm-mft") {
            outOpts.confirmMft = true;
            continue;
        }

        auto needVal = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) {
                outError = "Missing value for option: " + std::string(arg);
                return false;
            }
            dst = argv[++i];
            return true;
        };

        if (arg == "--volume") {
            if (!needVal(outOpts.volume)) return false;
        } else if (arg == "--sim") {
            if (!needVal(outOpts.simScenarioPath)) return false;
        } else if (arg == "--target") {
            if (!needVal(outOpts.targetPath)) return false;
        } else if (arg == "--export-csv") {
            if (!needVal(outOpts.exportCsvPath)) return false;
        } else if (arg == "--config") {
            if (!needVal(outOpts.configPath)) return false;
        } else if (arg == "--journal") {
            if (!needVal(outOpts.journalPath)) return false;
        } else if (arg == "--format") {
            if (!needVal(outOpts.format)) return false;
            if (outOpts.format != "text" && outOpts.format != "json") {
                outError = "Invalid --format (expected text or json)";
                return false;
            }
        } else if (arg == "--exclude") {
            std::string pat;
            if (!needVal(pat)) return false;
            outOpts.extraExcludes.push_back(std::move(pat));
        } else if (arg == "--mft-tier") {
            std::string t;
            if (!needVal(t)) return false;
            if (t == "1") outOpts.mftTier = 1;
            else if (t == "2") outOpts.mftTier = 2;
            else if (t == "3") outOpts.mftTier = 3;
            else {
                outError = "Invalid --mft-tier (expected 1, 2, or 3)";
                return false;
            }
        } else if (arg == "--log-level") {
            std::string lvl;
            if (!needVal(lvl)) return false;
            if (lvl == "debug") outOpts.logLevel = LogLevel::Debug;
            else if (lvl == "info") outOpts.logLevel = LogLevel::Info;
            else if (lvl == "warn") outOpts.logLevel = LogLevel::Warn;
            else if (lvl == "error") outOpts.logLevel = LogLevel::Error;
            else {
                outError = "Invalid --log-level";
                return false;
            }
        } else if (!arg.empty() && arg[0] != '-' && outOpts.verb.empty()) {
            outOpts.verb = std::string(arg);
        } else {
            outError = "Unknown argument: " + std::string(arg);
            return false;
        }
    }

    if (!IsValidVerb(outOpts.verb)) {
        outError = "Unknown or missing verb: '" + outOpts.verb + "'";
        return false;
    }
    return true;
}

int CliApp::Run(const CliOptions& opts, std::ostream& out, std::ostream& err) {
    if (opts.showHelp) {
        PrintHelp(out);
        return 0;
    }

    AppConfig cfg{};
    if (!opts.configPath.empty()) {
        auto cfgRes = LoadConfigFile(opts.configPath);
        if (!cfgRes) {
            err << StringTable::Lookup(cfgRes.Error().userMessageKey) << ": "
                << cfgRes.Error().diagnosticDetail << "\n";
            return MapErrorToCliExitCode(cfgRes.Error().code);
        }
        cfg = std::move(cfgRes.Value());
    }
    for (const auto& ex : opts.extraExcludes) {
        ExclusionRule r;
        r.pattern = ex;
        r.enabled = true;
        cfg.exclusions.push_back(std::move(r));
    }

    StructuredLogger logger;
    logger.SetMinLevel(opts.logLevel);
    logger.SetVerbosePaths(cfg.verboseLogPaths);

    PolicyGate policy(cfg);
    OperationController controller(policy, logger);

    std::unique_ptr<IVolumeProvider> volPtr;
    std::unique_ptr<IFileSystemAdapter> fsPtr;
    bool isElevated = false;

    if (!opts.simScenarioPath.empty()) {
        auto scRes = LoadScenarioFile(opts.simScenarioPath);
        if (!scRes) {
            err << StringTable::Lookup(scRes.Error().userMessageKey) << ": "
                << scRes.Error().diagnosticDetail << "\n";
            return MapErrorToCliExitCode(scRes.Error().code);
        }
        auto emuRes = BuildEmulatorFromScenario(scRes.Value());
        if (!emuRes) {
            err << "Failed to initialize simulation volume.\n";
            return 1;
        }
        std::shared_ptr<EmulatorBackend> sharedEmu(emuRes.Value().release());
        volPtr = std::make_unique<SimVolProxy>(sharedEmu);
        fsPtr = std::make_unique<SimFsProxy>(sharedEmu);
        isElevated = true;
    } else {
#ifdef _WIN32
        if (opts.volume.empty()) {
            err << "Missing --volume or --sim.\n";
            return 2;
        }
        isElevated = Win32Platform::IsCurrentProcessElevated();
        auto ntfs = std::make_shared<RealNtfsBackend>(opts.volume);
        volPtr = std::make_unique<NtfsVolumeProxy>(ntfs);
        fsPtr = std::make_unique<NtfsFsProxy>(ntfs);
#else
        err << "Real NTFS volumes are only available on Windows 10/11 x64. Use --sim <scenario.json> on this platform.\n";
        return 2;
#endif
    }

    const DefragMode mode = VerbToMode(opts.verb);
    PlanOptions planOpts{};
    planOpts.mode = mode;
    planOpts.dryRun = opts.dryRun;
    planOpts.fullDefragThresholdPct = cfg.fullDefragThresholdPct;
    planOpts.largeFileThresholdBytes = cfg.largeFileThresholdBytes;
    planOpts.quickMinFragments = cfg.quickDefragMinFragments;
    planOpts.quickMinSizeBytes = cfg.quickDefragMinSizeBytes;
    planOpts.targetFolderPrefix = opts.targetPath;
    planOpts.mftTier = static_cast<MftTier>(opts.mftTier);
    planOpts.mftConfirmed = opts.confirmMft;

    ConsoleProgressSink sink(out, opts.format == "json");
    CancelToken token;

    std::filesystem::path jPath = opts.journalPath.empty()
                                      ? (std::filesystem::temp_directory_path() /
                                         (opts.simScenarioPath.empty()
                                              ? "auralite-cli.journal"
                                              : ("auralite-sim-" + volPtr->Id() + ".journal")))
                                      : std::filesystem::path(opts.journalPath);
    if (opts.journalPath.empty() && !opts.simScenarioPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(jPath, ec);
    }
    BinaryJournal journal(jPath);

    AnalysisSnapshot beforeSnap{};
    AnalysisSnapshot afterSnap{};
    auto opRes = controller.RunOperation(*volPtr, *fsPtr, planOpts, sink, journal, token,
                                         isElevated, &beforeSnap, &afterSnap);

    // Any preflight failure deserves the probe trace: a bare error code on a
    // healthy volume is unactionable, and "Only NTFS volumes are supported" on an
    // NTFS volume is actively misleading.
    const auto printProbeTrace = [&]() {
        const auto ev = volPtr->Evidence();
        err << "Media probe trace:\n";
        if (ev.probeLog.empty()) {
            err << "  (no probes recorded)\n";
        } else {
            for (const auto& step : ev.probeLog) err << "  - " << step << "\n";
        }
        err << "  seekPenalty  valid=" << (ev.seekPenaltyValid ? "yes" : "no")
            << " incurs=" << (ev.incursSeekPenalty ? "yes" : "no") << "\n";
        err << "  trim         valid=" << (ev.trimValid ? "yes" : "no")
            << " enabled=" << (ev.trimEnabled ? "yes" : "no") << "\n";
        err << "  busType      " << ev.busType << "\n";
        err << "  virtualDisk  " << (ev.isVirtualDisk ? "yes" : "no") << "\n";
        err << "  vendor/product " << ev.vendorId << " / " << ev.productId << "\n";
    };

    if (!opRes) {
        if (opRes.Error().code == ErrorCode::E_PREFLIGHT_SSD_BLOCKED) {
            err << StringTable::Lookup("IDS_SSD_CLI_REFUSAL") << "\n";
            printProbeTrace();
            err << "\nWhat the trace means:\n"
                   "  VolumeOpenFailed        - the volume could not be opened; run elevated\n"
                   "  DiskExtentsFailed       - IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS failed\n"
                   "  MultiOrZeroExtents      - the volume spans several disks (spaces/tiered),\n"
                   "                            refused fail-closed. Use a single-disk volume.\n"
                   "  PhysicalDriveOpenFailed - \\\\.\\PhysicalDriveN needs administrator rights\n"
                   "  SeekPenalty=true        - rotational, this would be allowed for defrag\n"
                   "  no SeekPenalty line     - the drive did not answer, which is Unknown,\n"
                   "                            and Unknown is blocked by policy (S-03)\n";
        } else {
            err << StringTable::Lookup(opRes.Error().userMessageKey)
                << " (" << ErrorCodeName(opRes.Error().code) << ")\n";
            printProbeTrace();
            if (opRes.Error().code == ErrorCode::E_PREFLIGHT_NOT_ELEVATED ||
                opRes.Error().code == ErrorCode::E_PREFLIGHT_VOLUME_INACCESSIBLE) {
                err << "\nThis is a privilege problem, not a filesystem problem.\n"
                       "Run the terminal as administrator (the exe's manifest asks for\n"
                       "elevation, but a console launched unelevated cannot pass it on).\n";
            }
        }
        return MapErrorToCliExitCode(opRes.Error().code);
    }

    const auto& reportSnap = (mode == DefragMode::Analyze || opts.dryRun) ? beforeSnap : afterSnap;
    const auto ba = StatisticsEngine::ComputeBeforeAfter(beforeSnap, reportSnap);

    if (!opts.exportCsvPath.empty()) {
        std::string csv = (opts.verb == "export" || opts.verb == "analyze")
                              ? CsvExporter::BuildFileListCsv(reportSnap)
                              : CsvExporter::BuildStatisticsCsv(reportSnap, &ba);
        auto wRes = CsvExporter::WriteFileAtomic(opts.exportCsvPath, csv);
        if (!wRes) {
            err << StringTable::Lookup(wRes.Error().userMessageKey) << "\n";
            return MapErrorToCliExitCode(wRes.Error().code);
        }
    }

    if (opts.format == "json") {
        JsonObject jo;
        jo["volumeId"] = JsonValue{reportSnap.volumeId};
        jo["mode"] = JsonValue{opts.verb};
        jo["dryRun"] = JsonValue{opts.dryRun};
        jo["simulation"] = JsonValue{reportSnap.isSimulation};
        jo["totalClusters"] = JsonValue{static_cast<std::int64_t>(reportSnap.summary.totalClusters)};
        jo["freeClusters"] = JsonValue{static_cast<std::int64_t>(reportSnap.summary.freeClusters)};
        jo["fragmentedFilesBefore"] = JsonValue{static_cast<std::int64_t>(ba.fragmentedFilesBefore)};
        jo["fragmentedFilesAfter"] = JsonValue{static_cast<std::int64_t>(ba.fragmentedFilesAfter)};
        jo["volumeFragBeforePct"] = JsonValue{ba.volumeFragBeforePct};
        jo["volumeFragAfterPct"] = JsonValue{ba.volumeFragAfterPct};
        jo["stepsCompleted"] = JsonValue{static_cast<std::int64_t>(opRes.Value().stepsCompleted)};
        jo["stepsSkipped"] = JsonValue{static_cast<std::int64_t>(opRes.Value().stepsSkipped)};
        jo["mftFragments"] = JsonValue{static_cast<std::int64_t>(reportSnap.mftInfo.fragmentCount)};
        out << DumpJson(JsonValue{std::move(jo)}, 2) << "\n";
    } else {
        if (reportSnap.isSimulation) {
            out << StringTable::Lookup("IDS_DEMO_BANNER") << "\n";
        }
        if (opts.dryRun) {
            out << StringTable::Lookup("IDS_DRY_RUN_HEADER") << "\n";
        }
        out << "Volume: " << reportSnap.volumeId
            << " (" << reportSnap.geometry.fsName << ", "
            << FormatInteger(reportSnap.summary.totalClusters) << " clusters, "
            << FormatInteger(reportSnap.summary.freeClusters) << " free)\n"
            << "Files: " << FormatInteger(reportSnap.summary.totalFiles)
            << " | Fragmented: " << ba.fragmentedFilesBefore << " -> " << ba.fragmentedFilesAfter
            << " | Volume Frag: " << FormatPercent(ba.volumeFragBeforePct)
            << " -> " << FormatPercent(ba.volumeFragAfterPct) << "\n"
            << "MFT: " << reportSnap.mftInfo.fragmentCount << " fragment(s), Zone ["
            << reportSnap.mftZone.zoneStartLcn << ".." << reportSnap.mftZone.zoneEndLcn
            << "), Zone Free: " << reportSnap.mftZone.freeZoneClusters << " clusters\n"
            // Extent census. "Fragmented: 0" is only trustworthy if the extents
            // were actually read, and a file with length but no extents is proof
            // they were not, so both are printed rather than inferred.
            << "Extents: " << ExtentCensus(reportSnap.files) << "\n"
            << "Moves completed: " << opRes.Value().stepsCompleted
            << " | Verified: " << opRes.Value().stepsVerified
            << " | Skipped: " << opRes.Value().stepsSkipped << "\n";
        if (opts.verb == "mft" && opts.mftTier == 3) {
            out << StringTable::Lookup("IDS_MFT_TIER3_REPORT_ONLY") << "\n"
                << StringTable::Lookup("IDS_MFT_ZONE_GUIDANCE") << "\n";
        }
    }

    return 0;
}

} // namespace auralite
