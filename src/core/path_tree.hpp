#pragma once

// Path composition from USN parent references.
//
// A USN record carries a name and a parent file reference number, not a path. The
// previous backend asked the filesystem for each path with
// GetFinalPathNameByHandleW, one call per file, which on a real 320 GB HDD cost
// ~14 ms per file: 160,129 files in ~36 minutes. Walking the parent chain from
// data we already parsed removes that call entirely and works for files that
// could not be opened at all.

#include "core/types.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace auralite {

class PathComposer {
public:
    // The root directory of an NTFS volume. Its own USN record is named "." and
    // its parent is itself.
    static constexpr FileId kRootDirectoryId = 5;

    void AddDirectory(FileId id, FileId parentId, std::string_view name);
    void AddFile(FileId id, FileId parentId, std::string_view name);
    void Clear();

    // Full path below `rootPrefix` (for example "D:\"), or an empty string when the
    // chain does not reach the root - a partial path would be worse than none.
    std::string PathOf(FileId id, std::string_view rootPrefix) const;

    std::size_t Size() const noexcept { return parent_.size(); }

private:
    std::string DirectoryPath(FileId id, std::string_view rootPrefix) const;

    mutable std::unordered_map<FileId, std::string> directoryPathCache_;
    std::unordered_map<FileId, FileId> parent_;
    std::unordered_map<FileId, std::string> name_;
    std::unordered_map<FileId, bool> isDirectory_;
};

} // namespace auralite
