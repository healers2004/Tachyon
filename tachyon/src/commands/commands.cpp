#include "commands/commands.h"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <limits>
#include <system_error>

#include "commands/command_util.h"
#include "protocol/resp_writer.h"

namespace tachyon {

namespace {

// One row per command, like Redis's command table. arity counts the name too: 2 means exactly
// "GET key"; a negative arity -N means "at least N", e.g. DEL -2 is "DEL key [key ...]".
struct CommandSpec {
    const char* name;  // lowercase, as Redis prints it in error messages
    int arity;
    bool writes;  // can change the store, so it goes to the write-ahead log
    Handler handler;
};

// shortcut: looked up by a linear scan; a hash map would be faster if the table grows large.
const CommandSpec kCommands[] = {
    {"ping", -1, false, cmd_ping},        {"echo", 2, false, cmd_echo},
    {"get", 2, false, cmd_get},           {"set", -3, true, cmd_set},
    {"del", -2, true, cmd_del},           {"exists", -2, false, cmd_exists},
    {"expire", 3, true, cmd_expire},      {"pexpireat", 3, true, cmd_pexpireat},
    {"ttl", 2, false, cmd_ttl},           {"persist", 2, true, cmd_persist},
    {"type", 2, false, cmd_type},         {"dbsize", 1, false, cmd_dbsize},
    {"flushall", -1, true, cmd_flushall}, {"hset", -4, true, cmd_hset},
    {"hget", 3, false, cmd_hget},         {"hgetall", 2, false, cmd_hgetall},
    {"hdel", -3, true, cmd_hdel},         {"hlen", 2, false, cmd_hlen},
    {"lpush", -3, true, cmd_lpush},       {"rpush", -3, true, cmd_rpush},
    {"lpop", -2, true, cmd_lpop},         {"rpop", -2, true, cmd_rpop},
    {"lrange", 4, false, cmd_lrange},     {"ltrim", 4, true, cmd_ltrim},
    {"llen", 2, false, cmd_llen},
};

// nullptr if there's no such command. Names are case-insensitive, like Redis.
const CommandSpec* find_command(const std::string& name) {
    const std::string lower = to_lower(name);
    // Range-based for: visits each element of the array in order.
    for (const CommandSpec& spec : kCommands) {
        if (lower == spec.name) return &spec;
    }
    return nullptr;
}

bool arity_ok(const CommandSpec& spec, std::size_t argc) {
    if (spec.arity >= 0) return argc == static_cast<std::size_t>(spec.arity);
    return argc >= static_cast<std::size_t>(-spec.arity);
}

// Redis 7's wording: "unknown command 'FLY', with args beginning with: 'a' 'b' ". Arguments
// are added until the list passes 128 characters, each cut to fit.
std::string unknown_command(const std::vector<std::string>& args) {
    std::string listed;
    for (std::size_t i = 1; i < args.size() && listed.size() < 128; ++i) {
        listed += "'" + args[i].substr(0, 128 - listed.size()) + "' ";
    }
    return error_reply("ERR unknown command '" + args[0].substr(0, 128) +
                       "', with args beginning with: " + listed);
}

}  // namespace

// ---- Shared helpers (declared in command_util.h) ----

std::string wrong_type_reply() {
    return error_reply("WRONGTYPE Operation against a key holding the wrong kind of value");
}

std::string not_integer_reply() {
    return error_reply("ERR value is not an integer or out of range");
}

std::string syntax_error_reply() { return error_reply("ERR syntax error"); }

std::string wrong_arg_count(const std::string& command) {
    return error_reply("ERR wrong number of arguments for '" + command + "' command");
}

bool parse_int(const std::string& text, std::int64_t& out) {
    const char* end = text.data() + text.size();
    std::from_chars_result result = std::from_chars(text.data(), end, out);
    return !text.empty() && result.ec == std::errc() && result.ptr == end;
}

std::string to_upper(std::string text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        // The cast avoids undefined behaviour for bytes above 127 (negative chars).
        text[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
    }
    return text;
}

std::string to_lower(std::string text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        text[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
    }
    return text;
}

std::string set_expiry_deadline(const std::string& option, const std::string& value,
                                std::int64_t now_ms, std::int64_t& at_ms) {
    const std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    std::int64_t number = 0;
    if (!parse_int(value, number)) return not_integer_reply();
    const bool seconds = option == "EX" || option == "EXAT";
    const bool relative = option == "EX" || option == "PX";
    // The same checks as Redis, in the same order, so overflow gives the same error.
    if (number <= 0 || (seconds && number > kMax / 1000)) {
        return error_reply("ERR invalid expire time in 'set' command");
    }
    if (seconds) number *= 1000;
    if (relative) {
        if (number > kMax - now_ms) return error_reply("ERR invalid expire time in 'set' command");
        number += now_ms;
    }
    at_ms = number;
    return "";
}

std::string expire_deadline(const std::string& value, bool seconds, std::int64_t base_ms,
                            const std::string& command, std::int64_t& at_ms) {
    const std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    const std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    const std::string invalid = "ERR invalid expire time in '" + command + "' command";
    std::int64_t number = 0;
    if (!parse_int(value, number)) return not_integer_reply();
    if (seconds) {
        if (number > kMax / 1000 || number < kMin / 1000) return error_reply(invalid);
        number *= 1000;
    }
    if (number > kMax - base_ms) return error_reply(invalid);
    at_ms = number + base_ms;
    return "";
}

// ---- Commands that need no data ----

std::string cmd_ping(Store&, const Args& args, std::int64_t) {
    if (args.size() == 1) return simple_string_reply("PONG");
    if (args.size() == 2) return bulk_reply(args[1]);
    return wrong_arg_count("ping");
}

std::string cmd_echo(Store&, const Args& args, std::int64_t) { return bulk_reply(args[1]); }

// ---- Dispatch ----

std::string execute_command(Store& store, const std::vector<std::string>& args,
                            std::int64_t now_ms) {
    // The parser never produces an empty command, but never index into an empty vector.
    if (args.empty()) return error_reply("ERR empty command");

    const CommandSpec* spec = find_command(args[0]);
    // error_reply replaces any \r\n in the user's text, so this can't inject a reply.
    if (spec == nullptr) return unknown_command(args);
    if (!arity_ok(*spec, args.size())) return wrong_arg_count(spec->name);
    return spec->handler(store, args, now_ms);
}

bool is_write_command(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    const CommandSpec* spec = find_command(args[0]);
    return spec != nullptr && spec->writes;
}

std::vector<std::string> with_absolute_times(const std::vector<std::string>& args,
                                             std::int64_t now_ms) {
    if (args.empty()) return args;
    const std::string name = to_upper(args[0]);
    std::vector<std::string> out = args;  // a copy, changed below

    if (name == "EXPIRE" && args.size() == 3) {
        std::int64_t at = 0;
        if (expire_deadline(args[2], true, now_ms, "expire", at).empty()) {
            out = {"PEXPIREAT", args[1], std::to_string(at)};
        }
    } else if (name == "SET") {
        // Walk the options the way SET parses them: EX/PX/EXAT/PXAT take the next argument.
        for (std::size_t i = 3; i < out.size(); ++i) {
            const std::string option = to_upper(out[i]);
            if (option != "EX" && option != "PX" && option != "EXAT" && option != "PXAT") continue;
            if (i + 1 == out.size()) break;
            std::int64_t at = 0;
            if (set_expiry_deadline(option, out[i + 1], now_ms, at).empty()) {
                out[i] = "PXAT";
                out[i + 1] = std::to_string(at);
            }
            ++i;  // skip the value
        }
    }
    return out;
}

}  // namespace tachyon
