#pragma once
//
// P8-T02 — Exclusions engine (R-037).
//
// The existing PolicyGate::IsFileExcluded handled a single glob against path and
// name plus a directory prefix. That is not enough for the requirement, which
// asks for path / wildcard / extension / size / type rules with deterministic
// matching, Unicode and long-path support, import/export, and enforcement in
// planner, CLI and scheduler.
//
// This module is the single source of truth for "is this file excluded". It sits
// in core (L0) so that every layer that has a FileEntry — planner, CLI,
// scheduler, UI filter — can use the same evaluation instead of reimplementing
// glob semantics three ways.
//
#include "core/config.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace auralite {

// Which dimension a rule matches on. `Path` is the historical behaviour
// (glob against full path, then bare name, then directory prefix).
enum class ExclusionKind : std::uint8_t {
    Path = 0,       // glob against full path and bare filename
    Extension = 1,  // ".tmp", "tmp", "*.tmp" — all mean the same thing
    NameGlob = 2,   // glob against the bare filename only
    Category = 3,   // file category (Audio/Video/Archive/...)
    SizeRange = 4,  // size bounds only, no pattern needed
};

// Mirrors core/types.hpp FileCategory exactly; kept as a separate name because
// the exclusion file uses these strings on the wire ("temp", "diskimage").
// Any divergence would make an exported rule set silently match nothing.
enum class CategoryMatch : std::uint8_t {
    Other = 0,
    Document,
    Archive,
    Media,
    Executable,
    System,
    Temp,
    Database,
    Image,
    DiskImage,
};

struct CompiledExclusion {
    ExclusionKind kind{ExclusionKind::Path};
    bool enabled{true};
    std::string pattern;          // normalised: lowercased, backslashes
    std::uint64_t minSizeBytes{0}; // 0 = unbounded
    std::uint64_t maxSizeBytes{0}; // 0 = unbounded
    CategoryMatch category{CategoryMatch::Other}; // for kind == Category
    // Index of the rule in the source list, so the UI can report which rule bit.
    std::size_t sourceIndex{0};
};

// Result of evaluating one file: not just "excluded", but "why", so the UI can
// show which rule matched (P6-T07 exclusion editor) and the skip report can
// attribute the skip.
struct ExclusionVerdict {
    bool excluded{false};
    std::size_t ruleIndex{0};
    ExclusionKind kind{ExclusionKind::Path};
    std::string matchedPattern;

    explicit operator bool() const noexcept { return excluded; }
};

class ExclusionEngine {
public:
    ExclusionEngine() = default;
    explicit ExclusionEngine(const std::vector<ExclusionRule>& rules);

    // Replace the rule set. Invalid rules are reported, not silently dropped:
    // a typo in a pattern that quietly does nothing is worse than a config error.
    Result<void> SetRules(const std::vector<ExclusionRule>& rules);

    // Total rules, and how many are enabled.
    std::size_t RuleCount() const noexcept { return compiled_.size(); }
    std::size_t EnabledCount() const noexcept;

    // The single question every caller asks.
    ExclusionVerdict Evaluate(const FileEntry& file) const;

    // Convenience wrapper preserving the old PolicyGate signature.
    bool IsExcluded(const FileEntry& file) const noexcept { return Evaluate(file).excluded; }

    // Import/export in the same JSON shape the config file uses, so a rule set
    // can be moved between machines or committed to source control.
    Result<std::vector<ExclusionRule>> ImportJson(std::string_view jsonText) const;
    std::string ExportJson(const std::vector<ExclusionRule>& rules) const;

    // Normalise one rule's pattern in place. Exposed for the UI editor's live
    // validation: the dialog must show the same normal form the engine sees.
    static std::string NormalisePattern(std::string_view pattern, ExclusionKind* kindOut = nullptr);

    // Canonical long-path form: strips \\?\ and \\.\ prefixes, lowercases,
    // converts '/' to '\', collapses duplicate separators. Unicode is left
    // byte-exact — we compare UTF-8 bytes after case folding ASCII only, which
    // is what NTFS upcase tables do for the ASCII range.
    static std::string CanonicalPath(std::string_view path);

    // True when `path` lies on or below `scopePrefix` after both are canonicalised.
    // The boundary is a separator, so "D:\\Data" does not contain "D:\\Database",
    // and a resolved long path (\\?\\D:\\Data\\x) matches a rule written as
    // D:\\Data. Used for --target confinement and for folder selection; matching
    // raw strings instead silently selected nothing on real NTFS (P9-T02 F-02).
    static bool IsWithinScope(std::string_view scopePrefix, std::string_view path) noexcept;

    // Glob match with the documented semantics: '*' matches any run of
    // characters including '\'? No — '*' does not cross a separator, '**'
    // does. '?' matches exactly one character that is not a separator.
    static bool GlobMatch(std::string_view pattern, std::string_view text) noexcept;
    static bool GlobMatchCaseInsensitive(std::string_view pattern, std::string_view text) noexcept;

private:
    static bool GlobMatchImpl(std::string_view pattern, std::string_view text) noexcept;

    std::vector<CompiledExclusion> compiled_;
};

// Map a FileCategory to the CategoryMatch enum used by exclusion rules.
CategoryMatch ToCategoryMatch(FileCategory cat) noexcept;

} // namespace auralite
