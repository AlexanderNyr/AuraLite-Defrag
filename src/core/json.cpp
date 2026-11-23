#include "core/json.hpp"
#include "core/error_catalog.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace auralite {

bool IsValidUtf8(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c <= 0x7F) {
            ++i;
        } else if ((c >> 5) == 0x6) {
            if (i + 1 >= s.size()) return false;
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c1 & 0xC0) != 0x80 || c < 0xC2) return false;
            i += 2;
        } else if ((c >> 4) == 0xE) {
            if (i + 2 >= s.size()) return false;
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;
            if (c == 0xE0 && c1 < 0xA0) return false;
            if (c == 0xED && c1 >= 0xA0) return false;
            i += 3;
        } else if ((c >> 3) == 0x1E) {
            if (i + 3 >= s.size()) return false;
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            const unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return false;
            if (c == 0xF0 && c1 < 0x90) return false;
            if (c > 0xF4 || (c == 0xF4 && c1 > 0x8F)) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

namespace {

class Parser {
public:
    Parser(std::string_view src, JsonLimits limits) : src_(src), limits_(limits) {}

    Result<JsonValue> Parse() {
        SkipWs();
        auto r = ParseValue(0);
        if (!r) return r;
        SkipWs();
        if (pos_ != src_.size()) {
            return Result<JsonValue>::Failure(
                MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Trailing characters after JSON root"));
        }
        return r;
    }

private:
    std::string_view src_;
    JsonLimits limits_;
    std::size_t pos_{0};

    void SkipWs() noexcept {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    Result<JsonValue> ParseValue(std::size_t depth) {
        SkipWs();
        if (pos_ >= src_.size()) {
            return Result<JsonValue>::Failure(
                MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Unexpected end of JSON"));
        }
        char c = src_[pos_];
        if (c == '{') return ParseObject(depth + 1);
        if (c == '[') return ParseArray(depth + 1);
        if (c == '"') {
            auto s = ParseString();
            if (!s) return Result<JsonValue>::Failure(s.Error());
            return Result<JsonValue>::Success(JsonValue{std::move(s.Value())});
        }
        if (c == 't' && src_.substr(pos_, 4) == "true") {
            pos_ += 4;
            return Result<JsonValue>::Success(JsonValue{true});
        }
        if (c == 'f' && src_.substr(pos_, 5) == "false") {
            pos_ += 5;
            return Result<JsonValue>::Success(JsonValue{false});
        }
        if (c == 'n' && src_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return Result<JsonValue>::Success(JsonValue{nullptr});
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return ParseNumber();
        }
        return Result<JsonValue>::Failure(
            MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Unexpected token in JSON"));
    }

    Result<JsonValue> ParseObject(std::size_t depth) {
        if (depth > limits_.maxDepth) {
            return Result<JsonValue>::Failure(
                MakeError(ErrorCode::E_CONFIG_DEPTH_LIMIT, {}, 0, 0, "JSON nesting depth exceeds limit"));
        }
        ++pos_; // '{'
        JsonObject obj;
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return Result<JsonValue>::Success(JsonValue{std::move(obj)});
        }
        while (true) {
            SkipWs();
            if (pos_ >= src_.size() || src_[pos_] != '"') {
                return Result<JsonValue>::Failure(
                    MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Expected object key string"));
            }
            auto key = ParseString();
            if (!key) return Result<JsonValue>::Failure(key.Error());
            if (obj.find(key.Value()) != obj.end()) {
                return Result<JsonValue>::Failure(
                    MakeError(ErrorCode::E_CONFIG_SCHEMA, {}, 0, 0, "Duplicate JSON key rejected"));
            }
            SkipWs();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                return Result<JsonValue>::Failure(
                    MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Expected ':' after key"));
            }
            ++pos_;
            auto val = ParseValue(depth);
            if (!val) return val;
            obj.emplace(std::move(key.Value()), std::move(val.Value()));
            SkipWs();
            if (pos_ >= src_.size()) {
                return Result<JsonValue>::Failure(
                    MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Unterminated JSON object"));
            }
            if (src_[pos_] == '}') {
                ++pos_;
                return Result<JsonValue>::Success(JsonValue{std::move(obj)});
            }
            if (src_[pos_] != ',') {
                return Result<JsonValue>::Failure(
                    MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Expected ',' or '}' in object"));
            }
            ++pos_;
        }
    }

    Result<JsonValue> ParseArray(std::size_t depth) {
        if (depth > limits_.maxDepth) {
            return Result<JsonValue>::Failure(
                MakeError(ErrorCode::E_CONFIG_DEPTH_LIMIT, {}, 0, 0, "JSON nesting depth exceeds limit"));
        }
        ++pos_; // '['
        JsonArray arr;
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return Result<JsonValue>::Success(JsonValue{std::move(arr)});
        }
        while (true) {
            auto val = ParseValue(depth);
            if (!val) return val;
            arr.push_back(std::move(val.Value()));
            SkipWs();
            if (pos_ >= src_.size()) {
                return Result<JsonValue>::Failure(
                    MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Unterminated JSON array"));
            }
            if (src_[pos_] == ']') {
                ++pos_;
                return Result<JsonValue>::Success(JsonValue{std::move(arr)});
            }
            if (src_[pos_] != ',') {
                return Result<JsonValue>::Failure(
                    MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Expected ',' or ']' in array"));
            }
            ++pos_;
        }
    }

    static void AppendUtf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ReadHex4(std::uint32_t& out) noexcept {
        if (pos_ + 4 > src_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = src_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    Result<std::string> ParseString() {
        ++pos_; // '"'
        std::string out;
        while (pos_ < src_.size()) {
            unsigned char uc = static_cast<unsigned char>(src_[pos_++]);
            if (uc == '"') {
                return Result<std::string>::Success(std::move(out));
            }
            if (uc < 0x20) {
                return Result<std::string>::Failure(
                    MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Unescaped control char in JSON string"));
            }
            if (uc != '\\') {
                out.push_back(static_cast<char>(uc));
                continue;
            }
            if (pos_ >= src_.size()) {
                return Result<std::string>::Failure(
                    MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Unterminated escape in JSON string"));
            }
            char esc = src_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!ReadHex4(cp)) {
                        return Result<std::string>::Failure(
                            MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Invalid \\uXXXX escape"));
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 2 > src_.size() || src_[pos_] != '\\' || src_[pos_ + 1] != 'u') {
                            return Result<std::string>::Failure(
                                MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Lone high surrogate"));
                        }
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (!ReadHex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            return Result<std::string>::Failure(
                                MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Invalid low surrogate"));
                        }
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return Result<std::string>::Failure(
                            MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Lone low surrogate"));
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    return Result<std::string>::Failure(
                        MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Invalid JSON escape character"));
            }
        }
        return Result<std::string>::Failure(
            MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Unterminated JSON string"));
    }

    Result<JsonValue> ParseNumber() {
        const std::size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        if (pos_ >= src_.size()) {
            return Result<JsonValue>::Failure(MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Invalid number"));
        }
        if (src_[pos_] == '0') {
            ++pos_;
            if (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                return Result<JsonValue>::Failure(MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Leading zero in number"));
            }
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        } else {
            return Result<JsonValue>::Failure(MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Invalid number"));
        }

        bool isFloat = false;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            isFloat = true;
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                return Result<JsonValue>::Failure(MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Invalid fraction"));
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            isFloat = true;
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                return Result<JsonValue>::Failure(MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Invalid exponent"));
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }

        std::string token(src_.substr(start, pos_ - start));
        if (!isFloat) {
            char* endPtr = nullptr;
            errno = 0;
            long long v = std::strtoll(token.c_str(), &endPtr, 10);
            if (errno == 0 && endPtr == token.c_str() + token.size()) {
                return Result<JsonValue>::Success(JsonValue{static_cast<std::int64_t>(v)});
            }
        }
        char* endPtr = nullptr;
        errno = 0;
        double d = std::strtod(token.c_str(), &endPtr);
        if (errno != 0 || endPtr != token.c_str() + token.size() || !std::isfinite(d)) {
            return Result<JsonValue>::Failure(MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "Number out of range"));
        }
        return Result<JsonValue>::Success(JsonValue{d});
    }
};

void DumpString(std::ostringstream& os, const std::string& s) {
    os << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    os << buf;
                } else {
                    os << static_cast<char>(c);
                }
        }
    }
    os << '"';
}

void DumpImpl(std::ostringstream& os, const JsonValue& v, int indent, int level) {
    if (v.IsNull()) {
        os << "null";
    } else if (v.IsBool()) {
        os << (v.AsBool() ? "true" : "false");
    } else if (v.IsInt()) {
        os << v.AsInt();
    } else if (std::holds_alternative<double>(v.data)) {
        os << std::get<double>(v.data);
    } else if (v.IsString()) {
        DumpString(os, v.AsString());
    } else if (v.IsArray()) {
        const auto& arr = v.AsArray();
        if (arr.empty()) {
            os << "[]";
            return;
        }
        os << "[\n";
        for (std::size_t i = 0; i < arr.size(); ++i) {
            os << std::string(static_cast<std::size_t>((level + 1) * indent), ' ');
            DumpImpl(os, arr[i], indent, level + 1);
            if (i + 1 < arr.size()) os << ',';
            os << '\n';
        }
        os << std::string(static_cast<std::size_t>(level * indent), ' ') << ']';
    } else if (v.IsObject()) {
        const auto& obj = v.AsObject();
        if (obj.empty()) {
            os << "{}";
            return;
        }
        os << "{\n";
        std::size_t idx = 0;
        for (const auto& [k, val] : obj) {
            os << std::string(static_cast<std::size_t>((level + 1) * indent), ' ');
            DumpString(os, k);
            os << ": ";
            DumpImpl(os, val, indent, level + 1);
            if (++idx < obj.size()) os << ',';
            os << '\n';
        }
        os << std::string(static_cast<std::size_t>(level * indent), ' ') << '}';
    }
}

} // namespace

Result<JsonValue> ParseJsonStrict(std::string_view input, JsonLimits limits) {
    if (input.size() > limits.maxBytes) {
        return Result<JsonValue>::Failure(
            MakeError(ErrorCode::E_CONFIG_SIZE_LIMIT, {}, 0, 0, "JSON input exceeds maxBytes"));
    }
    if (!IsValidUtf8(input)) {
        return Result<JsonValue>::Failure(
            MakeError(ErrorCode::E_CONFIG_PARSE, {}, 0, 0, "JSON input is not valid UTF-8"));
    }
    Parser p(input, limits);
    return p.Parse();
}

std::string DumpJson(const JsonValue& value, int indent) {
    std::ostringstream os;
    DumpImpl(os, value, indent, 0);
    return os.str();
}

bool ValidateAllowedKeys(const JsonObject& obj, const std::vector<std::string_view>& allowedKeys, std::string& unknownKey) {
    for (const auto& [k, _] : obj) {
        bool found = false;
        for (auto ak : allowedKeys) {
            if (k == ak) {
                found = true;
                break;
            }
        }
        if (!found) {
            unknownKey = k;
            return false;
        }
    }
    return true;
}

} // namespace auralite
