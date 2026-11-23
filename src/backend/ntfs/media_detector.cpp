#include "backend/ntfs/media_detector.hpp"
#include "platform/win32_platform.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <vector>
#endif

namespace auralite {

MediaClass RealMediaDetector::Classify(const VolumeId& volumeId) {
    lastEvidence_ = MediaEvidence{};
#ifndef _WIN32
    lastEvidence_.probeLog.push_back("NonWin32Platform=" + volumeId);
    return MediaClass::Unknown; // Fail-closed
#else
    std::wstring devPath = Win32Platform::Utf8ToWide(volumeId);
    if (devPath.size() == 2 && devPath[1] == L':') {
        devPath = L"\\\\.\\" + devPath;
    } else if (!devPath.starts_with(L"\\\\.\\") && !devPath.starts_with(L"\\\\?\\")) {
        devPath = L"\\\\.\\" + devPath;
    }
    if (!devPath.empty() && devPath.back() == L'\\') {
        devPath.pop_back();
    }

    UniqueHandle hVol(::CreateFileW(devPath.c_str(),
                                    FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    0,
                                    nullptr));
    if (!hVol.Valid()) {
        lastEvidence_.probeLog.push_back("VolumeOpenFailed");
        return MediaClass::Unknown; // Fail-closed
    }

    // 1. Check volume disk extents (D-16)
    std::vector<BYTE> extBuf(sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT));
    DWORD bytesReturned = 0;
    if (!::DeviceIoControl(hVol.Get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                           nullptr, 0, extBuf.data(), static_cast<DWORD>(extBuf.size()),
                           &bytesReturned, nullptr)) {
        lastEvidence_.probeLog.push_back("DiskExtentsFailed");
        return MediaClass::Unknown;
    }

    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(extBuf.data());
    if (extents->NumberOfDiskExtents == 0 || extents->NumberOfDiskExtents > 1) {
        // Multi-disk/tiered/hybrid -> conservative fail-closed Unknown (S-03)
        lastEvidence_.probeLog.push_back("MultiOrZeroExtents");
        return MediaClass::Unknown;
    }

    const DWORD diskNumber = extents->Extents[0].DiskNumber;
    const std::wstring physPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(diskNumber);
    UniqueHandle hDisk(::CreateFileW(physPath.c_str(),
                                     FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr,
                                     OPEN_EXISTING,
                                     0,
                                     nullptr));
    if (!hDisk.Valid()) {
        lastEvidence_.probeLog.push_back("PhysicalDriveOpenFailed");
        return MediaClass::Unknown;
    }

    // 2. StorageDeviceSeekPenaltyProperty (D-09)
    {
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceSeekPenaltyProperty;
        q.QueryType = PropertyStandardQuery;
        DEVICE_SEEK_PENALTY_DESCRIPTOR desc{};
        DWORD ret = 0;
        if (::DeviceIoControl(hDisk.Get(), IOCTL_STORAGE_QUERY_PROPERTY,
                              &q, sizeof(q), &desc, sizeof(desc), &ret, nullptr)) {
            lastEvidence_.seekPenaltyValid = true;
            lastEvidence_.incursSeekPenalty = (desc.IncursSeekPenalty != FALSE);
            lastEvidence_.probeLog.push_back(lastEvidence_.incursSeekPenalty ? "SeekPenalty=true" : "SeekPenalty=false");
        }
    }

    // 3. StorageDeviceTrimProperty (D-09)
    {
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceTrimProperty;
        q.QueryType = PropertyStandardQuery;
        DEVICE_TRIM_DESCRIPTOR desc{};
        DWORD ret = 0;
        if (::DeviceIoControl(hDisk.Get(), IOCTL_STORAGE_QUERY_PROPERTY,
                              &q, sizeof(q), &desc, sizeof(desc), &ret, nullptr)) {
            lastEvidence_.trimValid = true;
            lastEvidence_.trimEnabled = (desc.TrimEnabled != FALSE);
            lastEvidence_.probeLog.push_back(lastEvidence_.trimEnabled ? "Trim=true" : "Trim=false");
        }
    }

    // 4. StorageDeviceProperty (BusType / Removable / Vendor / Product)
    {
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        std::vector<BYTE> buf(1024, 0);
        DWORD ret = 0;
        if (::DeviceIoControl(hDisk.Get(), IOCTL_STORAGE_QUERY_PROPERTY,
                              &q, sizeof(q), buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr)) {
            const auto* sdd = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
            lastEvidence_.busType = static_cast<std::uint32_t>(sdd->BusType);
            lastEvidence_.removableMedia = (sdd->RemovableMedia != FALSE);
            if (sdd->VendorIdOffset > 0 && sdd->VendorIdOffset < ret) {
                lastEvidence_.vendorId = reinterpret_cast<const char*>(buf.data() + sdd->VendorIdOffset);
            }
            if (sdd->ProductIdOffset > 0 && sdd->ProductIdOffset < ret) {
                lastEvidence_.productId = reinterpret_cast<const char*>(buf.data() + sdd->ProductIdOffset);
                if (lastEvidence_.productId.find("Virtual") != std::string::npos ||
                    lastEvidence_.productId.find("Msft") != std::string::npos) {
                    lastEvidence_.isVirtualDisk = true;
                }
            }
            lastEvidence_.probeLog.push_back("BusType=" + std::to_string(lastEvidence_.busType));
        }
    }

    // Fail-closed classification (R-016 / S-03):
    // NVMe bus (BusTypeNvme == 17) or TrimEnabled or SeekPenalty == false => SolidState
    if (lastEvidence_.busType == 17 ||
        (lastEvidence_.trimValid && lastEvidence_.trimEnabled) ||
        (lastEvidence_.seekPenaltyValid && !lastEvidence_.incursSeekPenalty)) {
        return MediaClass::SolidState;
    }
    // USB/SD/MMC bridges that don't confirm seek penalty => Unknown (blocked)
    if (!lastEvidence_.seekPenaltyValid) {
        return MediaClass::Unknown;
    }
    return MediaClass::Spinning;
#endif
}

} // namespace auralite
