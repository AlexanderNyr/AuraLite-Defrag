#pragma once

#include "core/result.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace auralite {

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;

struct JsonValue {
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, JsonArray, JsonObject>;
    Storage data{nullptr};

    bool IsNull() const noexcept { return std::holds_alternative<std::nullptr_t>(data); }
    bool IsBool() const noexcept { return std::holds_alternative<bool>(data); }
    bool IsInt() const noexcept { return std::holds_alternative<std::int64_t>(data); }
    bool IsDouble() const noexcept { return std::holds_alternative<double>(data) || std::holds_alternative<std::int64_t>(data); }
    bool IsString() const noexcept { return std::holds_alternative<std::string>(data); }
    bool IsArray() const noexcept { return std::holds_alternative<JsonArray>(data); }
    bool IsObject() const noexcept { return std::holds_alternative<JsonObject>(data); }

    bool AsBool(bool def = false) const noexcept {
        return IsBool() ? std::get<bool>(data) : def;
    }
    std::int64_t AsInt(std::int64_t def = 0) const noexcept {
        return IsInt() ? std::get<std::int64_t>(data) : def;
    }
    double AsDouble(double def = 0.0) const noexcept {
        if (std::holds_alternative<double>(data)) return std::get<double>(data);
        if (std::holds_alternative<std::int64_t>(data)) return static_cast<double>(std::get<std::int64_t>(data));
        return def;
    }
    const std::string& AsString() const {
        static const std::string kEmpty;
        return IsString() ? std::get<std::string>(data) : kEmpty;
    }
    const JsonArray& AsArray() const {
        static const JsonArray kEmpty;
        return IsArray() ? std::get<JsonArray>(data) : kEmpty;
    }
    const JsonObject& AsObject() const {
        static const JsonObject kEmpty;
        return IsObject() ? std::get<JsonObject>(data) : kEmpty;
    }

    const JsonValue* Find(std::string_view key) const noexcept {
        if (!IsObject()) return nullptr;
        const auto& obj = std::get<JsonObject>(data);
        auto it = obj.find(key);
        return it != obj.end() ? &it->second : nullptr;
    }
};

struct JsonLimits {
    std::size_t maxBytes{1024U * 1024U}; // 1 MiB per §1.3 / P2-T04
    std::size_t maxDepth{8U};            // ≤8 nesting levels per §1.3 / P2-T04
};

Result<JsonValue> ParseJsonStrict(std::string_view input, JsonLimits limits = {});
std::string DumpJson(const JsonValue& value, int indent = 2);
bool ValidateAllowedKeys(const JsonObject& obj, const std::vector<std::string_view>& allowedKeys, std::string& unknownKey);
bool IsValidUtf8(std::string_view s) noexcept;

} // namespace auralite
