#include "server/connection.h"

#include "commands/commands.h"
#include "protocol/resp_writer.h"
#include "server/client_commands.h"
#include "server/logging.h"
#include "server/pubsub.h"
#include "server/server_commands.h"

namespace tachyon {

Connection::~Connection() {
    if (pubsub != nullptr) pubsub->unsubscribe_all(*this);
}

bool over_output_limit(const Connection& connection) {
    return !connection.channels.empty() &&
           connection.pending_reply.size() > kMaxSubscriberBacklogBytes;
}

void handle_input(Connection& connection, ServerContext& context, std::string_view data) {
    connection.parser.feed(data);

    // One read can hold several commands (clients may pipeline them), so handle them all.
    // shortcut: a normal client's replies are queued without a size check, so pipelining many
    // large GETs can grow pending_reply past the read limit; Redis has no limit for normal
    // clients by default either, only for subscribers (over_output_limit).
    while (true) {
        ParseResult result = connection.parser.next();
        if (result.status == ParseStatus::kNeedMore) return;
        if (result.status == ParseStatus::kError) {
            // Like Redis: say why, then hang up, since the rest of the stream can't be trusted.
            connection.pending_reply += error_reply("ERR " + result.error);
            connection.closing = true;
            log_line(LogLevel::kVerbose, "closing client: " + result.error);
            return;
        }

        ++context.stats.commands_processed;

        // SUBSCRIBE, PUBLISH, QUIT and subscribed mode: nothing here touches the store or the log.
        if (run_client_command(connection, context.pubsub, result.args)) {
            if (connection.closing) return;  // QUIT: ignore anything sent after it
            continue;
        }
        // SAVE and INFO, which need the whole server.
        if (run_server_command(context, result.args, connection.pending_reply)) continue;

        // EXPIRE k 10 runs (and is logged) as PEXPIREAT k <deadline>, so a replay later sets
        // the same deadline instead of a fresh 10 seconds.
        const std::int64_t now_ms = current_time_ms();
        make_times_absolute(result.args, now_ms);
        const std::vector<std::string>& args = result.args;

        // Write-ahead: the command goes into the log before it changes the store, so the
        // store is never ahead of what's on disk.
        if (context.log != nullptr && !context.log->record(args)) {
            connection.pending_reply += error_reply("ERR command too large to log");
            continue;
        }
        if (is_write_command(args)) ++context.stats.changes_since_save;
        connection.pending_reply += execute_command(context.store, args, now_ms);
    }
}

}  // namespace tachyon
