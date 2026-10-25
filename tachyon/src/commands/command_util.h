// Helpers shared by the command handlers: argument parsing, common error replies and typed key
// lookup. Internal to commands/.
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "commands/store.h"

namespace tachyon {

// Every handler has this shape, so the command table can point at any of them.
// `now_ms` is passed in rather than read inside, so tests can run commands at any time.
using Args = std::vector<std::string>;
using Handler = std::string (*)(Store& store, const Args& args, std::int64_t now_ms);

// Redis's exact error texts, so clients see the messages they expect.
std::string wrong_type_reply();   // the key holds a different type
std::string not_integer_reply();  // an argument isn't a valid 64-bit integer
std::string syntax_error_reply();
std::string wrong_arg_count(const std::string& command);

// Parses a whole string as a signed 64-bit integer, like Redis: no spaces, no '+'.
bool parse_int(const std::string& text, std::int64_t& out);

std::string to_upper(std::string text);
std::string to_lower(std::string text);

// A function template: the compiler writes one copy per type T it's used with, e.g.
// find_as<Hash>(...) and find_as<List>(...). Templates live in headers because the compiler
// needs the body wherever a new T is used.
//
// Returns the key's value as a T, or nullptr if the key is missing. If the key holds another
// type, returns nullptr and sets wrong_type, and the command must reply wrong_type_reply().
template <typename T>
T* find_as(Store& store, const std::string& key, std::int64_t now_ms, bool& wrong_type) {
    wrong_type = false;
    Value* value = store.find(key, now_ms);
    if (value == nullptr) return nullptr;
    T* typed = std::get_if<T>(value);  // nullptr unless the variant currently holds a T
    if (typed == nullptr) wrong_type = true;
    return typed;
}

// Like find_as, but a missing key is created holding an empty T (as HSET or LPUSH do).
// Returns nullptr only when the key holds another type.
template <typename T>
T* find_or_create(Store& store, const std::string& key, std::int64_t now_ms, bool& wrong_type) {
    T* typed = find_as<T>(store, key, now_ms, wrong_type);
    if (typed != nullptr || wrong_type) return typed;
    return &std::get<T>(store.set(key, T()));  // T(): an empty string, hash or list
}

// Handlers, grouped by file.
std::string cmd_ping(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_echo(Store& store, const Args& args, std::int64_t now_ms);

// string_commands.cpp
std::string cmd_get(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_set(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_del(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_exists(Store& store, const Args& args, std::int64_t now_ms);

// key_commands.cpp
std::string cmd_expire(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_pexpireat(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_ttl(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_persist(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_type(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_dbsize(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_flushall(Store& store, const Args& args, std::int64_t now_ms);

// hash_commands.cpp
std::string cmd_hset(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_hget(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_hgetall(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_hdel(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_hlen(Store& store, const Args& args, std::int64_t now_ms);

// list_commands.cpp
std::string cmd_lpush(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_rpush(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_lpop(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_rpop(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_lrange(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_ltrim(Store& store, const Args& args, std::int64_t now_ms);
std::string cmd_llen(Store& store, const Args& args, std::int64_t now_ms);

// Expiry arguments, shared by SET and EXPIRE and by the log rewrite in commands.cpp.
//
// SET's EX/PX/EXAT/PXAT value: must be positive. Sets at_ms to the absolute deadline, or returns
// an error reply (not an integer, or "invalid expire time" on <= 0 or overflow).
std::string set_expiry_deadline(const std::string& option, const std::string& value,
                                std::int64_t now_ms, std::int64_t& at_ms);
// EXPIRE's seconds (base = now) or PEXPIREAT's milliseconds (base = 0): may be negative, which
// means "already expired". Same error replies as above.
std::string expire_deadline(const std::string& value, bool seconds, std::int64_t base_ms,
                            const std::string& command, std::int64_t& at_ms);

}  // namespace tachyon
