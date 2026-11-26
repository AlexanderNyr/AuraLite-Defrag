#pragma once

// Parser for one NTFS FILE record (MFT record).
//
// Why this exists
// ---------------
// The handle path asks the volume about every file: OpenFileById plus
// FSCTL_GET_RETRIEVAL_POINTERS. That is one random read into the MFT per file,
// and on the reference volume it runs at ~172 files/s even with eight requests
// in flight (5.8 ms per file of seek time, ~15 minutes for 160k files).
//
// The same information - size, allocation, the run list - is already written in
// the file's own MFT record. Reading $MFT once, sequentially, replaces 160,000
// random reads with 314 MB of contiguous reading plus CPU work.
//
// Everything here is deliberately platform-free and depends on nothing but the
// bytes: the same parser is exercised by the Linux test suite and by a fuzzer,
// which a Win32-only implementation could never be.

#include "core/result.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace auralite::mft {

// Sizes that come from the boot sector / volume geometry, not from the record.
struct RecordGeometry {
    std::uint32_t recordSize{1024};      // bytes per FILE record segment
    std::uint32_t bytesPerCluster{4096};
};

// File attribute bits ($STANDARD_INFORMATION / $FILE_NAME), repeated here so the
// backend does not have to include windows.h to name them.
inline constexpr std::uint32_t kAttrDirectory = 0x00000010U;
inline constexpr std::uint32_t kAttrSparse = 0x00000200U;
inline constexpr std::uint32_t kAttrReparsePoint = 0x00000400U;
inline constexpr std::uint32_t kAttrCompressed = 0x00000800U;
inline constexpr std::uint32_t kAttrEncrypted = 0x00004000U;

// Attribute type codes.
inline constexpr std::uint32_t kTypeStandardInformation = 0x10U;
inline constexpr std::uint32_t kTypeAttributeList = 0x20U;
inline constexpr std::uint32_t kTypeFileName = 0x30U;
inline constexpr std::uint32_t kTypeData = 0x80U;

// Non-resident attribute flags.
inline constexpr std::uint16_t kAttrFlagCompressed = 0x0001U;
inline constexpr std::uint16_t kAttrFlagEncrypted = 0x4000U;
inline constexpr std::uint16_t kAttrFlagSparse = 0x8000U;

// One decoded record. Only what the analysis needs; nothing is kept that the
// caller would have to reinterpret.
struct RecordLayout {
    FileId id{0};                 // record index ($MFT entry number)
    std::uint64_t sequence{0};    // 16-bit sequence number widened for refCache_
    std::uint64_t parentRef{0};   // full 64-bit parent reference from $FILE_NAME
    std::string name;             // UTF-8, Win32 namespace preferred over DOS 8.3
    bool inUse{false};
    bool isDirectory{false};
    // A record whose base reference is non-zero is an extension of another
    // record; its $DATA, if any, belongs to the base record.
    bool isExtension{false};
    // $ATTRIBUTE_LIST means some attributes live in other records. The run list
    // decoded here may therefore be incomplete, so the caller must resolve this
    // file through the handle path instead of trusting it.
    bool hasAttributeList{false};
    bool hasResidentData{false};
    bool hasNamedData{false};     // a second (named) $DATA stream exists -> hasAds
    std::uint64_t sizeBytes{0};
    std::uint32_t fileAttributes{0};
    std::uint16_t dataFlags{0};   // compressed / encrypted / sparse bits of $DATA
    std::vector<Extent> extents;  // allocated clusters only; holes are omitted
};

// Parses one record. `recordNumber` is the position in $MFT, used as the FileId
// and to ignore records that report a different number.
//
// A record that is not in use, or whose fixup does not verify, is reported as a
// failure of E_ENUM_MALFORMED_RECORD only when it claims to be valid; the scan
// loop turns that into a counter, because a real volume always carries a few
// unreadable records and one bad record must not abort a 160k-file analysis.
Result<RecordLayout> ParseRecord(std::string_view raw, FileId recordNumber,
                                const RecordGeometry& geometry);

// Applies the update sequence array in place (each 512-byte sector of the record
// ends with a 2-byte fixup). Returns false when the array does not verify, which
// is the standard way a torn or corrupt record is detected.
bool ApplyFixups(std::string& record, std::uint32_t bytesPerSector);

// Decodes a mapping-pairs run list. Runs with no LCN (sparse holes) are skipped
// so the result matches what FSCTL_GET_RETRIEVAL_POINTERS reports, which also
// omits them. Exposed for testability and because the fuzzer drives it directly.
Result<std::vector<Extent>> DecodeRunList(std::string_view runList,
                                         std::uint32_t bytesPerCluster);

} // namespace auralite::mft
