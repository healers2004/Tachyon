// Builds replies in RESP2, the format redis-cli expects back.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon {

// "+PONG\r\n": a short status. \r and \n in `text` become spaces so they can't break the framing.
std::string simple_string_reply(std::string_view text);

// "-ERR message\r\n": an error. Same \r\n rule, since messages can quote user input.
std::string error_reply(std::string_view message);

// ":3\r\n": an integer, e.g. how many keys DEL removed.
std::string integer_reply(long long value);

// "$3\r\nbar\r\n": a length-prefixed string; may contain any bytes.
std::string bulk_reply(std::string_view value);

// "$-1\r\n": nil, e.g. GET on a missing key.
std::string null_reply();

// "*-1\r\n": a nil array, e.g. LPOP key 2 on a missing key.
std::string null_array_reply();

// "*2\r\n": the start of an array of `count` elements; the elements follow as separate replies.
std::string array_header(std::size_t count);

// "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n": a command in the form clients send it. The write-ahead
// log stores commands this way, so replay reuses the normal RESP parser.
std::string encode_command(const std::vector<std::string>& args);

}  // namespace tachyon
