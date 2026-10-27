// Commands that act on the connection rather than the store: SUBSCRIBE, UNSUBSCRIBE, PUBLISH
// and QUIT, plus the rules for a connection in subscribed mode. They live here, not in
// commands/, because they need the connection and the channel registry.
#pragma once

#include <string>
#include <vector>

#include "server/connection.h"
#include "server/pubsub.h"

namespace tachyon {

// Appends the reply to connection.pending_reply and returns true if this was a connection
// command, or if the connection is subscribed (where every other command is refused, as in
// Redis). Returns false for a normal command, which the caller then runs against the store.
bool run_client_command(Connection& connection, PubSub& pubsub,
                        const std::vector<std::string>& args);

}  // namespace tachyon
