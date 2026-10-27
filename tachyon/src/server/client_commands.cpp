#include "server/client_commands.h"

#include <cstddef>

#include "commands/commands.h"
#include "protocol/resp_writer.h"

namespace tachyon {

namespace {

std::string wrong_args(const std::string& command) {
    return error_reply("ERR wrong number of arguments for '" + command + "' command");
}

// The reply to each (UN)SUBSCRIBE: *3 "subscribe" <channel> <channels now>. `channel` is
// nullptr for UNSUBSCRIBE with nothing to leave, which Redis answers with a nil name.
std::string confirmation(const char* kind, const std::string* channel, std::size_t count) {
    return array_header(3) + bulk_reply(kind) + (channel ? bulk_reply(*channel) : null_reply()) +
           integer_reply(static_cast<long long>(count));
}

// Redis 7's exact wording; only these commands work while subscribed.
std::string not_allowed(const std::string& command) {
    return error_reply("ERR Can't execute '" + command +
                       "': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are "
                       "allowed in this context");
}

}  // namespace

bool run_client_command(Connection& connection, PubSub& pubsub,
                        const std::vector<std::string>& args) {
    if (args.empty()) return false;
    const std::string& name = args[0];  // lowercased only for error messages
    const bool subscribed = !connection.channels.empty();
    std::string& reply = connection.pending_reply;

    if (is_command(name, "subscribe")) {
        if (args.size() < 2) {
            reply += wrong_args(to_lower(name));
            return true;
        }
        for (std::size_t i = 1; i < args.size(); ++i) {
            reply += confirmation("subscribe", &args[i], pubsub.subscribe(connection, args[i]));
        }
        return true;
    }

    if (is_command(name, "unsubscribe")) {
        if (args.size() > 1) {
            for (std::size_t i = 1; i < args.size(); ++i) {
                reply +=
                    confirmation("unsubscribe", &args[i], pubsub.unsubscribe(connection, args[i]));
            }
        } else if (!subscribed) {
            reply += confirmation("unsubscribe", nullptr, 0);
        } else {
            // No names: leave every channel. Copy the names first, because unsubscribe()
            // removes them from connection.channels, and a set mustn't change while it's being
            // looped over.
            const std::vector<std::string> names(connection.channels.begin(),
                                                 connection.channels.end());
            for (const std::string& channel : names) {
                reply +=
                    confirmation("unsubscribe", &channel, pubsub.unsubscribe(connection, channel));
            }
        }
        return true;
    }

    if (is_command(name, "quit")) {
        reply += simple_string_reply("OK");
        connection.closing = true;  // handle_input stops here; the loop closes after sending
        return true;
    }

    if (is_command(name, "publish")) {
        if (args.size() != 3) {
            reply += wrong_args(to_lower(name));
        } else if (subscribed) {
            reply += not_allowed(to_lower(name));
        } else {
            reply += integer_reply(static_cast<long long>(pubsub.publish(args[1], args[2])));
        }
        return true;
    }

    if (!subscribed) return false;  // a normal command for the store

    // Subscribed mode: PING answers as an array, so it can't be confused with a message.
    if (is_command(name, "ping")) {
        if (args.size() > 2) {
            reply += wrong_args(to_lower(name));
        } else {
            reply += array_header(2) + bulk_reply("pong") +
                     bulk_reply(args.size() == 2 ? args[1] : std::string());
        }
        return true;
    }
    // Like Redis, an unknown command or a wrong argument count is reported before the refusal.
    std::string error = validate_command(args);
    reply += error.empty() ? not_allowed(to_lower(name)) : error;
    return true;
}

}  // namespace tachyon
