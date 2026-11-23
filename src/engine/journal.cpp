#include "engine/journal.hpp"
#include "core/error_catalog.hpp"

#include <array>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

namespace auralite {

namespace {

constexpr std::uint32_t kHeaderMagic = 0x484A4C41U; // "ALJH"
constexpr std::uint32_t kRecordMagic = 0x524A4C41U; // "ALJR"
constexpr std::uint16_t kJournalVersion = 1;
constexpr std::size_t kHeaderSlotSize = 512;
constexpr std::uint64_t kDataStartOffset = 1024;

#pragma pack(push, 1)
struct DiskHeaderSlot {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t reservedFlags; // bit 0 = sealed
    std::uint32_t generation;
    std::uint32_t reservedPad;
    std::uint64_t opId;
    std::uint64_t volumeSerial;
    std::uint64_t planHash;
    std::uint64_t lastSequence;
    std::uint64_t lastCompletedStep;
    std::uint64_t checkpointOffset;
    std::uint8_t padding[512 - 64 - 4];
    std::uint32_t crc32c;
};

struct DiskRecord {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t type;
    std::uint32_t length;
    std::uint32_t sequence;
    std::uint64_t opId;
    std::uint64_t stepIndex;
    std::uint64_t fileId;
    std::uint64_t startVcn;
    std::uint64_t sourceLcn;
    std::uint64_t targetLcn;
    std::uint64_t clusterCount;
    std::uint64_t planHash;
    std::uint64_t volumeSerial;
    std::uint32_t extraCode;
    std::uint32_t crc32c;
};
#pragma pack(pop)

static_assert(sizeof(DiskHeaderSlot) == 512, "Header slot must be exactly 512 bytes");
static_assert(sizeof(DiskRecord) == 96, "DiskRecord size fixed");

constexpr std::array<std::uint32_t, 256> MakeCrc32cTable() noexcept {
    std::array<std::uint32_t, 256> t{};
    constexpr std::uint32_t poly = 0x82F63B78U;
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1U) ? ((crc >> 1) ^ poly) : (crc >> 1);
        }
        t[i] = crc;
    }
    return t;
}

constexpr auto kCrc32cTable = MakeCrc32cTable();

bool ValidateHeaderSlot(const DiskHeaderSlot& slot, Checkpoint& outCp) noexcept {
    if (slot.magic != kHeaderMagic || slot.version != kJournalVersion) {
        return false;
    }
    const std::uint32_t expected = ComputeCrc32c(&slot, sizeof(DiskHeaderSlot) - sizeof(std::uint32_t));
    if (expected != slot.crc32c) {
        return false;
    }
    outCp.generation = slot.generation;
    outCp.opId = slot.opId;
    outCp.volumeSerial = slot.volumeSerial;
    outCp.planHash = slot.planHash;
    outCp.lastSequence = slot.lastSequence;
    outCp.lastCompletedStep = slot.lastCompletedStep;
    outCp.byteOffset = slot.checkpointOffset;
    outCp.sealed = (slot.reservedFlags & 1U) != 0;
    return true;
}

} // namespace

std::uint32_t ComputeCrc32c(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc = kCrc32cTable[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

BinaryJournal::BinaryJournal(std::filesystem::path filePath)
    : filePath_(std::move(filePath)) {}

BinaryJournal::~BinaryJournal() {
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
}

void BinaryJournal::FlushStream() {
    if (!stream_.is_open()) return;
    stream_.flush();
#ifndef _WIN32
    // Best-effort OS buffer flush on POSIX
    int fd = ::open(filePath_.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
}

Result<void> BinaryJournal::WriteHeaderSlot(std::uint32_t slotIndex, const Checkpoint& cp) {
    if (!stream_.is_open()) {
        return Result<void>::Failure(MakeError(ErrorCode::E_JOURNAL_IO));
    }
    DiskHeaderSlot slot{};
    slot.magic = kHeaderMagic;
    slot.version = kJournalVersion;
    slot.reservedFlags = cp.sealed ? 1U : 0U;
    slot.generation = cp.generation;
    slot.opId = cp.opId;
    slot.volumeSerial = cp.volumeSerial;
    slot.planHash = cp.planHash;
    slot.lastSequence = cp.lastSequence;
    slot.lastCompletedStep = cp.lastCompletedStep;
    slot.checkpointOffset = cp.byteOffset;
    slot.crc32c = ComputeCrc32c(&slot, sizeof(DiskHeaderSlot) - sizeof(std::uint32_t));

    const std::streamoff offset = static_cast<std::streamoff>(slotIndex * kHeaderSlotSize);
    stream_.clear();
    stream_.seekp(offset, std::ios::beg);
    stream_.write(reinterpret_cast<const char*>(&slot), sizeof(slot));
    if (!stream_) {
        return Result<void>::Failure(MakeError(ErrorCode::E_JOURNAL_IO, {}, 0, 0, "Failed writing journal header slot"));
    }
    FlushStream();
    return Result<void>::Success();
}

Result<Checkpoint> BinaryJournal::ReadBestHeader(std::uint32_t& outSlot) const {
    std::ifstream ifs(filePath_, std::ios::binary);
    if (!ifs) {
        return Result<Checkpoint>::Failure(MakeError(ErrorCode::E_JOURNAL_IO, {}, 0, 0, "Cannot open journal for reading"));
    }

    DiskHeaderSlot slot0{};
    DiskHeaderSlot slot1{};
    ifs.read(reinterpret_cast<char*>(&slot0), sizeof(slot0));
    const bool read0 = ifs.gcount() == static_cast<std::streamsize>(sizeof(slot0));
    ifs.read(reinterpret_cast<char*>(&slot1), sizeof(slot1));
    const bool read1 = ifs.gcount() == static_cast<std::streamsize>(sizeof(slot1));

    Checkpoint cp0{};
    Checkpoint cp1{};
    const bool valid0 = read0 && ValidateHeaderSlot(slot0, cp0);
    const bool valid1 = read1 && ValidateHeaderSlot(slot1, cp1);

    if (!valid0 && !valid1) {
        return Result<Checkpoint>::Failure(MakeError(ErrorCode::E_JOURNAL_CORRUPT_HEADER, {}, 0, 0, "Both journal header slots invalid"));
    }
    if (valid0 && (!valid1 || cp0.generation >= cp1.generation)) {
        outSlot = 0;
        return Result<Checkpoint>::Success(cp0);
    }
    outSlot = 1;
    return Result<Checkpoint>::Success(cp1);
}

Result<void> BinaryJournal::OpenOrCreate(std::uint64_t opId, std::uint64_t volumeSerial, std::uint64_t planHash) {
    std::error_code ec;
    if (filePath_.has_parent_path()) {
        std::filesystem::create_directories(filePath_.parent_path(), ec);
    }

    if (std::filesystem::exists(filePath_, ec) && std::filesystem::file_size(filePath_, ec) >= kDataStartOffset) {
        std::uint32_t slot = 0;
        auto hdr = ReadBestHeader(slot);
        if (!hdr) return Result<void>::Failure(hdr.Error());
        if (hdr.Value().volumeSerial != volumeSerial) {
            return Result<void>::Failure(MakeError(ErrorCode::E_JOURNAL_SERIAL_MISMATCH));
        }
        if (hdr.Value().planHash != planHash) {
            return Result<void>::Failure(MakeError(ErrorCode::E_JOURNAL_PLAN_HASH_MISMATCH));
        }
        current_ = hdr.Value();
        activeSlot_ = slot;

        stream_.open(filePath_, std::ios::binary | std::ios::in | std::ios::out);
        if (!stream_) {
            return Result<void>::Failure(MakeError(ErrorCode::E_JOURNAL_IO));
        }
        stream_.seekp(0, std::ios::end);
        writeOffset_ = static_cast<std::uint64_t>(stream_.tellp());
        if (writeOffset_ < kDataStartOffset) writeOffset_ = kDataStartOffset;
        isOpen_ = true;
        return Result<void>::Success();
    }

    // Create new journal file with two header slots
    stream_.open(filePath_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!stream_) {
        return Result<void>::Failure(MakeError(ErrorCode::E_JOURNAL_IO, {}, 0, 0, "Failed creating journal"));
    }

    std::array<char, kDataStartOffset> zeros{};
    stream_.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    stream_.flush();

    current_ = Checkpoint{};
    current_.generation = 1;
    current_.opId = opId;
    current_.volumeSerial = volumeSerial;
    current_.planHash = planHash;
    current_.byteOffset = kDataStartOffset;
    current_.sealed = false;
    activeSlot_ = 0;
    writeOffset_ = kDataStartOffset;
    isOpen_ = true;

    auto w0 = WriteHeaderSlot(0, current_);
    if (!w0) return w0;
    current_.generation = 2;
    activeSlot_ = 1;
    return WriteHeaderSlot(1, current_);
}

Result<void> BinaryJournal::Append(const JournalRecord& record, bool flushNow) {
    if (!isOpen_ || !stream_.is_open()) {
        return Result<void>::Failure(MakeError(ErrorCode::E_JOURNAL_IO, {}, 0, 0, "Journal not open"));
    }

    DiskRecord dr{};
    dr.magic = kRecordMagic;
    dr.version = kJournalVersion;
    dr.type = static_cast<std::uint16_t>(record.type);
    dr.length = sizeof(DiskRecord);
    dr.sequence = record.sequence;
    dr.opId = record.opId;
    dr.stepIndex = record.stepIndex;
    dr.fileId = record.fileId;
    dr.startVcn = record.startVcn;
    dr.sourceLcn = record.sourceLcn;
    dr.targetLcn = record.targetLcn;
    dr.clusterCount = record.clusterCount;
    dr.planHash = record.planHash;
    dr.volumeSerial = record.volumeSerial;
    dr.extraCode = record.extraCode;
    dr.crc32c = ComputeCrc32c(&dr, sizeof(DiskRecord) - sizeof(std::uint32_t));

    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(writeOffset_), std::ios::beg);
    stream_.write(reinterpret_cast<const char*>(&dr), sizeof(dr));
    if (!stream_) {
        return Result<void>::Failure(MakeError(ErrorCode::E_JOURNAL_IO, {}, 0, 0, "Failed writing journal record"));
    }
    writeOffset_ += sizeof(dr);
    current_.lastSequence = record.sequence;
    if (record.type == JournalRecordType::MoveDone) {
        current_.lastCompletedStep = record.stepIndex;
    }

    if (flushNow) {
        FlushStream();
    }
    return Result<void>::Success();
}

Result<Checkpoint> BinaryJournal::Commit() {
    if (!isOpen_) {
        return Result<Checkpoint>::Failure(MakeError(ErrorCode::E_JOURNAL_IO));
    }
    JournalRecord cpRec{};
    cpRec.sequence = static_cast<std::uint32_t>(current_.lastSequence + 1);
    cpRec.opId = current_.opId;
    cpRec.type = JournalRecordType::Checkpoint;
    cpRec.stepIndex = current_.lastCompletedStep;
    cpRec.planHash = current_.planHash;
    cpRec.volumeSerial = current_.volumeSerial;
    auto app = Append(cpRec, true);
    if (!app) return Result<Checkpoint>::Failure(app.Error());

    current_.generation += 1;
    current_.byteOffset = writeOffset_;
    activeSlot_ = 1U - activeSlot_;
    auto wh = WriteHeaderSlot(activeSlot_, current_);
    if (!wh) return Result<Checkpoint>::Failure(wh.Error());
    return Result<Checkpoint>::Success(current_);
}

Result<std::vector<JournalRecord>> BinaryJournal::Recover() {
    std::uint32_t slot = 0;
    auto hdr = ReadBestHeader(slot);
    if (!hdr) return Result<std::vector<JournalRecord>>::Failure(hdr.Error());
    current_ = hdr.Value();
    activeSlot_ = slot;

    std::ifstream ifs(filePath_, std::ios::binary);
    if (!ifs) {
        return Result<std::vector<JournalRecord>>::Failure(MakeError(ErrorCode::E_JOURNAL_IO));
    }
    ifs.seekg(static_cast<std::streamoff>(kDataStartOffset), std::ios::beg);

    std::vector<JournalRecord> records;
    std::uint64_t validEnd = kDataStartOffset;
    hadTrailingPartial_ = false;

    while (true) {
        DiskRecord dr{};
        ifs.read(reinterpret_cast<char*>(&dr), sizeof(dr));
        const auto got = ifs.gcount();
        if (got == 0) {
            break;
        }
        if (got != static_cast<std::streamsize>(sizeof(dr))) {
            // Torn trailing record -> truncate back to last valid offset
            hadTrailingPartial_ = true;
            break;
        }
        const std::uint32_t expectedCrc = ComputeCrc32c(&dr, sizeof(DiskRecord) - sizeof(std::uint32_t));
        if (dr.magic != kRecordMagic || dr.version != kJournalVersion ||
            dr.length != sizeof(DiskRecord) || expectedCrc != dr.crc32c) {
            // Torn or corrupt trailing record -> stop at last valid record
            hadTrailingPartial_ = true;
            break;
        }
        JournalRecord rec{};
        rec.sequence = dr.sequence;
        rec.opId = dr.opId;
        rec.type = static_cast<JournalRecordType>(dr.type);
        rec.stepIndex = dr.stepIndex;
        rec.fileId = dr.fileId;
        rec.startVcn = dr.startVcn;
        rec.sourceLcn = dr.sourceLcn;
        rec.targetLcn = dr.targetLcn;
        rec.clusterCount = dr.clusterCount;
        rec.planHash = dr.planHash;
        rec.volumeSerial = dr.volumeSerial;
        rec.extraCode = dr.extraCode;
        records.push_back(rec);
        validEnd += sizeof(DiskRecord);

        current_.lastSequence = rec.sequence;
        if (rec.type == JournalRecordType::MoveDone) {
            current_.lastCompletedStep = rec.stepIndex;
        } else if (rec.type == JournalRecordType::Tail) {
            current_.sealed = true;
        }
    }
    ifs.close();

    if (hadTrailingPartial_) {
        std::error_code ec;
        std::filesystem::resize_file(filePath_, validEnd, ec);
    }
    writeOffset_ = validEnd;
    if (stream_.is_open()) {
        stream_.close();
    }
    stream_.open(filePath_, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream_) {
        return Result<std::vector<JournalRecord>>::Failure(MakeError(ErrorCode::E_JOURNAL_IO, {}, 0, 0, "Cannot reopen journal for append after recovery"));
    }
    isOpen_ = true;
    return Result<std::vector<JournalRecord>>::Success(std::move(records));
}

Result<Checkpoint> BinaryJournal::LastCheckpoint() const {
    return Result<Checkpoint>::Success(current_);
}

Result<void> BinaryJournal::Seal() {
    if (!isOpen_) {
        return Result<void>::Failure(MakeError(ErrorCode::E_JOURNAL_IO));
    }
    JournalRecord tail{};
    tail.sequence = static_cast<std::uint32_t>(current_.lastSequence + 1);
    tail.opId = current_.opId;
    tail.type = JournalRecordType::Tail;
    tail.stepIndex = current_.lastCompletedStep;
    tail.planHash = current_.planHash;
    tail.volumeSerial = current_.volumeSerial;
    auto app = Append(tail, true);
    if (!app) return app;

    current_.sealed = true;
    current_.generation += 1;
    current_.byteOffset = writeOffset_;
    activeSlot_ = 1U - activeSlot_;
    auto wh = WriteHeaderSlot(activeSlot_, current_);
    if (stream_.is_open()) {
        stream_.close();
    }
    isOpen_ = false;
    return wh;
}

} // namespace auralite
