#include "ui/main_window.hpp"
#include "backend/emulator/emulator_backend.hpp"
#include "backend/emulator/scenario_loader.hpp"
#include "backend/ntfs/media_detector.hpp"
#include "backend/ntfs/ntfs_backend.hpp"
#include "core/strings.hpp"
#include "engine/journal.hpp"
#include "platform/win32_platform.hpp"
#include "reporting/csv_exporter.hpp"
#include "ui/wic_exporter.hpp"

#ifdef _WIN32
#include "../../res/strings/ids.h"
#include <commdlg.h>
#include <shellapi.h>
#endif

namespace auralite {

#ifdef _WIN32

namespace {

constexpr UINT WM_AURALITE_SNAPSHOT = WM_APP + 101;
constexpr UINT WM_AURALITE_OP_DONE = WM_APP + 102;
constexpr UINT_PTR kUiTimerId = 1;

class UiProgressSink final : public IProgressSink {
public:
    UiProgressSink(HWND hwnd, std::mutex& mtx, ProgressSnapshot& dst)
        : hwnd_(hwnd), mtx_(mtx), dst_(dst) {}

    void OnSnapshot(const ProgressSnapshot& snapshot) override {
        {
            std::lock_guard lock(mtx_);
            dst_ = snapshot;
        }
        ::PostMessageW(hwnd_, WM_AURALITE_SNAPSHOT, 0, 0);
    }

    void OnEvent(const LogEvent&) override {}

private:
    HWND hwnd_;
    std::mutex& mtx_;
    ProgressSnapshot& dst_;
};

} // namespace

MainWindow::MainWindow()
    : policy_(config_),
      controller_(policy_, logger_),
      palette_(GetThemePalette(ThemeMode::Light)) {}

MainWindow::~MainWindow() {
    cancelToken_.RequestCancel();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::wstring MainWindow::LoadResString(UINT id) const {
    wchar_t buf[1024]{};
    int len = ::LoadStringW(hInst_, id, buf, 1024);
    if (len > 0) return std::wstring(buf, static_cast<std::size_t>(len));
    return L"AuraLite-Defrag";
}

bool MainWindow::Create(HINSTANCE hInst, int nCmdShow) {
    hInst_ = hInst;
    isElevated_ = Win32Platform::IsCurrentProcessElevated();

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES};
    ::InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWindow::WndProc;
    wc.hInstance = hInst_;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AuraLiteDefragMainWindow";
    if (!::RegisterClassExW(&wc)) return false;

    const std::wstring title = LoadResString(IDS_APP_TITLE);
    hwnd_ = ::CreateWindowExW(
        0, wc.lpszClassName, title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
        nullptr, nullptr, hInst_, this);
    if (!hwnd_) return false;

    currentDpi_ = ::GetDpiForWindow(hwnd_);
    CreateMenus();
    CreateControls();
    renderer_.Initialize(hwnd_);
    RefreshVolumeList();
    Relayout(currentDpi_);
    UpdateActionStates();
    ::SetTimer(hwnd_, kUiTimerId, 100, nullptr); // 10 Hz coalesced UI refresh

    ::ShowWindow(hwnd_, nCmdShow);
    ::UpdateWindow(hwnd_);
    return true;
}

int MainWindow::RunMessageLoop() {
    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(msg, wParam, lParam);
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

void MainWindow::CreateMenus() {
    hMenu_ = ::CreateMenu();

    HMENU hFile = ::CreatePopupMenu();
    ::AppendMenuW(hFile, MF_STRING, IDM_FILE_EXPORT_PNG, L"Export Map &PNG...");
    ::AppendMenuW(hFile, MF_STRING, IDM_FILE_EXPORT_CSV, L"Export &CSV...");
    ::AppendMenuW(hFile, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, L"E&xit");

    HMENU hAction = ::CreatePopupMenu();
    ::AppendMenuW(hAction, MF_STRING, IDM_ACTION_ANALYZE, L"&Analyze");
    ::AppendMenuW(hAction, MF_STRING, IDM_ACTION_DEFRAG, L"&Defrag");
    ::AppendMenuW(hAction, MF_STRING, IDM_ACTION_QUICK, L"&Quick Defrag");
    ::AppendMenuW(hAction, MF_STRING, IDM_ACTION_FREE_SPACE, L"Defrag &Free Space");
    ::AppendMenuW(hAction, MF_STRING, IDM_ACTION_MOVE_LARGE, L"Move &Large Files to End");
    ::AppendMenuW(hAction, MF_STRING, IDM_ACTION_DRY_RUN, L"Dry-&Run Simulation");
    ::AppendMenuW(hAction, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(hAction, MF_STRING, IDM_ACTION_PAUSE, L"&Pause");
    ::AppendMenuW(hAction, MF_STRING, IDM_ACTION_RESUME, L"&Resume");
    ::AppendMenuW(hAction, MF_STRING, IDM_ACTION_STOP, L"&Stop");

    HMENU hView = ::CreatePopupMenu();
    ::AppendMenuW(hView, MF_STRING, IDM_VIEW_DRIVE_MAP, L"Drive &Map");
    ::AppendMenuW(hView, MF_STRING, IDM_VIEW_FILE_LIST, L"&File List");
    ::AppendMenuW(hView, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(hView, MF_STRING, IDM_VIEW_REFRESH, L"&Refresh Drives");
    ::AppendMenuW(hView, MF_STRING, IDM_VIEW_TOGGLE_THEME, L"Toggle &Light/Dark Theme");
    ::AppendMenuW(hView, MF_STRING, IDM_VIEW_ZOOM_1X, L"Zoom &1x");
    ::AppendMenuW(hView, MF_STRING, IDM_VIEW_ZOOM_4X, L"Zoom &4x");
    ::AppendMenuW(hView, MF_STRING, IDM_VIEW_ZOOM_16X, L"Zoom 1&6x");

    HMENU hTools = ::CreatePopupMenu();
    ::AppendMenuW(hTools, MF_STRING, IDM_TOOLS_DEMO_MODE, L"Demo / &Simulation Mode");

    HMENU hHelp = ::CreatePopupMenu();
    ::AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT, L"&About AuraLite-Defrag");

    ::AppendMenuW(hMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(hFile), L"&File");
    ::AppendMenuW(hMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(hAction), L"&Action");
    ::AppendMenuW(hMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(hView), L"&View");
    ::AppendMenuW(hMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(hTools), L"&Tools");
    ::AppendMenuW(hMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(hHelp), L"&Help");
    ::SetMenu(hwnd_, hMenu_);
}

void MainWindow::CreateControls() {
    hwndDriveList_ = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"Drive list",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 240, 400, hwnd_, reinterpret_cast<HMENU>(IDC_DRIVE_LIST), hInst_, nullptr);
    ListView_SetExtendedListViewStyle(hwndDriveList_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    auto addCol = [](HWND lv, int idx, const wchar_t* text, int w) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<LPWSTR>(text);
        col.cx = w;
        ListView_InsertColumn(lv, idx, &col);
    };
    addCol(hwndDriveList_, 0, L"Volume", 100);
    addCol(hwndDriveList_, 1, L"FS", 50);
    addCol(hwndDriveList_, 2, L"Media", 75);

    hwndTabs_ = ::CreateWindowExW(
        0, WC_TABCONTROLW, L"Main views",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        250, 0, 600, 32, hwnd_, reinterpret_cast<HMENU>(IDC_MAIN_TABS), hInst_, nullptr);
    TCITEMW t0{TCIF_TEXT, 0, 0, const_cast<LPWSTR>(L"Drive map"), 0, 0, 0};
    TCITEMW t1{TCIF_TEXT, 0, 0, const_cast<LPWSTR>(L"File list"), 0, 0, 0};
    TabCtrl_InsertItem(hwndTabs_, 0, &t0);
    TabCtrl_InsertItem(hwndTabs_, 1, &t1);

    hwndFileList_ = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"File list",
        WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
        250, 40, 600, 400, hwnd_, reinterpret_cast<HMENU>(IDC_FILE_LIST), hInst_, nullptr);
    ListView_SetExtendedListViewStyle(hwndFileList_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    addCol(hwndFileList_, 0, L"Name", 180);
    addCol(hwndFileList_, 1, L"Fragments", 80);
    addCol(hwndFileList_, 2, L"Size", 100);
    addCol(hwndFileList_, 3, L"State", 100);
    addCol(hwndFileList_, 4, L"Path", 300);

    hwndStatus_ = ::CreateWindowExW(
        0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_STATUS_BAR), hInst_, nullptr);
    ::SendMessageW(hwndStatus_, SB_SETTEXTW, 0,
                   reinterpret_cast<LPARAM>(LoadResString(isElevated_ ? IDS_ELEVATION_OK : IDS_ELEVATION_REQUIRED).c_str()));
}

void MainWindow::Relayout(UINT dpi) {
    currentDpi_ = dpi;
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    ::SendMessageW(hwndStatus_, WM_SIZE, 0, 0);
    RECT stRc{};
    ::GetWindowRect(hwndStatus_, &stRc);
    const int statusH = stRc.bottom - stRc.top;

    const int leftW = ::MulDiv(240, static_cast<int>(dpi), 96);
    const int tabH = ::MulDiv(28, static_cast<int>(dpi), 96);
    const int bodyH = std::max(50, h - statusH);

    ::MoveWindow(hwndDriveList_, 4, 4, leftW - 8, bodyH - 8, TRUE);
    ::MoveWindow(hwndTabs_, leftW + 4, 4, std::max(100, w - leftW - 8), tabH, TRUE);
    ::MoveWindow(hwndFileList_, leftW + 4, tabH + 8, std::max(100, w - leftW - 8), std::max(50, bodyH - tabH - 12), TRUE);

    ::ShowWindow(hwndFileList_, activeTab_ == 1 ? SW_SHOW : SW_HIDE);
    renderer_.Resize(static_cast<UINT>(std::max(1, w - leftW - 8)),
                     static_cast<UINT>(std::max(1, bodyH - tabH - 12)));
}

void MainWindow::RefreshVolumeList() {
    drives_.clear();
    ListView_DeleteAllItems(hwndDriveList_);

    if (demoMode_) {
        drives_.push_back({"SIM-DEMO", "Simulation HDD", "NTFS", 64ULL * 1024 * 1024, 30ULL * 1024 * 1024, MediaClass::Spinning, 42.0});
    } else {
        RealMediaDetector det;
        for (const auto& v : Win32Platform::EnumerateVolumes()) {
            DriveRow row{};
            row.id = v.id;
            row.label = v.label.empty() ? v.id : (v.id + " (" + v.label + ")");
            row.fsName = v.fsName;
            row.totalBytes = v.totalBytes;
            row.freeBytes = v.freeBytes;
            row.media = det.Classify(v.id);
            drives_.push_back(std::move(row));
        }
    }

    for (int i = 0; i < static_cast<int>(drives_.size()); ++i) {
        const auto& d = drives_[static_cast<std::size_t>(i)];
        std::wstring col0 = Win32Platform::Utf8ToWide(d.label);
        std::wstring col1 = Win32Platform::Utf8ToWide(d.fsName);
        std::wstring col2 = d.media == MediaClass::Spinning ? L"HDD" : (d.media == MediaClass::SolidState ? L"SSD" : L"Unknown");
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = col0.data();
        ListView_InsertItem(hwndDriveList_, &item);
        ListView_SetItemText(hwndDriveList_, i, 1, col1.data());
        ListView_SetItemText(hwndDriveList_, i, 2, col2.data());
    }
    if (!drives_.empty()) {
        selectedDriveIndex_ = 0;
        ListView_SetItemState(hwndDriveList_, 0, LVIS_SELECTED, LVIS_SELECTED);
    } else {
        selectedDriveIndex_ = -1;
    }
}

void MainWindow::UpdateActionStates() {
    const auto st = controller_.State();
    const bool busy = (st == OperationState::Analyzing || st == OperationState::Defragmenting);
    const bool hasDrive = selectedDriveIndex_ >= 0 && selectedDriveIndex_ < static_cast<int>(drives_.size());
    const MediaClass mc = hasDrive ? drives_[static_cast<std::size_t>(selectedDriveIndex_)].media : MediaClass::Unknown;
    const bool canDefrag = !busy && hasDrive && !IsDefragBlockedMedia(mc) && (demoMode_ || isElevated_);

    auto setItem = [&](UINT id, bool enabled) {
        ::EnableMenuItem(hMenu_, id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    };
    setItem(IDM_ACTION_ANALYZE, !busy && hasDrive);
    setItem(IDM_ACTION_DEFRAG, canDefrag);
    setItem(IDM_ACTION_QUICK, canDefrag);
    setItem(IDM_ACTION_FREE_SPACE, canDefrag);
    setItem(IDM_ACTION_MOVE_LARGE, canDefrag);
    setItem(IDM_ACTION_DRY_RUN, !busy && hasDrive);
    setItem(IDM_ACTION_PAUSE, st == OperationState::Defragmenting);
    setItem(IDM_ACTION_RESUME, st == OperationState::Paused);
    setItem(IDM_ACTION_STOP, busy || st == OperationState::Paused);
}

void MainWindow::StartOperation(DefragMode mode, bool dryRun) {
    if (selectedDriveIndex_ < 0 || selectedDriveIndex_ >= static_cast<int>(drives_.size())) return;
    if (worker_.joinable()) worker_.join();

    cancelToken_.Reset();
    const DriveRow target = drives_[static_cast<std::size_t>(selectedDriveIndex_)];
    const bool isDemo = demoMode_;

    worker_ = std::thread([this, target, mode, dryRun, isDemo]() {
        UiProgressSink sink(hwnd_, stateMutex_, latestProgress_);
        PlanOptions opts{};
        opts.mode = mode;
        opts.dryRun = dryRun;
        opts.fullDefragThresholdPct = config_.fullDefragThresholdPct;

        AnalysisSnapshot beforeSnap{};
        AnalysisSnapshot afterSnap{};

        if (isDemo) {
            ScenarioSpec sc{};
            sc.id = "GUI-DEMO";
            sc.seed = 1001;
            sc.clusters = 16384;
            sc.fileCount = 120;
            sc.extentsPerFile = 5;
            auto emuRes = BuildEmulatorFromScenario(sc);
            if (emuRes) {
                auto& emu = *emuRes.Value();
                BinaryJournal journal(std::filesystem::temp_directory_path() / "auralite-demo.journal");
                (void)controller_.RunOperation(emu, emu, opts, sink, journal, cancelToken_, true, &beforeSnap, &afterSnap);
            }
        } else {
            auto ntfs = std::make_shared<RealNtfsBackend>(target.id);
            NtfsVolumeProxy vp(ntfs);
            NtfsFsProxy fp(ntfs);
            BinaryJournal journal(std::filesystem::temp_directory_path() / "auralite-live.journal");
            (void)controller_.RunOperation(vp, fp, opts, sink, journal, cancelToken_, isElevated_, &beforeSnap, &afterSnap);
        }

        const auto& finalSnap = (mode == DefragMode::Analyze || dryRun) ? beforeSnap : afterSnap;
        {
            std::lock_guard lock(stateMutex_);
            latestAnalysis_ = finalSnap;
            latestGrid_ = ClusterAggregator::BuildGrid(finalSnap, config_.mapCols, config_.mapRows);
        }
        ::PostMessageW(hwnd_, WM_AURALITE_OP_DONE, 0, 0);
    });
    UpdateActionStates();
}

void MainWindow::PopulateFileList() {
    ListView_DeleteAllItems(hwndFileList_);
    std::lock_guard lock(stateMutex_);
    const std::size_t cap = std::min<std::size_t>(latestAnalysis_.files.size(), 5000);
    for (std::size_t i = 0; i < cap; ++i) {
        const auto& f = latestAnalysis_.files[i];
        std::wstring name = Win32Platform::Utf8ToWide(f.name);
        std::wstring frags = std::to_wstring(f.fragmentCount);
        std::wstring sizeStr = Win32Platform::Utf8ToWide(FormatByteSize(f.sizeBytes));
        std::wstring stStr = f.fragmentCount > 1 ? L"Fragmented" : L"Ready";
        std::wstring pathStr = Win32Platform::Utf8ToWide(f.path);

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = name.data();
        ListView_InsertItem(hwndFileList_, &item);
        ListView_SetItemText(hwndFileList_, static_cast<int>(i), 1, frags.data());
        ListView_SetItemText(hwndFileList_, static_cast<int>(i), 2, sizeStr.data());
        ListView_SetItemText(hwndFileList_, static_cast<int>(i), 3, stStr.data());
        ListView_SetItemText(hwndFileList_, static_cast<int>(i), 4, pathStr.data());
    }
}

void MainWindow::SwitchTheme(ThemeMode mode) {
    config_.theme = mode;
    palette_ = GetThemePalette(mode);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::ToggleDemoMode() {
    demoMode_ = !demoMode_;
    RefreshVolumeList();
    UpdateActionStates();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::ExportPng() {
    std::lock_guard lock(stateMutex_);
    auto res = WicPngExporter::ExportClusterMapPng(
        std::filesystem::temp_directory_path() / "auralite-map.png",
        latestGrid_, palette_, 1920, 1080, static_cast<double>(currentDpi_));
    ::MessageBoxW(hwnd_, res ? L"Exported PNG map." : L"PNG export failed.", L"AuraLite-Defrag", MB_OK);
}

void MainWindow::ExportCsv() {
    std::lock_guard lock(stateMutex_);
    const std::string csv = CsvExporter::BuildFileListCsv(latestAnalysis_);
    auto res = CsvExporter::WriteFileAtomic(std::filesystem::temp_directory_path() / "auralite-files.csv", csv);
    ::MessageBoxW(hwnd_, res ? L"Exported CSV file list." : L"CSV export failed.", L"AuraLite-Defrag", MB_OK);
}

void MainWindow::ShowAboutDialog() {
    ::MessageBoxW(hwnd_,
                  L"AuraLite-Defrag v1.0.0\nApache-2.0 License\nNative C++20/Win32 NTFS Online Defragmenter",
                  L"About AuraLite-Defrag", MB_OK | MB_ICONINFORMATION);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            Relayout(currentDpi_);
            return 0;

        case WM_DPICHANGED: {
            const UINT newDpi = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left, suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            Relayout(newDpi);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 900;
            mmi->ptMinTrackSize.y = 560;
            return 0;
        }

        case WM_NOTIFY: {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
            if (hdr->idFrom == IDC_MAIN_TABS && hdr->code == TCN_SELCHANGE) {
                activeTab_ = TabCtrl_GetCurSel(hwndTabs_);
                Relayout(currentDpi_);
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (hdr->idFrom == IDC_DRIVE_LIST && hdr->code == LVN_ITEMCHANGED) {
                selectedDriveIndex_ = ListView_GetNextItem(hwndDriveList_, -1, LVNI_SELECTED);
                UpdateActionStates();
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_FILE_EXIT: ::DestroyWindow(hwnd_); break;
                case IDM_FILE_EXPORT_PNG: ExportPng(); break;
                case IDM_FILE_EXPORT_CSV: ExportCsv(); break;
                case IDM_ACTION_ANALYZE: StartOperation(DefragMode::Analyze, false); break;
                case IDM_ACTION_DEFRAG: StartOperation(DefragMode::FullDefrag, false); break;
                case IDM_ACTION_QUICK: StartOperation(DefragMode::QuickDefrag, false); break;
                case IDM_ACTION_FREE_SPACE: StartOperation(DefragMode::DefragFreeSpace, false); break;
                case IDM_ACTION_MOVE_LARGE: StartOperation(DefragMode::MoveLargeToEnd, false); break;
                case IDM_ACTION_DRY_RUN: StartOperation(DefragMode::FullDefrag, true); break;
                case IDM_ACTION_PAUSE: cancelToken_.RequestPause(); UpdateActionStates(); break;
                case IDM_ACTION_RESUME: cancelToken_.Resume(); UpdateActionStates(); break;
                case IDM_ACTION_STOP: cancelToken_.RequestCancel(); UpdateActionStates(); break;
                case IDM_VIEW_DRIVE_MAP: TabCtrl_SetCurSel(hwndTabs_, 0); activeTab_ = 0; Relayout(currentDpi_); break;
                case IDM_VIEW_FILE_LIST: TabCtrl_SetCurSel(hwndTabs_, 1); activeTab_ = 1; Relayout(currentDpi_); break;
                case IDM_VIEW_REFRESH: RefreshVolumeList(); UpdateActionStates(); break;
                case IDM_VIEW_TOGGLE_THEME:
                    SwitchTheme(config_.theme == ThemeMode::Light ? ThemeMode::Dark : ThemeMode::Light);
                    break;
                case IDM_VIEW_ZOOM_1X: zoomFactor_ = 1; ::InvalidateRect(hwnd_, nullptr, FALSE); break;
                case IDM_VIEW_ZOOM_4X: zoomFactor_ = 4; ::InvalidateRect(hwnd_, nullptr, FALSE); break;
                case IDM_VIEW_ZOOM_16X: zoomFactor_ = 16; ::InvalidateRect(hwnd_, nullptr, FALSE); break;
                case IDM_TOOLS_DEMO_MODE: ToggleDemoMode(); break;
                case IDM_HELP_ABOUT: ShowAboutDialog(); break;
            }
            return 0;

        case WM_TIMER:
        case WM_AURALITE_SNAPSHOT: {
            ProgressSnapshot snap{};
            {
                std::lock_guard lock(stateMutex_);
                snap = latestProgress_;
            }
            const std::wstring statusText = Win32Platform::Utf8ToWide(
                std::string(StringTable::Lookup(snap.statusKey)) + " | " +
                FormatPercent(snap.percentComplete));
            ::SendMessageW(hwndStatus_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(statusText.c_str()));
            if (activeTab_ == 0) ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_AURALITE_OP_DONE:
            PopulateFileList();
            UpdateActionStates();
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd_, &ps);
            if (activeTab_ == 0) {
                std::wstring banner;
                if (demoMode_) {
                    banner = LoadResString(IDS_DEMO_BANNER);
                } else if (selectedDriveIndex_ >= 0 &&
                           IsDefragBlockedMedia(drives_[static_cast<std::size_t>(selectedDriveIndex_)].media)) {
                    banner = LoadResString(IDS_SSD_BLOCKED_BODY);
                }
                std::lock_guard lock(stateMutex_);
                renderer_.RenderClusterMap(hwnd_, latestGrid_, palette_, banner, zoomFactor_);
            }
            ::EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_DESTROY:
            ::KillTimer(hwnd_, kUiTimerId);
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

#endif

} // namespace auralite
