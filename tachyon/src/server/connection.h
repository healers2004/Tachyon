// Per-client state for the event loop: the socket, the parser holding a half-received command,
// the reply bytes still waiting to be sent, and the pub/sub channels it listens on.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "commands/command_log.h"
#include "commands/store.h"
#include "platform/socket.h"
#include "protocol/resp_parser.h"

namespace tachyon {

class PubSub;

// A subscriber that falls this far behind is disconnected (Redis's pub/sub hard limit), since
// messages to it would otherwise pile up in memory without bound.
const std::size_t kMaxSubscriberBacklogBytes = 32 * 1024 * 1024;

struct Connection {
    // std::move hands the socket's ownership to the member; `socket` is left empty.
    explicit Connection(Socket s) : socket(std::move(s)) {}

    // Leaves every channel first, so PubSub never holds a pointer to a deleted connection,
    // however the connection ends: hang-up, error, QUIT or server shutdown.
    ~Connection();

    // PubSub stores this object's address, so it must never be copied or moved elsewhere.
    // (Declaring the copy operations deleted also stops the compiler generating moves.)
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Socket socket;
    RespParser parser;
    std::string pending_reply;  // write buffer: reply bytes the OS hasn't accepted yet
    bool closing = false;       // stop reading; close once pending_reply has been sent

    std::unordered_set<std::string> channels;  // not empty: the client is in subscribed mode
    PubSub* pubsub = nullptr;                  // set by its first SUBSCRIBE
};

// True for a subscriber whose unsent messages passed kMaxSubscriberBacklogBytes.
bool over_output_limit(const Connection& connection);

// Feeds bytes received from the client to its parser, runs every complete command and
// appends the replies to pending_reply. After a protocol error or QUIT, appends the reply,
// sets `closing` and ignores the rest of the input. Does no socket I/O.
// `log` is nullptr with --no-persist. Otherwise every write command is recorded in it before it
// runs, and the queued replies must not be sent until log->commit() succeeds.
// `pubsub` must outlive every connection that subscribes through it.
void handle_input(Connection& connection, Store& store, CommandLog* log, PubSub& pubsub,
                  std::string_view data);

}  // namespace tachyon
