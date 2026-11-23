#pragma once

#include "core/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace auralite {

#ifdef _WIN32
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept : handle_(h) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    void Reset(HANDLE newHandle = INVALID_HANDLE_VALUE) noexcept {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            ::CloseHandle(handle_);
        }
        handle_ = newHandle;
    }

    HANDLE Get() const noexcept { return handle_; }
    bool Valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};
#endif

struct DiscoveredVolume {
    VolumeId id;
    std::string driveLetter;
    std::string volumeGuidPath;
    std::string label;
    std::string fsName;
    std::uint64_t totalBytes{0};
    std::uint64_t freeBytes{0};
    std::uint32_t serialNumber{0};
    bool isReadOnly{false};
};

class Win32Platform {
public:
    static bool IsCurrentProcessElevated() noexcept;
    static std::wstring Utf8ToWide(std::string_view utf8);
    static std::string WideToUtf8(std::wstring_view wide);
    static std::wstring ToExtendedLengthPath(std::wstring_view path);
    static std::vector<DiscoveredVolume> EnumerateVolumes();
};

} // namespace auralite
