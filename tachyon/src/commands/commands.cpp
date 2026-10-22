#include "commands/commands.h"

#include <cctype>
#include <cstddef>
#include <optional>

#include "protocol/resp_writer.h"

namespace tachyon {

namespace {

// Command names are case-insensitive, like Redis: "set" works the same as "SET".
std::string to_upper(std::string text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        // The cast avoids undefined behaviour for bytes above 127 (negative chars).
        text[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
    }
    return text;
}

// Same wording as Redis, so clients and tests see familiar errors.
std::string wrong_arg_count(const std::string& command) {
    return error_reply("ERR wrong number of arguments for '" + command + "' command");
}

// DEL and EXISTS both count keys: DEL counts the ones it removed, EXISTS the ones present.
// Redis counts a key named twice in EXISTS twice, and so does this.
std::string count_keys(Store& store, const std::vector<std::string>& args, bool remove) {
    long long count = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
        bool hit = remove ? store.del(args[i]) : store.exists(args[i]);
        if (hit) {
            ++count;
        }
    }
    return integer_reply(count);
}

}  // namespace

std::string execute_command(Store& store, const std::vector<std::string>& args) {
    // The parser never produces an empty command, but never index into an empty vector.
    if (args.empty()) {
        return error_reply("ERR empty command");
    }
    const std::string name = to_upper(args[0]);
    const std::size_t argc = args.size();

    if (name == "PING") {
        if (argc == 1) return simple_string_reply("PONG");
        if (argc == 2) return bulk_reply(args[1]);
        return wrong_arg_count("ping");
    }
    if (name == "ECHO") {
        if (argc != 2) return wrong_arg_count("echo");
        return bulk_reply(args[1]);
    }
    if (name == "GET") {
        if (argc != 2) return wrong_arg_count("get");
        std::optional<std::string> value = store.get(args[1]);
        if (!value) return null_reply();  // an empty optional tests as false
        return bulk_reply(*value);
    }
    if (name == "SET") {
        if (argc < 3) return wrong_arg_count("set");
        // shortcut: no EX/PX/NX/XX options yet; expiry arrives in Phase 4.
        if (argc > 3) return error_reply("ERR syntax error");
        store.set(args[1], args[2]);
        return simple_string_reply("OK");
    }
    if (name == "DEL") {
        if (argc < 2) return wrong_arg_count("del");
        return count_keys(store, args, true);
    }
    if (name == "EXISTS") {
        if (argc < 2) return wrong_arg_count("exists");
        return count_keys(store, args, false);
    }
    // error_reply replaces any \r\n in the user's text, so this can't inject a reply.
    return error_reply("ERR unknown command '" + args[0] + "'");
}

bool is_write_command(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    const std::string name = to_upper(args[0]);
    return name == "SET" || name == "DEL";
}

}  // namespace tachyon
