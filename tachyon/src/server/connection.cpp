#include "server/connection.h"

#include <iostream>

#include "commands/commands.h"
#include "protocol/resp_writer.h"

namespace tachyon {

void handle_input(Connection& connection, Store& store, std::string_view data) {
    connection.parser.feed(data);

    // One read can hold several commands (clients may pipeline them), so handle them all.
    // shortcut: replies are queued without a size check, so a client pipelining many large
    // GETs can grow pending_reply past the read limit; Redis disconnects such clients
    // (client-output-buffer-limit), worth adding in Phase 7.
    while (true) {
        ParseResult result = connection.parser.next();
        if (result.status == ParseStatus::kNeedMore) return;
        if (result.status == ParseStatus::kError) {
            // Like Redis: say why, then hang up, since the rest of the stream can't be trusted.
            connection.pending_reply += error_reply("ERR " + result.error);
            connection.closing = true;
            std::cout << "tachyon: closing client: " << result.error << std::endl;
            return;
        }
        connection.pending_reply += execute_command(store, result.args);
    }
}

}  // namespace tachyon
