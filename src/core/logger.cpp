#include "core/logger.hpp"
#include "core/error_catalog.hpp"
#include "core/json.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace auralite {

std::string HashPathForPrivacy(std::string_view path) {
    // 64-bit FNV-1a hashed path identifier (never logs raw user path by default)
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : path) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[24]{};
    std::snprintf(buf, sizeof(buf), "pid:%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

std::string SanitizeLogField(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        if (c == '\r' || c == '\n' || c == '\t') {
            out.push_back(' ');
        } else if (c >= 0x20 && c != 0x7F) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string StructuredLogger::CurrentUtcIso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tmUtc{};
#ifdef _WIN32
    gmtime_s(&tmUtc, &tt);
#else
    gmtime_r(&tt, &tmUtc);
#endif
    char buf[80]{};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
                  tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec,
                  static_cast<int>(ms.count()));
    return std::string(buf);
}

namespace {
const char* LevelName(LogLevel lvl) noexcept {
    switch (lvl) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

const char* StageName(ErrorStage st) noexcept {
    switch (st) {
        case ErrorStage::None: return "NONE";
        case ErrorStage::Preflight: return "PREFLIGHT";
        case ErrorStage::Enumeration: return "ENUMERATION";
        case ErrorStage::Bitmap: return "BITMAP";
        case ErrorStage::Planning: return "PLANNING";
        case ErrorStage::Move: return "MOVE";
        case ErrorStage::Verification: return "VERIFICATION";
        case ErrorStage::Journal: return "JOURNAL";
        case ErrorStage::Config: return "CONFIG";
        case ErrorStage::Export: return "EXPORT";
        case ErrorStage::Internal: return "INTERNAL";
    }
    return "NONE";
}

std::string EscapeJsonInline(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}
} // namespace

StructuredLogger::StructuredLogger(std::filesystem::path logFilePath,
                                   std::size_t maxFileBytes,
                                   std::size_t maxRotations)
    : logFilePath_(std::move(logFilePath)),
      maxFileBytes_(maxFileBytes),
      maxRotations_(maxRotations) {}

void StructuredLogger::SetLogFile(std::filesystem::path logFilePath,
                                  std::size_t maxFileBytes,
                                  std::size_t maxRotations) {
    std::lock_guard lock(mutex_);
    logFilePath_ = std::move(logFilePath);
    maxFileBytes_ = maxFileBytes;
    maxRotations_ = maxRotations;
}

void StructuredLogger::SetMinLevel(LogLevel level) noexcept {
    std::lock_guard lock(mutex_);
    minLevel_ = level;
}

void StructuredLogger::SetVerbosePaths(bool verbose) noexcept {
    std::lock_guard lock(mutex_);
    verbosePaths_ = verbose;
}

bool StructuredLogger::VerbosePaths() const noexcept {
    std::lock_guard lock(mutex_);
    return verbosePaths_;
}

std::string StructuredLogger::FormatEventJsonLine(const LogEvent& ev) {
    std::ostringstream os;
    os << "{"
       << "\"ts_utc\":" << EscapeJsonInline(ev.tsUtc.empty() ? CurrentUtcIso8601() : ev.tsUtc)
       << ",\"level\":" << EscapeJsonInline(LevelName(ev.level))
       << ",\"opId\":" << EscapeJsonInline(SanitizeLogField(ev.opId))
       << ",\"volumeId\":" << EscapeJsonInline(SanitizeLogField(ev.volumeId))
       << ",\"errorCode\":" << EscapeJsonInline(ErrorCodeName(ev.errorCode))
       << ",\"stage\":" << EscapeJsonInline(StageName(ev.stage))
       << ",\"duration_ms\":" << ev.durationMs
       << ",\"clusters\":" << ev.clusters
       << ",\"messageKey\":" << EscapeJsonInline(SanitizeLogField(ev.messageKey))
       << ",\"arguments\":[";
    for (std::size_t i = 0; i < ev.arguments.size(); ++i) {
        if (i > 0) os << ',';
        os << EscapeJsonInline(SanitizeLogField(ev.arguments[i]));
    }
    os << "]}";
    return os.str();
}

void StructuredLogger::RotateIfNeededLocked(std::size_t incomingBytes) {
    if (logFilePath_.empty() || maxFileBytes_ == 0 || maxRotations_ == 0) return;
    std::error_code ec;
    if (!std::filesystem::exists(logFilePath_, ec)) return;
    const auto sz = std::filesystem::file_size(logFilePath_, ec);
    if (ec || sz + incomingBytes <= maxFileBytes_) return;

    for (std::size_t i = maxRotations_; i >= 1; --i) {
        auto dst = logFilePath_;
        dst += "." + std::to_string(i);
        if (i == maxRotations_) {
            std::filesystem::remove(dst, ec);
        }
        auto src = (i == 1) ? logFilePath_ : [&]() {
            auto p = logFilePath_;
            p += "." + std::to_string(i - 1);
            return p;
        }();
        if (std::filesystem::exists(src, ec)) {
            std::filesystem::rename(src, dst, ec);
        }
    }
}

void StructuredLogger::Log(LogEvent event) {
    if (event.tsUtc.empty()) {
        event.tsUtc = CurrentUtcIso8601();
    }
    std::lock_guard lock(mutex_);
    if (static_cast<int>(event.level) < static_cast<int>(minLevel_)) {
        return;
    }
    if (memoryRing_.size() >= 2000U) {
        memoryRing_.erase(memoryRing_.begin());
    }
    memoryRing_.push_back(event);

    if (!logFilePath_.empty()) {
        const std::string line = FormatEventJsonLine(event) + "\n";
        std::error_code ec;
        if (logFilePath_.has_parent_path()) {
            std::filesystem::create_directories(logFilePath_.parent_path(), ec);
        }
        RotateIfNeededLocked(line.size());
        std::ofstream ofs(logFilePath_, std::ios::binary | std::ios::app);
        if (ofs) {
            ofs.write(line.data(), static_cast<std::streamsize>(line.size()));
            ofs.flush();
        }
    }
}

std::vector<LogEvent> StructuredLogger::RecentEvents(std::size_t maxCount) const {
    std::lock_guard lock(mutex_);
    if (memoryRing_.size() <= maxCount) return memoryRing_;
    return std::vector<LogEvent>(memoryRing_.end() - static_cast<std::ptrdiff_t>(maxCount), memoryRing_.end());
}

} // namespace auralite
