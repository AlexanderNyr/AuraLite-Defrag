// Cluster-map snapshot renderer (P9-T07, acceptance criterion 1).
//
//   auralite-map --scenario tests/scenarios/SC-01.json --out docs/user/images/map.svg
//   auralite-map --scenario ... --mode defrag --out after.svg --when after
//
// The GUI cannot be built on this host, and a screenshot of someone else's
// product is forbidden by the clean-room rules. So the documentation gets
// something better than a mock-up: the actual engine analysing an actual
// scenario, rendered from the bitmap and file records the engine produced.
//
// The image is a *visualisation of data*, not an artist's impression: every
// colour comes from a real cluster's state after a real run.
#include "backend/emulator/emulator_backend.hpp"
#include "backend/emulator/scenario_loader.hpp"
#include "core/error_catalog.hpp"
#include "engine/journal.hpp"
#include "engine/operation_controller.hpp"
#include "engine/policy_gate.hpp"
#include "interfaces/i_progress_sink.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace auralite;

namespace {

class NullSink final : public IProgressSink {
public:
    void OnSnapshot(const ProgressSnapshot&) override {}
    void OnEvent(const LogEvent&) override {}
};

struct Args {
    std::string scenario;
    std::string out;
    std::string mode = "analyze";
    std::string when = "before";
    std::string title;
};

const char* CellFor(const AnalysisSnapshot& snap, Lcn lcn) {
    const BitmapSnapshot& bm = snap.bitmap;
    if (!bm.IsAllocated(lcn)) {
        // Free space inside the MFT zone is drawn differently: that reservation
        // is the thing tier 2 exists to protect.
        if (lcn >= snap.mftZone.zoneStartLcn && lcn < snap.mftZone.zoneEndLcn) return "#e8e4d2";
        return "#f4f4f2";
    }
    if (lcn >= snap.mftZone.zoneStartLcn && lcn < snap.mftZone.zoneEndLcn) return "#b9a44f";
    return "#5f7f97";
}

// Fragmented clusters get a warmer colour, so the "before" image shows the
// problem and the "after" image shows it gone.
std::vector<std::uint8_t> FragmentedMask(const AnalysisSnapshot& snap) {
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(snap.geometry.totalClusters), 0);
    for (const auto& f : snap.files) {
        if (f.extents.size() < 2) continue;
        for (const auto& e : f.extents) {
            for (ClusterCount k = 0; k < e.clusterCount; ++k) {
                const std::uint64_t idx = static_cast<std::uint64_t>(e.startLcn + k);
                if (idx < mask.size()) mask[static_cast<std::size_t>(idx)] = 1;
            }
        }
    }
    return mask;
}

int Render(const Args& args) {
    auto scRes = LoadScenarioFile(args.scenario);
    if (!scRes.Ok()) {
        std::cerr << "map: cannot load scenario " << args.scenario << "\n";
        return 2;
    }
    const auto& spec = scRes.Value();

    auto emuRes = BuildEmulatorFromScenario(spec);
    if (!emuRes.Ok()) {
        std::cerr << "map: cannot build emulator\n";
        return 2;
    }
    auto& emu = *emuRes.Value();

    PolicyGate policy;
    StructuredLogger logger;
    OperationController ctrl(policy, logger);
    NullSink sink;
    CancelToken token;

    PlanOptions opts;
    opts.mftTier = MftTier::Tier3ReportOnly;
    if (args.mode == "defrag") opts.mode = DefragMode::FullDefrag;
    else if (args.mode == "quick") opts.mode = DefragMode::QuickDefrag;
    else if (args.mode == "free-space") opts.mode = DefragMode::DefragFreeSpace;

    AnalysisSnapshot before{};
    AnalysisSnapshot after{};

    std::error_code ec;
    const auto jpath = std::filesystem::temp_directory_path(ec) / "auralite-map.journal";
    std::filesystem::remove(jpath, ec);
    BinaryJournal journal(jpath);

    // dryRun=false so the "after" picture is the real post-operation state.
    const bool dryRun = (args.mode == "analyze");
    auto res = ctrl.RunOperation(emu, emu, opts, sink, journal, token, dryRun, &before, &after);
    std::filesystem::remove(jpath, ec);

    const AnalysisSnapshot& snap = (args.when == "after" && after.geometry.totalClusters > 0) ? after : before;
    if (snap.geometry.totalClusters == 0) {
        std::cerr << "map: engine produced no snapshot";
        if (!res.Ok()) std::cerr << " (error " << static_cast<int>(res.Error().code) << ")";
        std::cerr << "\n";
        return 1;
    }

    const auto total = static_cast<std::uint64_t>(snap.geometry.totalClusters);
    const auto mask = FragmentedMask(snap);

    // Grid: as square as the volume allows, capped so the SVG stays reasonable.
    int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(total))));
    cols = std::clamp(cols, 8, 128);
    const int rows = static_cast<int>((total + static_cast<std::uint64_t>(cols) - 1) /
                                      static_cast<std::uint64_t>(cols));
    const int cell = std::max(2, std::min(8, 640 / cols));
    const int pad = 16;
    const int legendH = 96;
    const int width = cols * cell + 2 * pad;
    const int height = rows * cell + 2 * pad + 40 + legendH;

    std::ofstream out(args.out);
    if (!out) {
        std::cerr << "map: cannot write " << args.out << "\n";
        return 2;
    }

    const std::string title = args.title.empty()
        ? (spec.id + " — " + args.when + " (" + args.mode + ")")
        : args.title;

    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\"" << height
        << "\" viewBox=\"0 0 " << width << " " << height << "\" font-family=\"Segoe UI,Helvetica,Arial,sans-serif\">\n";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";
    out << "<text x=\"" << pad << "\" y=\"" << (pad + 12)
        << "\" font-size=\"13\" font-weight=\"600\" fill=\"#1e1e1e\">" << title << "</text>\n";
    out << "<text x=\"" << pad << "\" y=\"" << (pad + 30)
        << "\" font-size=\"10\" fill=\"#5a5a5a\">" << total << " clusters, "
        << (snap.geometry.bytesPerCluster / 1024) << " KiB each, media "
        << (snap.mediaClass == MediaClass::SolidState ? "solid-state (analysis only)"
                                                      : "spinning")
        << "</text>\n";

    const int gy = pad + 40;
    out << "<g shape-rendering=\"crispEdges\">\n";
    for (int r = 0; r < rows; ++r) {
        int runStart = 0;
        std::string runColor = CellFor(snap, static_cast<Lcn>(r * cols));
        auto flush = [&](int endCol) {
            const int w = (endCol - runStart) * cell;
            if (w <= 0) return;
            out << "<rect x=\"" << (pad + runStart * cell) << "\" y=\"" << (gy + r * cell)
                << "\" width=\"" << w << "\" height=\"" << cell << "\" fill=\"" << runColor << "\"/>\n";
        };
        for (int c = 1; c <= cols; ++c) {
            const std::uint64_t idx = static_cast<std::uint64_t>(r) * static_cast<std::uint64_t>(cols) +
                                      static_cast<std::uint64_t>(c);
            if (idx >= total) break;
            std::string col = CellFor(snap, static_cast<Lcn>(idx));
            if (mask.size() > idx && mask[idx] != 0) col = "#c2683f";
            if (col != runColor) {
                flush(c);
                runStart = c;
                runColor = col;
            }
        }
        flush(cols);
    }
    out << "</g>\n";

    // Legend — the same nine states the GUI legend documents (R-066).
    const int ly = gy + rows * cell + 20;
    struct LegendItem { const char* colour; const char* label; };
    const LegendItem items[] = {
        {"#5f7f97", "used, contiguous"},
        {"#c2683f", "fragmented"},
        {"#b9a44f", "$MFT zone"},
        {"#e8e4d2", "free in $MFT zone"},
        {"#f4f4f2", "free"},
    };
    int lx = pad;
    for (const auto& it : items) {
        out << "<rect x=\"" << lx << "\" y=\"" << ly << "\" width=\"12\" height=\"12\" fill=\""
            << it.colour << "\" stroke=\"#cfcfcf\"/>\n";
        out << "<text x=\"" << (lx + 17) << "\" y=\"" << (ly + 10)
            << "\" font-size=\"10\" fill=\"#3a3a3a\">" << it.label << "</text>\n";
        lx += 17 + static_cast<int>(std::strlen(it.label)) * 6 + 20;
        if (lx > width - 140) { lx = pad; }
    }

    double fragPct = 0.0;
    if (snap.summary.totalClusters > 0) {
        fragPct = snap.summary.volumeFragmentationPct;
    }
    out << "<text x=\"" << pad << "\" y=\"" << (ly + 40) << "\" font-size=\"10\" fill=\"#3a3a3a\">"
        << "files " << snap.files.size() << " · fragmented " << snap.summary.fragmentedFiles
        << " → " << std::fixed << std::setprecision(2) << fragPct << "% of allocated clusters"
        << "</text>\n";

    out << "<text x=\"" << pad << "\" y=\"" << (height - 10)
        << "\" font-size=\"9\" fill=\"#8a8a8a\">Rendered by tools/map_render.cpp from the engine's own "
           "snapshot (scenario " << spec.id << ", seed " << spec.seed << "). No third-party imagery.</text>\n";
    out << "</svg>\n";
    if (!out) {
        std::cerr << "map: write failed\n";
        return 2;
    }

    std::cout << "map_render: wrote " << args.out << " (" << total << " clusters, " << cols << "x"
              << rows << " grid, " << snap.files.size() << " files)\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 < argc) dst = argv[++i];
        };
        if (a == "--scenario") next(args.scenario);
        else if (a == "--out") next(args.out);
        else if (a == "--mode") next(args.mode);
        else if (a == "--when") next(args.when);
        else if (a == "--title") next(args.title);
        else {
            std::cerr << "usage: auralite-map --scenario FILE --out FILE [--mode analyze|defrag|quick|free-space]"
                         " [--when before|after] [--title TEXT]\n";
            return 2;
        }
    }
    if (args.scenario.empty() || args.out.empty()) {
        std::cerr << "map: --scenario and --out are required\n";
        return 2;
    }
    return Render(args);
}
