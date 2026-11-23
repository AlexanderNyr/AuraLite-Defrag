#include "cli/cli_app.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    auralite::CliOptions opts{};
    std::string errMsg;
    if (!auralite::CliApp::ParseArgs(argc, argv, opts, errMsg)) {
        std::cerr << "Error: " << errMsg << "\n";
        return 2;
    }
    return auralite::CliApp::Run(opts, std::cout, std::cerr);
}
