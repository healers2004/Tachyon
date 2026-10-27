// Shared by the command tests: run a command at a fixed, made-up time.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "commands/commands.h"
#include "commands/store.h"

namespace test {

// Any fixed Unix time works; a fixed one makes expiry tests exact.
const std::int64_t kNow = 1'700'000'000'000;

// Runs one command against `store` at `now_ms`; the braces at call sites build the vector.
inline std::string run(tachyon::Store& store, const std::vector<std::string>& args,
                       std::int64_t now_ms = kNow) {
    return tachyon::execute_command(store, args, now_ms);
}

// A copy of `args` with relative expiry times made absolute, as the server does before running
// and logging a command.
inline std::vector<std::string> absolute(std::vector<std::string> args, std::int64_t now_ms) {
    tachyon::make_times_absolute(args, now_ms);
    return args;
}

}  // namespace test
