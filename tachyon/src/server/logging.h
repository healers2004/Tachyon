// The server's log lines, filtered by level like Redis's --loglevel.
#pragma once

#include <string>
#include <string_view>

namespace tachyon {

enum class LogLevel {
    kDebug,    // everything, for tracking down a problem
    kVerbose,  // routine events, e.g. a client sent bad input and was disconnected
    kNotice,   // the default: startup, snapshots, shutdown
    kWarning,  // something went wrong
};

// Lines below `level` are dropped. Set once at startup.
void set_log_level(LogLevel level);

// Parses "debug", "verbose", "notice" or "warning". false for anything else.
bool parse_log_level(std::string_view text, LogLevel& level);

// Prints "tachyon: <message>": to stderr for warnings, stdout otherwise.
// shortcut: no timestamps; a production log would add them, and a file option.
void log_line(LogLevel level, const std::string& message);

}  // namespace tachyon
