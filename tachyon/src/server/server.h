// The TCP server: accepts clients, feeds their bytes to the parser and sends back replies.
#pragma once

#include <string>

#include "horizon/log.h"

namespace tachyon {

// Never Redis's 6379, so tachyon can't clash with a real Redis on the same machine.
const unsigned short kDefaultPort = 6380;

// The write-ahead log's file name inside the data directory.
const char* const kLogFileName = "tachyon.wal";

// Settings from the command line. The defaults are what plain `tachyon` runs with.
struct ServerOptions {
    std::string ip = "127.0.0.1";  // localhost only: nothing else on the network can connect
    unsigned short port = kDefaultPort;
    bool persist = true;    // false with --no-persist: memory only, nothing written to disk
    std::string dir = ".";  // data directory, which must already exist
    horizon::FlushPolicy flush = horizon::FlushPolicy::kEveryWrite;
};

// Asks run_server to stop. Safe to call from a signal handler: it only sets a flag.
void request_stop();

// Loads the log (unless options.persist is false), then serves any number of clients at once,
// on one thread, until request_stop() is called.
// Returns the process exit code: 0 after a clean stop, 1 if the server failed.
int run_server(const ServerOptions& options);

}  // namespace tachyon
