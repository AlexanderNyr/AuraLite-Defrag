#pragma once

// Sequential $MFT scan.
//
// The reader is injected: on Windows it is a lambda that reads clusters from the
// volume through the $MFT run list; in tests it is an in-memory image. That is
// the only way this half of the code gets tested on Linux at all.

#include "backend/ntfs/mft/mft_record.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace auralite::mft {

struct ScanStats {
    std::uint64_t recordsSeen{0};
    std::uint64_t recordsUsed{0};
    std::uint64_t extensionRecords{0};
    std::uint64_t withAttributeList{0};
    std::uint64_t malformed{0};
    std::uint64_t directories{0};
};

// Reads exactly `length` bytes of $MFT data starting at `offset` within the
// stream. Returning a short or failed result aborts the scan.
using ReadFn = std::function<Result<std::string>(std::uint64_t offset, std::size_t length)>;

// Reads exactly `length` bytes from a volume at an absolute offset.
using ReadAtFn = std::function<Result<std::string>(std::uint64_t volumeOffset, std::size_t length)>;

// Maps a byte range of a non-contiguous stream onto its runs and reads it.
// $MFT is described by its own run list, which may contain more than one run, so
// "the table" is not one contiguous span; this is the piece that turns stream
// offsets into physical offsets. Portable and therefore tested: a fragmented
// table is the case that would silently misread records otherwise.
Result<std::string> ReadStreamRange(const std::vector<Extent>& runs, std::uint32_t bytesPerCluster,
                                   std::uint64_t offset, std::size_t length,
                                   const ReadAtFn& readAt);

// Records that the handle path has to confirm, because the record itself says
// its attributes are split across records ($ATTRIBUTE_LIST).
struct ScanResult {
    std::vector<RecordLayout> records;
    std::vector<FileId> needsHandleFallback;
    ScanStats stats;
};

// `validDataLength` is geometry.mftValidDataLength: the part of $MFT that holds
// initialised records. Scanning the allocated-but-uninitialised tail produces
// millions of zeroed segments and nothing else.
//
// `progress` is called every 4096 records with the number of records examined so
// far; the analysis already reports progress from the backend and this keeps the
// same rate visible while the scan is the thing taking the time.
Result<ScanResult> ScanMft(std::uint64_t validDataLength, const RecordGeometry& geometry,
                           const ReadFn& read, std::uint64_t maxFiles,
                           const std::function<void(std::uint64_t)>& progress = {});

// Turns one decoded record into the FileEntry the rest of the engine consumes.
// The caller keeps the RecordLayout as well, because it still needs parentRef
// (to compose paths) and sequence (to build the 64-bit reference for
// OpenFileById), neither of which belongs in FileEntry.
FileEntry EntryFromRecord(const RecordLayout& record);

} // namespace auralite::mft
