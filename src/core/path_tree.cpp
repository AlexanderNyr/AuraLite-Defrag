#include "core/path_tree.hpp"

#include <vector>

namespace auralite {

namespace {
// A corrupt or hostile volume can describe a cycle or an absurdly deep tree; the
// walk below is bounded so it cannot hang on either.
constexpr std::size_t kMaxDepth = 256;
} // namespace

void PathComposer::AddDirectory(FileId id, FileId parentId, std::string_view name) {
    parent_[id] = parentId;
    name_[id] = std::string(name);
    isDirectory_[id] = true;
}

void PathComposer::AddFile(FileId id, FileId parentId, std::string_view name) {
    parent_[id] = parentId;
    name_[id] = std::string(name);
    isDirectory_[id] = false;
}

void PathComposer::Clear() {
    directoryPathCache_.clear();
    parent_.clear();
    name_.clear();
    isDirectory_.clear();
}

std::string PathComposer::DirectoryPath(FileId id, std::string_view rootPrefix) const {
    // Walk up to the root collecting the directory names, then join them. Cached
    // per directory: a volume with 160k files has ~15k directories, so the walk is
    // amortised to almost nothing.
    const auto cached = directoryPathCache_.find(id);
    if (cached != directoryPathCache_.end()) return cached->second;

    std::vector<FileId> chain;
    FileId current = id;
    bool reachedRoot = false;
    for (std::size_t depth = 0; depth < kMaxDepth; ++depth) {
        if (current == kRootDirectoryId) {
            reachedRoot = true;
            break;
        }
        const auto parentIt = parent_.find(current);
        if (parentIt == parent_.end()) return {}; // unknown ancestor: no usable path
        if (parentIt->second == current) return {}; // degenerate self-parenting record
        chain.push_back(current);
        current = parentIt->second;
    }
    if (!reachedRoot) return {};

    std::string path(rootPrefix);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto nameIt = name_.find(*it);
        if (nameIt == name_.end() || nameIt->second.empty()) return {};
        if (!path.empty() && path.back() != '\\') path.push_back('\\');
        path += nameIt->second;
    }
    directoryPathCache_[id] = path;
    return path;
}

std::string PathComposer::PathOf(FileId id, std::string_view rootPrefix) const {
    const auto parentIt = parent_.find(id);
    const auto nameIt = name_.find(id);
    if (parentIt == parent_.end() || nameIt == name_.end() || nameIt->second.empty()) {
        return {};
    }
    if (id == kRootDirectoryId) return std::string(rootPrefix);

    const std::string dir = DirectoryPath(parentIt->second, rootPrefix);
    if (dir.empty()) return {};
    std::string path = dir;
    if (path.empty() || path.back() != '\\') path.push_back('\\');
    path += nameIt->second;
    return path;
}

} // namespace auralite
