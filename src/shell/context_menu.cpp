#include "shell/context_menu.hpp"
#include "core/error_catalog.hpp"
#include "platform/win32_platform.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW - shlwapi.h does not include this
#include <shobjidl.h>
#include <shlwapi.h>
#endif

namespace auralite {

#ifdef _WIN32

// Clean-room Win11 IExplorerCommand COM implementation (D-10 / S-08)
class AuraLiteExplorerCommand final : public IExplorerCommand {
public:
    explicit AuraLiteExplorerCommand(std::wstring cliPath) : cliPath_(std::move(cliPath)) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IExplorerCommand) {
            *ppv = static_cast<IExplorerCommand*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG r = --refCount_;
        if (r == 0) delete this;
        return r;
    }

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* ppszName) override {
        return ::SHStrDupW(L"Defragment with AuraLite-Defrag", ppszName);
    }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* ppszIcon) override {
        std::wstring iconLoc = cliPath_ + L",0";
        return ::SHStrDupW(iconLoc.c_str(), ppszIcon);
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* ppszInfotip) override {
        return ::SHStrDupW(L"Analyze and defragment selected files or folders", ppszInfotip);
    }
    IFACEMETHODIMP GetCanonicalName(GUID* pguidCommandName) override {
        // Original generated GUID for AuraLite-Defrag
        static constexpr GUID kCmdGuid = {0x9A4E2F11, 0x7C34, 0x4E8D, {0x91, 0x2A, 0x6B, 0x4F, 0x88, 0x0C, 0x31, 0x10}};
        *pguidCommandName = kCmdGuid;
        return S_OK;
    }
    IFACEMETHODIMP GetState(IShellItemArray* psiItemArray, BOOL, EXPCMDSTATE* pCmdState) override {
        DWORD count = 0;
        if (!psiItemArray || FAILED(psiItemArray->GetCount(&count)) || count == 0) {
            *pCmdState = ECS_HIDDEN;
            return S_OK;
        }
        *pCmdState = ECS_ENABLED;
        return S_OK;
    }
    IFACEMETHODIMP Invoke(IShellItemArray* psiItemArray, IBindCtx*) override {
        if (!psiItemArray) return E_INVALIDARG;
        DWORD count = 0;
        if (FAILED(psiItemArray->GetCount(&count)) || count == 0) return S_OK;
        for (DWORD i = 0; i < count; ++i) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(psiItemArray->GetItemAt(i, &item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    DWORD attrs = ::GetFileAttributesW(path);
                    std::wstring verb = (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) ? L"folder" : L"file";
                    std::wstring args = verb + L" --target \"" + std::wstring(path) + L"\"";
                    ::ShellExecuteW(nullptr, L"runas", cliPath_.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
                    ::CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        return S_OK;
    }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* pFlags) override {
        *pFlags = ECF_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** ppEnum) override {
        *ppEnum = nullptr;
        return E_NOTIMPL;
    }

private:
    ~AuraLiteExplorerCommand() = default;
    ULONG refCount_{1};
    std::wstring cliPath_;
};

static Result<void> WriteVerbKey(const wchar_t* subKey, const std::wstring& cmdLine, const std::wstring& exePath) {
    HKEY hKey = nullptr;
    LONG rc = ::RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (rc != ERROR_SUCCESS) {
        return Result<void>::Failure(MakeError(ErrorCode::E_INTERNAL_INVALID_STATE, {}, 0, static_cast<std::uint32_t>(rc)));
    }
    const wchar_t* title = L"Defragment with AuraLite-Defrag";
    ::RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(title),
                     static_cast<DWORD>((wcslen(title) + 1) * sizeof(wchar_t)));
    ::RegSetValueExW(hKey, L"Icon", 0, REG_SZ, reinterpret_cast<const BYTE*>(exePath.c_str()),
                     static_cast<DWORD>((exePath.size() + 1) * sizeof(wchar_t)));

    HKEY hCmd = nullptr;
    if (::RegCreateKeyExW(hKey, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &hCmd, nullptr) == ERROR_SUCCESS) {
        ::RegSetValueExW(hCmd, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmdLine.c_str()),
                         static_cast<DWORD>((cmdLine.size() + 1) * sizeof(wchar_t)));
        ::RegCloseKey(hCmd);
    }
    ::RegCloseKey(hKey);
    return Result<void>::Success();
}

#endif

Result<void> ShellContextMenu::RegisterPerUserStaticVerb(const std::string& cliExePath) {
#ifndef _WIN32
    (void)cliExePath;
    return Result<void>::Failure(MakeError(ErrorCode::E_INTERNAL_INVALID_STATE));
#else
    const std::wstring exeW = Win32Platform::Utf8ToWide(cliExePath);
    const std::wstring fileCmd = L"\"" + exeW + L"\" file --target \"%1\"";
    const std::wstring dirCmd = L"\"" + exeW + L"\" folder --target \"%1\"";
    auto r1 = WriteVerbKey(L"Software\\Classes\\*\\shell\\AuraLiteDefrag", fileCmd, exeW);
    if (!r1) return r1;
    return WriteVerbKey(L"Software\\Classes\\Directory\\shell\\AuraLiteDefrag", dirCmd, exeW);
#endif
}

Result<void> ShellContextMenu::UnregisterPerUserStaticVerb() {
#ifndef _WIN32
    return Result<void>::Failure(MakeError(ErrorCode::E_INTERNAL_INVALID_STATE));
#else
    ::RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\AuraLiteDefrag");
    ::RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\AuraLiteDefrag");
    return Result<void>::Success();
#endif
}

} // namespace auralite
