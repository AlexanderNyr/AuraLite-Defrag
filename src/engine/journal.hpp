#pragma once

#include "interfaces/i_journal.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <fstream>
#include <vector>

namespace auralite {

std::uint32_t ComputeCrc32c(const void* data, std::size_t size) noexcept;

class BinaryJournal final : public IJournal {
public:
    explicit BinaryJournal(std::filesystem::path filePath);
    ~BinaryJournal() override;

    Result<void> OpenOrCreate(std::uint64_t opId, std::uint64_t volumeSerial, std::uint64_t planHash) override;
    Result<void> Append(const JournalRecord& record, bool flushNow) override;
    Result<Checkpoint> Commit() override;
    Result<std::vector<JournalRecord>> Recover() override;
    Result<Checkpoint> LastCheckpoint() const override;
    Result<void> Seal() override;

    const std::filesystem::path& Path() const noexcept { return filePath_; }
    bool HadTrailingPartialRecord() const noexcept { return hadTrailingPartial_; }
    // Non-empty when OpenOrCreate moved an earlier operation's journal aside:
    // "different plan" (the previous run finished and this is a new operation) or
    // "different volume serial" (the volume was recreated). The operator has to be
    // told, because the file is what a resume would have read.
    const std::string& RotationNote() const noexcept { return rotationNote_; }
    std::uint32_t ActiveHeaderSlot() const noexcept { return activeSlot_; }

private:
    Result<void> WriteHeaderSlot(std::uint32_t slotIndex, const Checkpoint& cp);
    Result<Checkpoint> ReadBestHeader(std::uint32_t& outSlot) const;
    void FlushStream();

    std::filesystem::path filePath_;
    std::fstream stream_;
    Checkpoint current_{};
    std::uint32_t activeSlot_{0};
    std::uint64_t writeOffset_{1024};
    std::string rotationNote_;
    Result<void> RotateAside(const std::string& reason);
    bool isOpen_{false};
    bool hadTrailingPartial_{false};
};

} // namespace auralite
