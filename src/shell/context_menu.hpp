#pragma once

#include "core/result.hpp"

#include <string>

namespace auralite {

// Clean-room shell integration ("Defragment with AuraLite-Defrag"):
// - Win10 static verb under HKCU\Software\Classes\*\shell and Directory\shell
// - Win11 modern menu via IExplorerCommand COM server + sparse MSIX identity (D-10)
class ShellContextMenu {
public:
    static Result<void> RegisterPerUserStaticVerb(const std::string& cliExePath);
    static Result<void> UnregisterPerUserStaticVerb();
};

} // namespace auralite
