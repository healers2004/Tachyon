// The TCP server: accepts clients, feeds their bytes to the parser and sends back replies.
#pragma once

#include "server/context.h"

namespace tachyon {

// Asks run_server to stop. Safe to call from a signal handler: it only sets a flag.
void request_stop();

// Loads the snapshot and log (unless options.persist is false), then serves any number of
// clients at once, on one thread, until request_stop() is called.
// Returns the process exit code: 0 after a clean stop, 1 if the server failed.
int run_server(const ServerOptions& options);

}  // namespace tachyon
