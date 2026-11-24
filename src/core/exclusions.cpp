#include "core/exclusions.hpp"
#include "core/error_catalog.hpp"
#include "core/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace auralite {

namespace {

// ASCII-only lowercase. NTFS upcase tables map non-ASCII according to the
// volume's case table, which we deliberately do not emulate: excluding
// "Файл.txt" when the rule says "файл.txt" is a locale-dependent guess, and a
// wrong guess either over- or under-excludes. Byte-exact comparison outside
// ASCII is the predictable choice, and it is documented as such.
char AsciiLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string AsciiLowerCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(AsciiLower(c));
    return out;
}

bool IsSeparator(char c) noexcept { return c == '\\' || c == '/'; }

bool RegexSafe(std::string_view s, std::string& why) {
    // Reject patterns that cannot be meaningful: an unbalanced '[' or a
    // dangling escape is almost always a typo, and silently treating it as a
    // literal turns a working exclusion into a no-op.
    bool inClass = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inClass) {
            if (c == ']') inClass = false;
            continue;
        }
        if (c == '[') inClass = true;
    }
    if (inClass) {
        why = "unbalanced '[' character class";
        return false;
    }
    return true;
}

const char* CategoryName(CategoryMatch c) noexcept {
    switch (c) {
        case CategoryMatch::Document: return "document";
        case CategoryMatch::Archive: return "archive";
        case CategoryMatch::Media: return "media";
        case CategoryMatch::Executable: return "executable";
        case CategoryMatch::System: return "system";
        case CategoryMatch::Temp: return "temp";
        case CategoryMatch::Database: return "database";
        case CategoryMatch::Image: return "image";
        case CategoryMatch::DiskImage: return "diskimage";
        case CategoryMatch::Other: break;
    }
    return "other";
}

bool ParseCategoryName(std::string_view name, CategoryMatch& out) noexcept {
    if (name == "other") { out = CategoryMatch::Other; return true; }
    if (name == "document") { out = CategoryMatch::Document; return true; }
    if (name == "archive") { out = CategoryMatch::Archive; return true; }
    if (name == "media") { out = CategoryMatch::Media; return true; }
    if (name == "executable") { out = CategoryMatch::Executable; return true; }
    if (name == "system") { out = CategoryMatch::System; return true; }
    if (name == "temp") { out = CategoryMatch::Temp; return true; }
    if (name == "database") { out = CategoryMatch::Database; return true; }
    if (name == "image") { out = CategoryMatch::Image; return true; }
    if (name == "diskimage") { out = CategoryMatch::DiskImage; return true; }
    return false;
}

const char* KindName(ExclusionKind k) noexcept {
    switch (k) {
        case ExclusionKind::Extension: return "extension";
        case ExclusionKind::NameGlob: return "name";
        case ExclusionKind::Category: return "category";
        case ExclusionKind::SizeRange: return "size";
        case ExclusionKind::Path: break;
    }
    return "path";
}

bool ParseKindName(std::string_view name, ExclusionKind& out) noexcept {
    if (name == "path") { out = ExclusionKind::Path; return true; }
    if (name == "extension" || name == "ext") { out = ExclusionKind::Extension; return true; }
    if (name == "name") { out = ExclusionKind::NameGlob; return true; }
    if (name == "category") { out = ExclusionKind::Category; return true; }
    if (name == "size") { out = ExclusionKind::SizeRange; return true; }
    return false;
}

// Extract the lowercased extension including the dot, or "" when absent.
std::string_view ExtensionOf(std::string_view name) noexcept {
    const auto slash = name.find_last_of("\\/");
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    if (slash != std::string_view::npos && dot < slash) return {};
    return name.substr(dot);
}

} // namespace

CategoryMatch ToCategoryMatch(FileCategory cat) noexcept {
    switch (cat) {
        case FileCategory::Document: return CategoryMatch::Document;
        case FileCategory::Archive: return CategoryMatch::Archive;
        case FileCategory::Media: return CategoryMatch::Media;
        case FileCategory::Executable: return CategoryMatch::Executable;
        case FileCategory::System: return CategoryMatch::System;
        case FileCategory::Temp: return CategoryMatch::Temp;
        case FileCategory::Database: return CategoryMatch::Database;
        case FileCategory::Image: return CategoryMatch::Image;
        case FileCategory::DiskImage: return CategoryMatch::DiskImage;
        case FileCategory::Other: break;
    }
    return CategoryMatch::Other;
}

std::string ExclusionEngine::CanonicalPath(std::string_view path) {
    // Long-path prefixes. Both \\?\C:\x and \\.\C:\x mean the same file, and an
    // exclusion written by a user will not have them.
    if (path.size() >= 4 && path[0] == '\\' && path[1] == '\\' &&
        (path[2] == '?' || path[2] == '.') && path[3] == '\\') {
        path.remove_prefix(4);
    }
    // \\?\UNC\server\share -> \\server\share
    if (path.size() >= 8 && path.substr(0, 8) == "UNC\\" ) {
        path.remove_prefix(4);
        std::string out = "\\\\";
        out.append(AsciiLowerCopy(path));
        return out;
    }

    std::string out;
    out.reserve(path.size());
    bool lastWasSep = false;
    for (char c : path) {
        char ch = c;
        if (IsSeparator(ch)) ch = '\\';
        if (ch == '\\') {
            if (lastWasSep) continue;  // collapse runs of separators
            lastWasSep = true;
        } else {
            lastWasSep = false;
        }
        out.push_back(AsciiLower(ch));
    }
    // Trailing separator is not part of a file path.
    while (!out.empty() && out.back() == '\\') out.pop_back();
    return out;
}

bool ExclusionEngine::GlobMatch(std::string_view pattern, std::string_view text) noexcept {
    // A leading "**\\" means "at any depth, including the root". Users write
    // "**\\*.tmp" meaning every temp file on the volume, and it would be a
    // footgun if that silently missed the ones in the volume root. Rather than
    // special-casing it inside the matcher, strip the prefix repeatedly and
    // accept a match at any of those depths.
    std::string_view stripped = pattern;
    while (true) {
        if (GlobMatchImpl(stripped, text)) return true;
        // Try again with one fewer leading "**\\".
        if (stripped.size() >= 3 && stripped[0] == '*' && stripped[1] == '*' &&
            IsSeparator(stripped[2])) {
            stripped.remove_prefix(3);
            continue;
        }
        return false;
    }
}

bool ExclusionEngine::GlobMatchImpl(std::string_view pattern, std::string_view text) noexcept {
    // Iterative wildcard matcher with a backtrack *stack*.
    //
    // A single backtrack pointer is not sufficient once '*' is forbidden from
    // crossing a separator. With the pattern "**\*.tmp" against
    // "a\b\file.tmp", the inner '*' must stop at the second separator, at which
    // point the correct move is to go back to the outer '**' and let it absorb
    // "a\b". Remembering only the most recent star loses that option and the
    // match fails. A stack of pending stars fixes it, and stays O(n*m) worst
    // case with no recursion, so a hostile pattern cannot exhaust the stack.
    //
    //   '*'   matches any run of characters except a separator
    //   '**'  matches any run including separators
    //   '?'   matches exactly one non-separator character
    //   [..]  character class, '!' or '^' negates, 'a-z' ranges
    struct Frame {
        std::size_t patternAfterStars;
        std::size_t textPos;
        bool crossesSeparator;
    };
    // Fixed capacity: no allocation inside a noexcept function. Adjacent stars
    // collapse into one frame, so only a pattern with more than 128 separate
    // star runs could overflow — and such a pattern is already pathological.
    constexpr std::size_t kMaxFrames = 128;
    Frame stack[kMaxFrames];
    std::size_t depth = 0;

    const auto classMatch = [&](std::size_t& ci, char ch) -> bool {
        std::size_t i = ci + 1;
        bool negate = false;
        if (i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^')) {
            negate = true;
            ++i;
        }
        bool matched = false;
        bool first = true;
        while (i < pattern.size() && (pattern[i] != ']' || first)) {
            first = false;
            if (i + 2 < pattern.size() && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
                const char lo = pattern[i], hi = pattern[i + 2];
                if (ch >= lo && ch <= hi) matched = true;
                i += 3;
            } else {
                if (pattern[i] == ch) matched = true;
                ++i;
            }
        }
        if (i >= pattern.size()) return false;
        ci = i + 1;
        return negate ? !matched : matched;
    };

    std::size_t p = 0, t = 0;
    while (true) {
        if (t >= text.size()) {
            // Only stars may remain in the pattern.
            while (p < pattern.size() && pattern[p] == '*') ++p;
            if (p == pattern.size()) return true;
            // Otherwise the trailing literals need characters we do not have;
            // fall through to backtracking.
        } else if (p < pattern.size()) {
            const char pc = pattern[p];
            if (pc == '*') {
                std::size_t n = 0;
                while (p + n < pattern.size() && pattern[p + n] == '*') ++n;
                const bool crosses = (n >= 2);
                p += n;
                if (p == pattern.size()) return true; // trailing star eats the rest
                if (depth < kMaxFrames) {
                    stack[depth++] = Frame{p, t, crosses};
                }
                continue;
            }
            if (pc == '?') {
                if (!IsSeparator(text[t])) { ++p; ++t; continue; }
            } else if (pc == '[') {
                std::size_t ci = p;
                if (classMatch(ci, text[t])) { p = ci; ++t; continue; }
            } else if (AsciiLower(pc) == AsciiLower(text[t])) {
                ++p; ++t; continue;
            }
        }

        // Mismatch (or text exhausted with literals left): unwind to the most
        // recent star that can still grow.
        bool advanced = false;
        while (depth > 0) {
            Frame& f = stack[depth - 1];
            if (f.textPos >= text.size()) { --depth; continue; }
            if (!f.crossesSeparator && IsSeparator(text[f.textPos])) { --depth; continue; }
            ++f.textPos;
            t = f.textPos;
            p = f.patternAfterStars;
            advanced = true;
            break;
        }
        if (!advanced) return false;
    }
}

bool ExclusionEngine::GlobMatchCaseInsensitive(std::string_view pattern, std::string_view text) noexcept {
    // The matcher lowercases while comparing, so this is the same entry point;
    // kept separate because the public contract distinguishes them and callers
    // read better with the explicit name.
    return GlobMatch(pattern, text);
}

std::string ExclusionEngine::NormalisePattern(std::string_view pattern, ExclusionKind* kindOut) {
    // A pattern may itself be written in long-path form; the file side is
    // canonicalised by CanonicalPath(), which strips the prefix, so the pattern
    // must be stripped identically or the two can never compare equal.
    std::string_view view = pattern;
    if (view.size() >= 4 && view[0] == '\\' && view[1] == '\\' &&
        (view[2] == '?' || view[2] == '.') && view[3] == '\\') {
        view.remove_prefix(4);
    }
    std::string p = AsciiLowerCopy(view);
    ExclusionKind kind = ExclusionKind::Path;

    // "ext:.tmp", "category:temp", "name:*.log", "size" prefixes are a compact
    // authored form; the JSON form uses an explicit "kind" field. Both land here.
    if (p.rfind("ext:", 0) == 0) {
        kind = ExclusionKind::Extension;
        p.erase(0, 4);
    } else if (p.rfind("category:", 0) == 0) {
        kind = ExclusionKind::Category;
        p.erase(0, 9);
    } else if (p.rfind("name:", 0) == 0) {
        kind = ExclusionKind::NameGlob;
        p.erase(0, 5);
    } else if (p.rfind("size:", 0) == 0) {
        kind = ExclusionKind::SizeRange;
        p.erase(0, 5);
    } else if (!p.empty() && p[0] == '.' && p.find_first_of("*?[") == std::string::npos) {
        // A bare ".tmp" is an extension rule, not a path.
        kind = ExclusionKind::Extension;
    } else if (p.size() >= 2 && p[0] == '*' && p[1] == '.' &&
               p.find_first_of("*?[", 1) == std::string::npos) {
        // "*.tmp" is an extension rule too.
        kind = ExclusionKind::Extension;
        p.erase(0, 1); // drop the '*'
    }

    if (kind == ExclusionKind::Extension && !p.empty() && p[0] != '.') {
        p.insert(p.begin(), '.');
    }

    // Separators are normalised so a rule written with '/' matches a path
    // stored with '\'. A single trailing separator is kept: it is how the author
    // says "this is a directory". Collapsing it away silently turned
    // "c:\windows\" into the bare word "c:\windows", which then matched
    // nothing. Runs of separator are collapsed to one.
    for (char& c : p) {
        if (c == '/') c = '\\';
    }
    std::string collapsed;
    collapsed.reserve(p.size());
    for (char c : p) {
        if (c == '\\' && !collapsed.empty() && collapsed.back() == '\\') continue;
        collapsed.push_back(c);
    }
    p = std::move(collapsed);

    if (kindOut) *kindOut = kind;
    return p;
}

ExclusionEngine::ExclusionEngine(const std::vector<ExclusionRule>& rules) {
    (void)SetRules(rules);
}

Result<void> ExclusionEngine::SetRules(const std::vector<ExclusionRule>& rules) {
    std::vector<CompiledExclusion> compiled;
    compiled.reserve(rules.size());

    for (std::size_t i = 0; i < rules.size(); ++i) {
        const auto& r = rules[i];
        CompiledExclusion c;
        c.enabled = r.enabled;
        c.minSizeBytes = r.minSizeBytes;
        c.maxSizeBytes = r.maxSizeBytes;
        c.sourceIndex = i;
        c.kind = r.kind;

        if (c.minSizeBytes != 0 && c.maxSizeBytes != 0 && c.minSizeBytes > c.maxSizeBytes) {
            return Result<void>::Failure(MakeError(
                ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0,
                "exclusion rule " + std::to_string(i) + ": minSizeBytes > maxSizeBytes"));
        }

        std::string norm = NormalisePattern(r.pattern, &c.kind);
        if (c.kind == ExclusionKind::Category) {
            CategoryMatch cm{};
            if (!ParseCategoryName(norm, cm)) {
                return Result<void>::Failure(MakeError(
                    ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0,
                    "exclusion rule " + std::to_string(i) + ": unknown category '" + norm + "'"));
            }
            c.category = cm;
            c.pattern = norm;
        } else if (c.kind == ExclusionKind::SizeRange) {
            // "size:" with no pattern is legal: the bounds carry the meaning.
            c.pattern.clear();
        } else {
            if (norm.empty()) {
                return Result<void>::Failure(MakeError(
                    ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0,
                    "exclusion rule " + std::to_string(i) + ": empty pattern"));
            }
            std::string why;
            if (!RegexSafe(norm, why)) {
                return Result<void>::Failure(MakeError(
                    ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0,
                    "exclusion rule " + std::to_string(i) + ": " + why));
            }
            c.pattern = std::move(norm);
        }

        // Normalise the rule's own kind back onto the caller's struct is not our
        // job; the compiled form is authoritative here.
        compiled.push_back(std::move(c));
    }

    compiled_ = std::move(compiled);
    return Result<void>::Success();
}

std::size_t ExclusionEngine::EnabledCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        compiled_.begin(), compiled_.end(), [](const CompiledExclusion& c) { return c.enabled; }));
}

ExclusionVerdict ExclusionEngine::Evaluate(const FileEntry& file) const {
    ExclusionVerdict v;
    if (file.flags.isExcluded) {
        v.excluded = true;
        v.matchedPattern = "<flag>";
        return v;
    }
    if (compiled_.empty()) return v;

    const std::string canonicalPath = CanonicalPath(file.path);
    const std::string canonicalName = CanonicalPath(file.name);
    const std::string_view ext = ExtensionOf(canonicalName);

    for (const auto& c : compiled_) {
        if (!c.enabled) continue;

        // Size bounds gate every kind: a rule with bounds and a pattern applies
        // both. A rule with bounds only (SizeRange) is a pure size filter.
        if (c.minSizeBytes != 0 && file.sizeBytes < c.minSizeBytes) continue;
        if (c.maxSizeBytes != 0 && file.sizeBytes > c.maxSizeBytes) continue;

        bool hit = false;
        switch (c.kind) {
            case ExclusionKind::SizeRange:
                // Bounds already applied; reaching here means they all passed.
                hit = (c.minSizeBytes != 0 || c.maxSizeBytes != 0);
                break;
            case ExclusionKind::Category:
                hit = (ToCategoryMatch(file.category) == c.category);
                break;
            case ExclusionKind::Extension:
                hit = (!ext.empty() && ext == c.pattern);
                break;
            case ExclusionKind::NameGlob:
                hit = GlobMatch(c.pattern, canonicalName);
                break;
            case ExclusionKind::Path: {
                // Historical semantics, preserved: full path, then bare name,
                // then directory prefix when the pattern names a directory.
                if (GlobMatch(c.pattern, canonicalPath) || GlobMatch(c.pattern, canonicalName)) {
                    hit = true;
                } else if (!c.pattern.empty() && c.pattern.back() == '\\') {
                    hit = canonicalPath.size() >= c.pattern.size() &&
                          canonicalPath.compare(0, c.pattern.size(), c.pattern) == 0;
                } else if (c.pattern.find('\\') != std::string::npos &&
                           canonicalPath.size() > c.pattern.size() &&
                           canonicalPath.compare(0, c.pattern.size(), c.pattern) == 0 &&
                           canonicalPath[c.pattern.size()] == '\\') {
                    // A pattern that names a directory path (no trailing slash)
                    // excludes everything under it.
                    hit = true;
                }
                break;
            }
        }

        if (hit) {
            v.excluded = true;
            v.ruleIndex = c.sourceIndex;
            v.kind = c.kind;
            v.matchedPattern = c.pattern.empty()
                                   ? std::string("<size>")
                                   : (c.kind == ExclusionKind::Category ? CategoryName(c.category)
                                                                        : c.pattern);
            return v;
        }
    }
    return v;
}

Result<std::vector<ExclusionRule>> ExclusionEngine::ImportJson(std::string_view jsonText) const {
    auto parsed = ParseJsonStrict(jsonText);
    if (!parsed) return Result<std::vector<ExclusionRule>>::Failure(parsed.Error());
    if (!parsed.Value().IsArray()) {
        return Result<std::vector<ExclusionRule>>::Failure(MakeError(
            ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Exclusion import must be a JSON array"));
    }

    std::vector<ExclusionRule> out;
    for (const auto& item : parsed.Value().AsArray()) {
        if (!item.IsObject()) {
            return Result<std::vector<ExclusionRule>>::Failure(MakeError(
                ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Exclusion entry must be an object"));
        }
        std::string unk;
        if (!ValidateAllowedKeys(item.AsObject(),
                                 {"pattern", "minSizeBytes", "maxSizeBytes", "enabled", "kind",
                                  "category"},
                                 unk)) {
            return Result<std::vector<ExclusionRule>>::Failure(MakeError(
                ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Unknown exclusion key: " + unk));
        }

        ExclusionRule r;
        if (const auto* pat = item.Find("pattern")) {
            if (!pat->IsString()) {
                return Result<std::vector<ExclusionRule>>::Failure(MakeError(
                    ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "pattern must be a string"));
            }
            r.pattern = pat->AsString();
        }
        if (const auto* cat = item.Find("category")) {
            if (!cat->IsString()) {
                return Result<std::vector<ExclusionRule>>::Failure(MakeError(
                    ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "category must be a string"));
            }
            r.kind = ExclusionKind::Category;
            r.pattern = "category:" + cat->AsString();
        }
        if (const auto* k = item.Find("kind")) {
            if (!k->IsString() || !ParseKindName(k->AsString(), r.kind)) {
                return Result<std::vector<ExclusionRule>>::Failure(MakeError(
                    ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Unknown exclusion kind"));
            }
            // A size rule may be declared purely by its kind and bounds.
            if (r.kind == ExclusionKind::SizeRange && r.pattern.empty()) r.pattern = "size:";
        }
        if (r.pattern.empty() && r.kind != ExclusionKind::SizeRange) {
            return Result<std::vector<ExclusionRule>>::Failure(MakeError(
                ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Exclusion entry needs a pattern"));
        }
        if (const auto* mn = item.Find("minSizeBytes")) {
            if (!mn->IsInt() || mn->AsInt() < 0) {
                return Result<std::vector<ExclusionRule>>::Failure(
                    MakeError(ErrorCode::E_CONFIG_SCHEMA));
            }
            r.minSizeBytes = static_cast<std::uint64_t>(mn->AsInt());
        }
        if (const auto* mx = item.Find("maxSizeBytes")) {
            if (!mx->IsInt() || mx->AsInt() < 0) {
                return Result<std::vector<ExclusionRule>>::Failure(
                    MakeError(ErrorCode::E_CONFIG_SCHEMA));
            }
            r.maxSizeBytes = static_cast<std::uint64_t>(mx->AsInt());
        }
        if (const auto* en = item.Find("enabled")) {
            if (!en->IsBool()) {
                return Result<std::vector<ExclusionRule>>::Failure(
                    MakeError(ErrorCode::E_CONFIG_SCHEMA));
            }
            r.enabled = en->AsBool();
        }
        out.push_back(std::move(r));
    }

    // Validate by compiling; a malformed import must fail here rather than
    // produce a rule set that silently matches nothing.
    ExclusionEngine probe;
    auto rc = probe.SetRules(out);
    if (!rc.Ok()) return Result<std::vector<ExclusionRule>>::Failure(rc.Error());
    return Result<std::vector<ExclusionRule>>::Success(std::move(out));
}

std::string ExclusionEngine::ExportJson(const std::vector<ExclusionRule>& rules) const {
    JsonArray arr;
    for (const auto& r : rules) {
        ExclusionKind kind = r.kind;
        std::string norm = NormalisePattern(r.pattern, &kind);

        JsonObject o;
        if (kind == ExclusionKind::Category) {
            o["category"] = JsonValue{norm};
        } else if (kind == ExclusionKind::Extension) {
            o["pattern"] = JsonValue{norm};
            o["kind"] = JsonValue{std::string(KindName(kind))};
        } else {
            if (!norm.empty()) o["pattern"] = JsonValue{norm};
            if (kind != ExclusionKind::Path) o["kind"] = JsonValue{std::string(KindName(kind))};
        }
        o["minSizeBytes"] = JsonValue{static_cast<std::int64_t>(r.minSizeBytes)};
        o["maxSizeBytes"] = JsonValue{static_cast<std::int64_t>(r.maxSizeBytes)};
        o["enabled"] = JsonValue{r.enabled};
        arr.push_back(JsonValue{std::move(o)});
    }
    return DumpJson(JsonValue{std::move(arr)}, 2) + "\n";
}

} // namespace auralite
