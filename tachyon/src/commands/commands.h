// Turns a parsed command into a store operation and a RESP reply.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "commands/store.h"

namespace tachyon {

// Runs one command, e.g. {"SET", "foo", "bar"}, at time `now_ms` (Unix ms, used for expiry) and
// returns the reply bytes to send back. Unknown commands and bad arguments return a RESP error
// reply; this never fails otherwise. `store` is a non-const reference: writes change it.
std::string execute_command(Store& store, const std::vector<std::string>& args,
                            std::int64_t now_ms);

// "" if the command exists and has a valid number of arguments; otherwise the error reply that
// execute_command would give (unknown command, or wrong number of arguments).
std::string validate_command(const std::vector<std::string>& args);

// True for commands that can change the store. Only these go to the log.
bool is_write_command(const std::vector<std::string>& args);

// Rewrites relative expiry times into absolute ones, so the command means the same when the
// log is replayed later: EXPIRE k 10 becomes PEXPIREAT k <now + 10 s>, and SET's EX/PX/EXAT
// become PXAT. Run (and log) the result instead of the original; the reply is the same.
// Anything that would be an error is left unchanged, so it fails the same way.
std::vector<std::string> with_absolute_times(const std::vector<std::string>& args,
                                             std::int64_t now_ms);

}  // namespace tachyon
