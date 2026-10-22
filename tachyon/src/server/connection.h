// Per-client state for the event loop: the socket, the parser holding a half-received command,
// and the reply bytes still waiting to be sent.
#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "commands/command_log.h"
#include "commands/store.h"
#include "platform/socket.h"
#include "protocol/resp_parser.h"

namespace tachyon {

struct Connection {
    // std::move hands the socket's ownership to the member; `socket` is left empty.
    explicit Connection(Socket s) : socket(std::move(s)) {}

    Socket socket;
    RespParser parser;
    std::string pending_reply;  // write buffer: reply bytes the OS hasn't accepted yet
    bool closing = false;       // stop reading; close once pending_reply has been sent
};

// Feeds bytes received from the client to its parser, runs every complete command and
// appends the replies to pending_reply. After a protocol error, appends the error reply,
// sets `closing` and ignores the rest of the input. Does no socket I/O.
// `log` is nullptr with --no-persist. Otherwise every write command is recorded in it before it
// runs, and the queued replies must not be sent until log->commit() succeeds.
void handle_input(Connection& connection, Store& store, CommandLog* log, std::string_view data);

}  // namespace tachyon
