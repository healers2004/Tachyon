// Commands about the server itself: SAVE and INFO. They need the whole ServerContext, so they
// run here rather than in commands/.
#pragma once

#include <string>
#include <vector>

#include "server/context.h"

namespace tachyon {

// Appends the reply to `reply` and returns true for SAVE or INFO; false for anything else.
bool run_server_command(ServerContext& context, const std::vector<std::string>& args,
                        std::string& reply);

// Writes a snapshot and starts a new log, updating the stats. Returns "" on success or the
// reason it failed. A failure leaves the old snapshot and log in charge (the server carries
// on), unless it was fatal, which sets context.fatal.
std::string save_snapshot(ServerContext& context);

// The text of INFO for the given sections ("" or "all": every section).
std::string info_text(const ServerContext& context, const std::vector<std::string>& sections);

}  // namespace tachyon
