#pragma once

#include "core/config.hpp"
#include "core/logger.hpp"
#include "engine/operation_controller.hpp"
#include "engine/policy_gate.hpp"
#include "reporting/cluster_aggregator.hpp"
#include "ui/d2d_renderer.hpp"
#include "ui/theme.hpp"

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#endif

#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace auralite {

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

#ifdef _WIN32
    bool Create(HINSTANCE hInst, int nCmdShow);
    int RunMessageLoop();
#endif

private:
#ifdef _WIN32
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void CreateControls();
    void CreateMenus();
    void Relayout(UINT dpi);
    void RefreshVolumeList();
    void StartOperation(DefragMode mode, bool dryRun);
    void UpdateActionStates();
    void PopulateFileList();
    void SwitchTheme(ThemeMode mode);
    void ToggleDemoMode();
    void ExportPng();
    void ExportCsv();
    void ShowAboutDialog();
    std::wstring LoadResString(UINT id) const;

    HINSTANCE hInst_{nullptr};
    HWND hwnd_{nullptr};
    HWND hwndToolbar_{nullptr};
    HWND hwndDriveList_{nullptr};
    HWND hwndTabs_{nullptr};
    HWND hwndFileList_{nullptr};
    HWND hwndStatus_{nullptr};
    HMENU hMenu_{nullptr};

    UINT currentDpi_{96};
    int activeTab_{0}; // 0 = Drive map, 1 = File list
    int zoomFactor_{1};
    bool demoMode_{false};
    bool isElevated_{false};

    AppConfig config_{};
    StructuredLogger logger_{};
    PolicyGate policy_{};
    OperationController controller_;
    ThemePalette palette_{};
    D2DRenderer renderer_;

    struct DriveRow {
        VolumeId id;
        std::string label;
        std::string fsName;
        std::uint64_t totalBytes{0};
        std::uint64_t freeBytes{0};
        MediaClass media{MediaClass::Unknown};
        double fragPct{0.0};
    };

    std::vector<DriveRow> drives_;
    int selectedDriveIndex_{-1};

    mutable std::mutex stateMutex_;
    AnalysisSnapshot latestAnalysis_{};
    ClusterGrid latestGrid_{};
    ProgressSnapshot latestProgress_{};

    CancelToken cancelToken_{};
    std::thread worker_;
#endif
};

} // namespace auralite
