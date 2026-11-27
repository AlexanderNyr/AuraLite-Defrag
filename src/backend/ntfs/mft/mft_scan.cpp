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

Result<std::string> ReadStreamRange(const std::vector<Extent>& runs, std::uint32_t bytesPerCluster,
                                   std::uint64_t offset, std::size_t length,
                                   const ReadAtFn& readAt) {
    if (bytesPerCluster == 0) {
        return Result<std::string>::Failure(
            MakeError(ErrorCode::E_ENUM_IO, {}, 0, 0, "stream: zero cluster size"));
    }
    std::string out;
    out.resize(length);
    std::size_t done = 0;
    while (done < length) {
        const std::uint64_t streamOffset = offset + done;
        const std::uint64_t vcn = streamOffset / bytesPerCluster;
        const std::uint64_t within = streamOffset % bytesPerCluster;

        const Extent* hit = nullptr;
        for (const auto& run : runs) {
            const std::uint64_t begin = static_cast<std::uint64_t>(run.startVcn);
            const std::uint64_t end = begin + static_cast<std::uint64_t>(run.clusterCount);
            if (vcn >= begin && vcn < end) {
                hit = &run;
                break;
            }
        }
        if (hit == nullptr) {
            return Result<std::string>::Failure(
                MakeError(ErrorCode::E_ENUM_IO, {}, 0, 0, "stream: run list does not cover the range"));
        }

        const std::uint64_t clustersLeft = static_cast<std::uint64_t>(hit->startVcn) +
                                           static_cast<std::uint64_t>(hit->clusterCount) - vcn;
        const std::uint64_t bytesLeftInRun = clustersLeft * bytesPerCluster - within;
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(bytesLeftInRun, static_cast<std::uint64_t>(length - done)));
        const std::uint64_t physical =
            (static_cast<std::uint64_t>(hit->startLcn) + (vcn - hit->startVcn)) * bytesPerCluster + within;

        auto chunk = readAt(physical, want);
        if (!chunk) return Result<std::string>::Failure(chunk.Error());
        if (chunk.Value().size() != want) {
            return Result<std::string>::Failure(
                MakeError(ErrorCode::E_ENUM_IO, {}, 0, 0, "stream: short read"));
        }
        out.replace(done, want, chunk.Value());
        done += want;
    }
    return Result<std::string>::Success(std::move(out));
}

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
