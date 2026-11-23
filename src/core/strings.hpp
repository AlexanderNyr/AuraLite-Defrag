#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace auralite {

class StringTable {
public:
    static std::string_view Lookup(std::string_view key) noexcept;
    static std::string Format(std::string_view key, const std::vector<std::string>& args);
    static std::vector<std::string_view> AllKeys();
};

// Locale-aware formatting helpers (R-047)
std::string FormatInteger(std::uint64_t value, char thousandsSep = ',');
std::string FormatByteSize(std::uint64_t bytes, char decimalSep = '.');
std::string FormatPercent(double pct, char decimalSep = '.');

} // namespace auralite
