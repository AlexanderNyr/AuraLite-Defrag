#include "backend/ntfs/mft/mft_record.hpp"

#include "core/error_catalog.hpp"

#include <algorithm>
#include <cstring>

namespace auralite::mft {
namespace {

std::uint16_t ReadU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t ReadU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(ReadU32(p)) |
           (static_cast<std::uint64_t>(ReadU32(p + 4)) << 32);
}

// UTF-16LE -> UTF-8. Surrogate pairs are combined; an unpaired surrogate becomes
// U+FFFD rather than invalid UTF-8, because this text ends up in CSV exports and
// in exclusion matching, both of which assume valid UTF-8.
std::string Utf16LeToUtf8(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    const std::size_t units = raw.size() / 2;
    auto unit = [&](std::size_t i) {
        return static_cast<std::uint16_t>(static_cast<std::uint8_t>(raw[i * 2]) |
                                         (static_cast<std::uint16_t>(
                                              static_cast<std::uint8_t>(raw[i * 2 + 1]))
                                          << 8));
    };
    auto append = [&out](std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };

    for (std::size_t i = 0; i < units; ++i) {
        const std::uint32_t u = unit(i);
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units) {
            const std::uint32_t lo = unit(i + 1);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                append(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                ++i;
                continue;
            }
            append(0xFFFD);
            continue;
        }
        if (u >= 0xDC00 && u <= 0xDFFF) {
            append(0xFFFD);
            continue;
        }
        append(u);
    }
    return out;
}

} // namespace

bool ApplyFixups(std::string& record, std::uint32_t bytesPerSector) {
    if (record.size() < 48) return false;
    auto* p = reinterpret_cast<std::uint8_t*>(record.data());
    const std::uint16_t usaOffset = ReadU16(p + 0x04);
    const std::uint16_t usaCount = ReadU16(p + 0x06);
    if (usaCount == 0) return false;
    const std::size_t usaBytes = static_cast<std::size_t>(usaCount) * 2U;
    if (static_cast<std::size_t>(usaOffset) + usaBytes > record.size()) return false;

    // Entry 0 is the sequence number the sectors must currently end with.
    const std::uint16_t sequenceCheck = ReadU16(p + usaOffset);
    const std::uint16_t sectors = static_cast<std::uint16_t>(usaCount - 1);
    if (bytesPerSector == 0) return false;
    if (static_cast<std::size_t>(sectors) * bytesPerSector > record.size()) return false;

    for (std::uint16_t i = 0; i < sectors; ++i) {
        const std::size_t end = (static_cast<std::size_t>(i) + 1U) * bytesPerSector;
        if (end > record.size() || end < 2) return false;
        if (ReadU16(p + end - 2) != sequenceCheck) return false;
        const std::uint16_t replacement = ReadU16(p + usaOffset + 2U * (i + 1U));
        p[end - 2] = static_cast<std::uint8_t>(replacement & 0xFF);
        p[end - 1] = static_cast<std::uint8_t>(replacement >> 8);
    }
    return true;
}

Result<std::vector<Extent>> DecodeRunList(std::string_view runList,
                                         std::uint32_t bytesPerCluster) {
    std::vector<Extent> out;
    if (bytesPerCluster == 0) {
        return Result<std::vector<Extent>>::Failure(
            MakeError(ErrorCode::E_ENUM_MALFORMED_RECORD, {}, 0, 0, "runlist: zero cluster size"));
    }

    std::int64_t lcn = 0;
    std::uint64_t vcn = 0;
    std::size_t i = 0;
    while (i < runList.size()) {
        const std::uint8_t header = static_cast<std::uint8_t>(runList[i++]);
        if (header == 0) break; // end of the run list
        const std::size_t lengthBytes = header & 0x0FU;
        const std::size_t offsetBytes = (header >> 4) & 0x0FU;
        if (lengthBytes == 0 || lengthBytes > 8 || offsetBytes > 8) {
            return Result<std::vector<Extent>>::Failure(MakeError(
                ErrorCode::E_ENUM_MALFORMED_RECORD, {}, 0, 0, "runlist: bad field sizes"));
        }
        if (i + lengthBytes + offsetBytes > runList.size()) {
            return Result<std::vector<Extent>>::Failure(MakeError(
                ErrorCode::E_ENUM_MALFORMED_RECORD, {}, 0, 0, "runlist: truncated entry"));
        }

        std::uint64_t count = 0;
        for (std::size_t b = 0; b < lengthBytes; ++b) {
            count |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(runList[i + b])) << (8 * b);
        }
        i += lengthBytes;
        if (count == 0) {
            return Result<std::vector<Extent>>::Failure(
                MakeError(ErrorCode::E_ENUM_MALFORMED_RECORD, {}, 0, 0, "runlist: zero-length run"));
        }

        // The LCN delta is signed and relative to the previous run's LCN.
        std::int64_t delta = 0;
        if (offsetBytes > 0) {
            std::uint64_t raw = 0;
            for (std::size_t b = 0; b < offsetBytes; ++b) {
                raw |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(runList[i + b])) << (8 * b);
            }
            const std::size_t bits = 8 * offsetBytes;
            if (bits < 64 && (raw & (1ULL << (bits - 1))) != 0) {
                raw |= ~((1ULL << bits) - 1ULL);
            }
            delta = static_cast<std::int64_t>(raw);
        }
        i += offsetBytes;

        const std::int64_t runLcn = lcn + delta;
        // offsetBytes == 0 marks a sparse run: it occupies VCNs but no clusters.
        if (offsetBytes > 0) {
            if (runLcn < 0) {
                return Result<std::vector<Extent>>::Failure(MakeError(
                    ErrorCode::E_ENUM_MALFORMED_RECORD, {}, 0, 0, "runlist: negative LCN"));
            }
            out.push_back(Extent{static_cast<Vcn>(vcn), static_cast<Lcn>(runLcn),
                                 static_cast<ClusterCount>(count)});
            lcn = runLcn;
        }
        vcn += count;
    }
    return Result<std::vector<Extent>>::Success(std::move(out));
}

Result<RecordLayout> ParseRecord(std::string_view raw, FileId recordNumber,
                                const RecordGeometry& geometry) {
    if (geometry.recordSize < 48 || raw.size() < geometry.recordSize) {
        return Result<RecordLayout>::Failure(MakeError(ErrorCode::E_ENUM_MALFORMED_RECORD, {}, recordNumber,
                                                       0, "record: short buffer"));
    }
    std::string record(raw.substr(0, geometry.recordSize));
    auto* p = reinterpret_cast<const std::uint8_t*>(record.data());
    if (std::memcmp(p, "FILE", 4) != 0) {
        return Result<RecordLayout>::Failure(
            MakeError(ErrorCode::E_ENUM_MALFORMED_RECORD, {}, recordNumber, 0, "record: bad signature"));
    }
    if (!ApplyFixups(record, 512)) {
        return Result<RecordLayout>::Failure(
            MakeError(ErrorCode::E_ENUM_MALFORMED_RECORD, {}, recordNumber, 0, "record: fixup failed"));
    }
    p = reinterpret_cast<const std::uint8_t*>(record.data());

    RecordLayout out{};
    out.id = recordNumber;
    out.sequence = ReadU16(p + 0x10);
    out.inUse = (ReadU16(p + 0x16) & 0x0001U) != 0;
    out.isDirectory = (ReadU16(p + 0x16) & 0x0002U) != 0;
    out.isExtension = ReadU64(p + 0x20) != 0;

    const std::uint32_t usedSize = std::min<std::uint32_t>(ReadU32(p + 0x18), geometry.recordSize);
    std::uint32_t offset = ReadU16(p + 0x14);
    if (offset < 0x18 || offset >= usedSize) {
        return Result<RecordLayout>::Failure(MakeError(ErrorCode::E_ENUM_MALFORMED_RECORD, {}, recordNumber,
                                                       0, "record: bad first attribute offset"));
    }

    bool haveName = false;
    int bestNameNamespace = -1;
    std::uint64_t dataSize = 0;
    bool haveUnnamedData = false;
    bool haveDataRun = false;

    // Walk the attribute list. Every step re-checks the bounds: this data comes
    // from the disk and a corrupt or hostile record must not walk off the buffer.
    while (offset + 8 <= usedSize) {
        const std::uint8_t* attr = p + offset;
        const std::uint32_t type = ReadU32(attr);
        if (type == 0xFFFFFFFFU) break; // end marker
        const std::uint32_t attrLen = ReadU32(attr + 4);
        if (attrLen < 8 || offset + attrLen > usedSize) {
            return Result<RecordLayout>::Failure(MakeError(ErrorCode::E_ENUM_MALFORMED_RECORD, {}, recordNumber,
                                                           0, "record: bad attribute length"));
        }
        const bool nonResident = attr[8] != 0;
        const std::uint8_t nameLength = attr[9];
        const std::uint16_t attrFlags = ReadU16(attr + 12);
        const bool named = nameLength > 0;

        if (type == kTypeAttributeList) {
            out.hasAttributeList = true;
        } else if (type == kTypeStandardInformation) {
            // Resident only; file attributes live at the end of the value.
            if (!nonResident) {
                const std::uint32_t valueLen = ReadU32(attr + 16);
                const std::uint32_t valueOff = ReadU16(attr + 20);
                if (valueLen >= 36 && 24U + valueOff + 36U <= attrLen) {
                    out.fileAttributes = ReadU32(attr + valueOff + 32);
                }
            }
        } else if (type == kTypeFileName) {
            if (!nonResident) {
                const std::uint32_t valueLen = ReadU32(attr + 16);
                const std::uint32_t valueOff = ReadU16(attr + 20);
                if (valueOff + 66 <= attrLen) {
                    const std::uint8_t* value = attr + valueOff;
                    const std::uint8_t ns = value[65];
                    const std::uint8_t chars = value[64];
                    const std::size_t nameBytes = static_cast<std::size_t>(chars) * 2U;
                    if (nameBytes <= valueLen && valueOff + 66U + nameBytes <= attrLen) {
                        // Namespace 2 is the 8.3 alias; prefer 1 (Win32) or 3 (both).
                        const int rank = (ns == 2) ? 0 : (ns == 0 ? 1 : 2);
                        if (!haveName || rank > bestNameNamespace) {
                            out.parentRef = ReadU64(value);
                            out.fileAttributes = ReadU32(value + 56);
                            out.name = Utf16LeToUtf8(
                                std::string_view(reinterpret_cast<const char*>(value + 66), nameBytes));
                            bestNameNamespace = rank;
                            haveName = true;
                        }
                    }
                }
            }
        } else if (type == kTypeData) {
            if (named) {
                out.hasNamedData = true; // $DATA with a name is an alternate stream
            } else if (nonResident) {
                // Non-resident header layout: value/run fields start at +16.
                if (attrLen < 64) {
                    return Result<RecordLayout>::Failure(MakeError(
                        ErrorCode::E_ENUM_MALFORMED_RECORD, {}, recordNumber, 0, "record: short $DATA"));
                }
                haveUnnamedData = true;
                out.dataFlags = attrFlags;
                dataSize = ReadU64(attr + 48);
                const std::uint16_t runOffset = ReadU16(attr + 32);
                if (runOffset > attrLen) {
                    return Result<RecordLayout>::Failure(MakeError(
                        ErrorCode::E_ENUM_MALFORMED_RECORD, {}, recordNumber, 0, "record: bad run offset"));
                }
                haveDataRun = true;
                if (runOffset < attrLen) {
                    // The run list runs to the end of the attribute; its own zero
                    // terminator ends the decode. A truly empty attribute decodes
                    // to zero runs, which is correct, so no special case is needed.
                    auto runs = DecodeRunList(
                        std::string_view(reinterpret_cast<const char*>(attr + runOffset),
                                         attrLen - runOffset),
                        geometry.bytesPerCluster);
                    if (!runs) return Result<RecordLayout>::Failure(runs.Error());
                    out.extents = std::move(runs.Value());
                }
            } else {
                // Resident $DATA: the whole content sits inside the record.
                const std::uint32_t valueLen = ReadU32(attr + 16);
                if (!haveUnnamedData) {
                    haveUnnamedData = true;
                    out.hasResidentData = true;
                    dataSize = valueLen;
                }
            }
        }

        offset += attrLen;
    }

    if (!haveName) {
        return Result<RecordLayout>::Failure(
            MakeError(ErrorCode::E_ENUM_MALFORMED_RECORD, {}, recordNumber, 0, "record: no $FILE_NAME"));
    }
    out.sizeBytes = dataSize;
    if (!haveUnnamedData) out.sizeBytes = 0;
    if (out.hasResidentData) out.extents.clear();
    (void)haveDataRun;
    return Result<RecordLayout>::Success(std::move(out));
}

} // namespace auralite::mft
