// Turns a parsed command into a store operation and a RESP reply.
#pragma once

#include <string>
#include <vector>

#include "commands/store.h"

namespace tachyon {

// Runs one command, e.g. {"SET", "foo", "bar"}, and returns the reply bytes to send back.
// Unknown commands and bad arguments return a RESP error reply; this never fails otherwise.
// `store` is a non-const reference: SET and DEL change it.
std::string execute_command(Store& store, const std::vector<std::string>& args);

// True for commands that can change the store (SET, DEL). Only these go to the log.
bool is_write_command(const std::vector<std::string>& args);

}  // namespace tachyon
