// Incremental parser for RESP2, the Redis wire protocol. Clients send each command as an
// array of bulk strings, e.g. SET foo bar arrives as:
//   *3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
// TCP may deliver that in any number of pieces, so bytes are buffered until a full command is in.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon {

// Limits that stop one client from making tachyon use unbounded memory.
const std::size_t kMaxArgs = 1024;
const std::size_t kMaxCommandBytes = 1024 * 1024;

// enum class: values are written ParseStatus::kCommand and don't convert to int by accident.
enum class ParseStatus {
    kNeedMore,  // only part of a command has arrived; feed more bytes and call next() again
    kCommand,   // a full command is in ParseResult::args
    kError,     // malformed or oversized input; the connection should be closed
};

// Errors are returned as values, not thrown as exceptions.
struct ParseResult {
    ParseStatus status = ParseStatus::kNeedMore;
    std::vector<std::string> args;  // set when status is kCommand, e.g. {"SET", "foo", "bar"}
    std::string error;              // set when status is kError
};

// A class bundles data (the private buffer) with the only functions allowed to touch it.
class RespParser {
public:
    // Appends bytes received from the socket. string_view: a pointer + length, no copy.
    void feed(std::string_view data);

    // Returns the next complete command, kNeedMore, or kError. Only a complete command
    // removes bytes from the buffer.
    ParseResult next();

private:
    // `const` after the parameters: this function only reads the object's data.
    ParseResult need_more() const;

    std::string buffer_;  // received bytes not yet returned as a command
};

}  // namespace tachyon
