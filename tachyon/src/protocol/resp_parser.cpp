#include "protocol/resp_parser.h"

#include <charconv>
#include <system_error>

namespace tachyon {

namespace {

// A "*<count>\r\n" or "$<length>\r\n" line is never longer than this.
const std::size_t kMaxLengthLine = 32;

enum class LineStatus { kNeedMore, kOk, kError };

// Reads a "<type><number>\r\n" line starting at buf[pos]. On kOk, stores the number in `value`
// and moves `pos` past the line. Both are references, so the caller's variables change.
LineStatus read_length_line(std::string_view buf, std::size_t& pos, char type, long long& value) {
    if (pos >= buf.size()) {
        return LineStatus::kNeedMore;
    }
    // Checked before waiting for "\r\n", so e.g. an HTTP request fails on its first byte.
    if (buf[pos] != type) {
        return LineStatus::kError;
    }
    std::size_t end = buf.find("\r\n", pos);
    if (end == std::string_view::npos) {
        // Incomplete, unless it's already too long to ever become a valid line.
        return buf.size() - pos > kMaxLengthLine ? LineStatus::kError : LineStatus::kNeedMore;
    }

    // The digits between the type character and "\r\n". substr on a string_view copies nothing.
    std::string_view digits = buf.substr(pos + 1, end - pos - 1);
    const char* first = digits.data();
    const char* last = first + digits.size();
    // std::from_chars parses a number without exceptions. ec is set on non-digits or overflow;
    // ptr != last means junk after the number.
    std::from_chars_result parsed = std::from_chars(first, last, value);
    if (digits.empty() || parsed.ec != std::errc() || parsed.ptr != last) {
        return LineStatus::kError;
    }
    pos = end + 2;
    return LineStatus::kOk;
}

ParseResult make_error(const std::string& message) {
    ParseResult result;
    result.status = ParseStatus::kError;
    result.error = "Protocol error: " + message;
    return result;
}

}  // namespace

void RespParser::feed(std::string_view data) { buffer_.append(data); }

ParseResult RespParser::next() {
    // Parse from the start of the buffer through a view, so nothing is consumed until a whole
    // command is present.
    // shortcut: re-scans from the start after every feed (fine up to kMaxCommandBytes); a state
    // machine that resumes where it stopped is faster for big commands.
    std::string_view buf = buffer_;
    std::size_t pos = 0;

    long long count = 0;
    LineStatus line = read_length_line(buf, pos, '*', count);
    if (line == LineStatus::kNeedMore) return need_more();
    if (line == LineStatus::kError) return make_error("expected '*<count>'");
    // static_cast: an explicit, checked-at-compile-time conversion, like a C cast but narrower.
    if (count < 1 || count > static_cast<long long>(kMaxArgs)) {
        return make_error("invalid argument count");
    }

    ParseResult result;
    result.status = ParseStatus::kCommand;
    for (long long i = 0; i < count; ++i) {
        long long length = 0;
        line = read_length_line(buf, pos, '$', length);
        if (line == LineStatus::kNeedMore) return need_more();
        if (line == LineStatus::kError) return make_error("expected '$<length>'");
        if (length < 0 || length > static_cast<long long>(kMaxCommandBytes)) {
            return make_error("invalid bulk length");
        }

        // The string is length-prefixed, so it may itself contain "\r\n".
        std::size_t size = static_cast<std::size_t>(length);
        if (buf.size() - pos < size + 2) return need_more();
        if (buf.substr(pos + size, 2) != "\r\n") {
            return make_error("bulk string not followed by CRLF");
        }
        result.args.push_back(std::string(buf.substr(pos, size)));
        pos += size + 2;
    }

    buffer_.erase(0, pos);
    return result;
}

ParseResult RespParser::need_more() const {
    // A single unfinished command bigger than the limit will never be accepted.
    if (buffer_.size() > kMaxCommandBytes) {
        return make_error("command too large");
    }
    return ParseResult{};  // status defaults to kNeedMore
}

}  // namespace tachyon
