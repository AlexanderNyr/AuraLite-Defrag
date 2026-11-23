#include "platform/win32_platform.hpp"

namespace auralite {

#ifdef _WIN32

bool Win32Platform::IsCurrentProcessElevated() noexcept {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    UniqueHandle tokenGuard(token);
    TOKEN_ELEVATION elev{};
    DWORD retLen = 0;
    if (!::GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &retLen)) {
        return false;
    }
    return elev.TokenIsElevated != 0;
}

std::wstring Win32Platform::Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             utf8.data(), static_cast<int>(utf8.size()),
                                             nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                          utf8.data(), static_cast<int>(utf8.size()),
                          out.data(), needed);
    return out;
}

std::string Win32Platform::WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0,
                                             wide.data(), static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0,
                          wide.data(), static_cast<int>(wide.size()),
                          out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring Win32Platform::ToExtendedLengthPath(std::wstring_view path) {
    if (path.empty()) return {};
    if (path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\")) {
        return std::wstring(path);
    }
    if (path.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + std::wstring(path.substr(2));
    }
    return L"\\\\?\\" + std::wstring(path);
}

std::vector<DiscoveredVolume> Win32Platform::EnumerateVolumes() {
    std::vector<DiscoveredVolume> list;
    wchar_t volName[MAX_PATH]{};
    HANDLE hFind = ::FindFirstVolumeW(volName, MAX_PATH);
    if (hFind == INVALID_HANDLE_VALUE) return list;

    do {
        DiscoveredVolume dv{};
        dv.volumeGuidPath = WideToUtf8(volName);

        wchar_t paths[4096]{};
        DWORD chars = 0;
        if (::GetVolumePathNamesForVolumeNameW(volName, paths, 4096, &chars) && paths[0] != L'\0') {
            std::wstring firstPath(paths);
            if (firstPath.size() >= 2 && firstPath[1] == L':') {
                dv.driveLetter = WideToUtf8(firstPath.substr(0, 2));
            }
        }
        dv.id = !dv.driveLetter.empty() ? dv.driveLetter : dv.volumeGuidPath;

        wchar_t label[MAX_PATH]{};
        wchar_t fsName[MAX_PATH]{};
        DWORD serial = 0;
        DWORD maxComp = 0;
        DWORD flags = 0;
        if (::GetVolumeInformationW(volName, label, MAX_PATH, &serial, &maxComp, &flags, fsName, MAX_PATH)) {
            dv.label = WideToUtf8(label);
            dv.fsName = WideToUtf8(fsName);
            dv.serialNumber = serial;
            dv.isReadOnly = (flags & FILE_READ_ONLY_VOLUME) != 0;
        }

        ULARGE_INTEGER avail{}, total{}, freeB{};
        if (::GetDiskFreeSpaceExW(volName, &avail, &total, &freeB)) {
            dv.totalBytes = total.QuadPart;
            dv.freeBytes = freeB.QuadPart;
        }
        list.push_back(std::move(dv));
    } while (::FindNextVolumeW(hFind, volName, MAX_PATH));

    ::FindVolumeClose(hFind);
    return list;
}

#else

bool Win32Platform::IsCurrentProcessElevated() noexcept { return false; }
std::wstring Win32Platform::Utf8ToWide(std::string_view) { return {}; }
std::string Win32Platform::WideToUtf8(std::wstring_view) { return {}; }
std::wstring Win32Platform::ToExtendedLengthPath(std::wstring_view path) { return std::wstring(path); }
std::vector<DiscoveredVolume> Win32Platform::EnumerateVolumes() { return {}; }

#endif

} // namespace auralite
