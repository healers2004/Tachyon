// The TCP server: accepts clients, feeds their bytes to the parser and sends back replies.
#pragma once

#include <string>

namespace tachyon {

// Never Redis's 6379, so tachyon can't clash with a real Redis on the same machine.
const unsigned short kDefaultPort = 6380;

// Asks run_server to stop. Safe to call from a signal handler: it only sets a flag.
void request_stop();

// Serves clients one at a time on ip:port until request_stop() is called.
// Returns the process exit code: 0 after a clean stop, 1 if the server failed.
int run_server(const std::string& ip, unsigned short port);

}  // namespace tachyon
