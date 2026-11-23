#include "ui/main_window.hpp"

#ifdef _WIN32
#include <windows.h>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auralite::MainWindow win;
    if (!win.Create(hInstance, nCmdShow)) {
        return 1;
    }
    return win.RunMessageLoop();
}
#else
int main() { return 0; }
#endif
