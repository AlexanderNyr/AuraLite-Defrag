#pragma once

#include "core/result.hpp"
#include "interfaces/snapshots.hpp"

#include <vector>

namespace auralite {

class IJournal {
public:
    virtual ~IJournal() = default;

    virtual Result<void> OpenOrCreate(std::uint64_t opId, std::uint64_t volumeSerial, std::uint64_t planHash) = 0;
    virtual Result<void> Append(const JournalRecord& record, bool flushNow) = 0;
    virtual Result<Checkpoint> Commit() = 0;
    virtual Result<std::vector<JournalRecord>> Recover() = 0;
    virtual Result<Checkpoint> LastCheckpoint() const = 0;
    virtual Result<void> Seal() = 0;
};

} // namespace auralite
