#include "protocol/resp_writer.h"

namespace tachyon {

namespace {

// Builds "<prefix><text>\r\n" with any \r or \n inside `text` replaced by spaces.
std::string single_line(char prefix, std::string_view text) {
    std::string line(1, prefix);
    line.append(text);
    for (std::size_t i = 1; i < line.size(); ++i) {
        if (line[i] == '\r' || line[i] == '\n') {
            line[i] = ' ';
        }
    }
    line.append("\r\n");
    return line;
}

}  // namespace

std::string simple_string_reply(std::string_view text) { return single_line('+', text); }

std::string error_reply(std::string_view message) { return single_line('-', message); }

std::string integer_reply(long long value) { return ":" + std::to_string(value) + "\r\n"; }

std::string bulk_reply(std::string_view value) {
    std::string reply = "$" + std::to_string(value.size()) + "\r\n";
    reply.append(value);
    reply.append("\r\n");
    return reply;
}

std::string null_reply() { return "$-1\r\n"; }

}  // namespace tachyon
