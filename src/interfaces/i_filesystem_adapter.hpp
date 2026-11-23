#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <vector>

namespace auralite {

class IFileSystemAdapter {
public:
    virtual ~IFileSystemAdapter() = default;

    virtual CapabilityBits Capabilities() const = 0;
    virtual Result<std::vector<FileEntry>> Enumerate(std::uint64_t maxFiles) = 0;
    virtual Result<std::vector<Extent>> Extents(FileId fileId) = 0;
    virtual Result<FileFlags> Flags(FileId fileId) = 0;
    virtual MoveResult MoveExtent(FileId fileId, Vcn startVcn, Lcn targetLcn, std::uint32_t clusters) = 0;
    virtual Result<MftInfo> Mft() = 0;
    virtual Result<MftZoneInfo> Zone() = 0;
    virtual Result<std::uint64_t> ComputeWindowChecksum(FileId fileId) = 0;
};

} // namespace auralite
