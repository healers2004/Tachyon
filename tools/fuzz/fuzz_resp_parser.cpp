// libFuzzer target for the RESP parser. libFuzzer calls LLVMFuzzerTestOneInput with millions
// of generated inputs, keeping the ones that reach new code, while AddressSanitizer and
// UndefinedBehaviorSanitizer watch for memory and arithmetic errors.
//
// Besides "doesn't crash", it checks the parser's main promise: feeding the same bytes in one
// piece or in many small pieces gives exactly the same commands, then the same final status.
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "protocol/resp_parser.h"

namespace {

struct Outcome {
    std::vector<std::vector<std::string>> commands;
    tachyon::ParseStatus last = tachyon::ParseStatus::kNeedMore;  // kNeedMore or kError
};

// Feeds `input` in chunks of `chunk` bytes (0: all at once), taking every command it can
// after each one, and stopping at the first error, as the server does.
Outcome parse(std::string_view input, std::size_t chunk) {
    Outcome outcome;
    tachyon::RespParser parser;
    std::size_t offset = 0;
    while (offset < input.size() || offset == 0) {
        std::size_t size = chunk == 0 ? input.size() : chunk;
        if (size > input.size() - offset) size = input.size() - offset;
        parser.feed(input.substr(offset, size));
        offset += size;
        while (true) {
            tachyon::ParseResult result = parser.next();
            if (result.status == tachyon::ParseStatus::kCommand) {
                // Limits the parser promises to enforce.
                if (result.args.empty() || result.args.size() > tachyon::kMaxArgs) {
                    __builtin_trap();
                }
                outcome.commands.push_back(result.args);
                continue;
            }
            outcome.last = result.status;
            break;
        }
        if (outcome.last == tachyon::ParseStatus::kError || input.empty()) break;
    }
    return outcome;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);
    const Outcome whole = parse(input, 0);
    // The chunk size comes from the input itself, so libFuzzer explores it too.
    const std::size_t chunk = size == 0 ? 1 : (data[0] % 13) + 1;
    const Outcome pieces = parse(input, chunk);
    if (whole.commands != pieces.commands || whole.last != pieces.last) {
        __builtin_trap();  // the same bytes parsed differently depending on how they arrived
    }
    return 0;
}
