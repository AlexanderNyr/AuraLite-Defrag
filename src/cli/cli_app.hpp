#pragma once

#include "core/config.hpp"
#include "core/logger.hpp"

#include <iosfwd>
#include <string>
#include <vector>

namespace auralite {

struct CliOptions {
    std::string verb;
    std::string volume;
    std::string simScenarioPath;
    std::string targetPath;
    std::string exportCsvPath;
    std::string configPath;
    std::string journalPath;
    std::string format{"text"};
    LogLevel logLevel{LogLevel::Info};
    bool dryRun{false};
    bool confirmMft{false};
    int mftTier{3};
    std::vector<std::string> extraExcludes;
    bool showHelp{false};
};

class CliApp {
public:
    static bool ParseArgs(int argc, const char* const* argv, CliOptions& outOpts, std::string& outError);
    static int Run(const CliOptions& opts, std::ostream& out, std::ostream& err);
    static void PrintHelp(std::ostream& out);
};

} // namespace auralite
