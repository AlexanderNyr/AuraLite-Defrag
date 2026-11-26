#include "backend/ntfs/mft/mft_scan.hpp"

#include "core/error_catalog.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace auralite::mft {
namespace {

// 1 MiB per read: large enough that the sequential read is not dominated by
// syscalls, small enough that it stays out of the large-object heap.
constexpr std::size_t kChunkBytes = 1024U * 1024U;

} // namespace

Result<ScanResult> ScanMft(std::uint64_t validDataLength, const RecordGeometry& geometry,
                           const ReadFn& read, std::uint64_t maxFiles,
                           const std::function<void(std::uint64_t)>& progress) {
    if (geometry.recordSize == 0) {
        return Result<ScanResult>::Failure(
            MakeError(ErrorCode::E_ENUM_MALFORMED_RECORD, {}, 0, 0, "mft: zero record size"));
    }

    ScanResult result{};
    const std::uint64_t totalRecords = validDataLength / geometry.recordSize;

    // A chunk is a whole number of records so a record never straddles two reads.
    const std::uint64_t recordsPerChunk =
        std::max<std::uint64_t>(1, kChunkBytes / geometry.recordSize);

    std::uint64_t recordIndex = 0;
    while (recordIndex < totalRecords) {
        const std::uint64_t batch =
            std::min<std::uint64_t>(recordsPerChunk, totalRecords - recordIndex);
        const std::size_t bytes =
            static_cast<std::size_t>(batch) * static_cast<std::size_t>(geometry.recordSize);
        auto chunk = read(recordIndex * geometry.recordSize, bytes);
        if (!chunk) return Result<ScanResult>::Failure(chunk.Error());
        const std::string& buffer = chunk.Value();
        if (buffer.size() < bytes) {
            return Result<ScanResult>::Failure(MakeError(
                ErrorCode::E_ENUM_IO, {}, 0, 0, "mft: short read of the file table"));
        }

        for (std::uint64_t i = 0; i < batch; ++i) {
            ++result.stats.recordsSeen;
            const FileId id = static_cast<FileId>(recordIndex + i);
            const std::string_view raw(buffer.data() + static_cast<std::size_t>(i) * geometry.recordSize,
                                       geometry.recordSize);

            // Records are pre-allocated, so the majority of a $MFT on a healthy
            // volume is zeroed or marked unused. Only a "FILE" signature is worth
            // parsing, and that keeps a malformed-record flood from being counted
            // as corruption.
            if (raw.size() < 4 || std::memcmp(raw.data(), "FILE", 4) != 0) continue;

            auto parsed = ParseRecord(raw, id, geometry);
            if (!parsed) {
                ++result.stats.malformed;
                continue;
            }
            RecordLayout rec = std::move(parsed.Value());
            if (!rec.inUse) continue;
            ++result.stats.recordsUsed;
            if (rec.isDirectory) ++result.stats.directories;
            if (rec.isExtension) {
                ++result.stats.extensionRecords;
                continue; // its data belongs to the base record
            }
            if (rec.hasAttributeList) {
                ++result.stats.withAttributeList;
                result.needsHandleFallback.push_back(rec.id);
            }
            if (result.records.size() >= maxFiles) {
                // Count what was skipped so the caller can say the list is partial.
                continue;
            }
            result.records.push_back(std::move(rec));
        }

        recordIndex += batch;
        if (progress) progress(result.stats.recordsSeen);
    }

    return Result<ScanResult>::Success(std::move(result));
}

FileEntry EntryFromRecord(const RecordLayout& rec) {
    FileEntry fe{};
    fe.id = rec.id;
    fe.name = rec.name;
    fe.sizeBytes = rec.sizeBytes;
    fe.flags.isDirectory = rec.isDirectory;
    fe.flags.resident = rec.hasResidentData;
    fe.flags.hasAds = rec.hasNamedData;
    fe.flags.isMetadata = (!rec.name.empty() && rec.name[0] == '$');
    fe.flags.sparse = (rec.fileAttributes & kAttrSparse) != 0 ||
                      (rec.dataFlags & kAttrFlagSparse) != 0;
    fe.flags.compressed = (rec.fileAttributes & kAttrCompressed) != 0 ||
                          (rec.dataFlags & kAttrFlagCompressed) != 0;
    fe.flags.encrypted = (rec.fileAttributes & kAttrEncrypted) != 0 ||
                         (rec.dataFlags & kAttrFlagEncrypted) != 0;
    fe.flags.reparsePoint = (rec.fileAttributes & kAttrReparsePoint) != 0;
    fe.extents = rec.extents;
    fe.fragmentCount = fe.extents.empty() ? 0 : static_cast<std::uint32_t>(fe.extents.size());
    for (const auto& e : fe.extents) fe.allocatedClusters += e.clusterCount;
    return fe;
}

} // namespace auralite::mft
