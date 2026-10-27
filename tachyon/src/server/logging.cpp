#include "server/logging.h"

#include <iostream>

namespace tachyon {

namespace {

LogLevel g_level = LogLevel::kNotice;  // only main() changes it, before the server starts

}  // namespace

void set_log_level(LogLevel level) { g_level = level; }

bool parse_log_level(std::string_view text, LogLevel& level) {
    if (text == "debug") {
        level = LogLevel::kDebug;
    } else if (text == "verbose") {
        level = LogLevel::kVerbose;
    } else if (text == "notice") {
        level = LogLevel::kNotice;
    } else if (text == "warning") {
        level = LogLevel::kWarning;
    } else {
        return false;
    }
    return true;
}

void log_line(LogLevel level, const std::string& message) {
    // enum class values compare in the order they're declared: kDebug < ... < kWarning.
    if (level < g_level) return;
    std::ostream& out = level == LogLevel::kWarning ? std::cerr : std::cout;
    out << "tachyon: " << message << std::endl;  // endl flushes, so lines appear at once
}

}  // namespace tachyon
