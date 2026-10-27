// Everything a running server's commands share: the data, the log, the pub/sub channels, the
// settings and some counters for INFO. run_server owns one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "commands/command_log.h"
#include "commands/store.h"
#include "horizon/log.h"
#include "server/pubsub.h"

namespace tachyon {

// Never Redis's 6379, so tachyon can't clash with a real Redis on the same machine.
const unsigned short kDefaultPort = 6380;

// Settings from the command line. The defaults are what plain `tachyon` runs with.
struct ServerOptions {
    std::string ip = "127.0.0.1";  // localhost only: nothing else on the network can connect
    unsigned short port = kDefaultPort;
    bool persist = true;    // false with --no-persist: memory only, nothing written to disk
    std::string dir = ".";  // data directory, which must already exist
    horizon::FlushPolicy flush = horizon::FlushPolicy::kEveryWrite;
    // Take a snapshot (and start a new, empty log) once the log reaches this size; 0 = never.
    std::uint64_t snapshot_after_bytes = 64 * 1024 * 1024;
    std::string crash_point;  // test hook for the crash test, see horizon::Engine
};

// Counters reported by INFO.
struct ServerStats {
    std::int64_t start_ms = 0;
    std::uint64_t connections_received = 0;
    std::uint64_t commands_processed = 0;
    std::size_t connected_clients = 0;
    std::uint64_t changes_since_save = 0;  // write commands since the last snapshot
    std::int64_t last_save_ms = 0;         // 0: never
    bool last_save_ok = true;
    std::uint64_t saves = 0;
};

struct ServerContext {
    ServerOptions options;
    Store store;
    CommandLog* log = nullptr;  // nullptr with --no-persist
    // Connections point into this, so a ServerContext must be declared before (and so be
    // destroyed after) the connections that use it.
    PubSub pubsub;
    ServerStats stats;
    // The next automatic snapshot happens once the log reaches this size: the threshold, or the
    // last snapshot's size if that's bigger (as Redis does), so a big dataset isn't rewritten
    // after every few writes. Set by every successful snapshot, SAVE included.
    std::uint64_t next_snapshot_at = 0;
    // Set when the data on disk can no longer be trusted to follow memory (a failed log write,
    // or a snapshot that couldn't start its new log). The event loop then stops.
    bool fatal = false;
};

}  // namespace tachyon
