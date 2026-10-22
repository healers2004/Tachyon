// Builds replies in RESP2, the format redis-cli expects back.
#pragma once

#include <string>
#include <string_view>

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

}  // namespace tachyon
