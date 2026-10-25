// Commands about keys rather than values: expiry, TYPE, DBSIZE, FLUSHALL.
#include "commands/command_util.h"
#include "protocol/resp_writer.h"

namespace tachyon {

namespace {

// EXPIRE and PEXPIREAT: parse the time first (so a bad number is an error even for a missing
// key), then set it. A time already passed deletes the key, as in Redis.
std::string expire_generic(Store& store, const Args& args, std::int64_t now_ms, bool seconds,
                           std::int64_t base_ms, const std::string& command) {
    std::int64_t at_ms = 0;
    std::string error = expire_deadline(args[2], seconds, base_ms, command, at_ms);
    if (!error.empty()) return error;

    if (store.find(args[1], now_ms) == nullptr) return integer_reply(0);
    if (at_ms <= now_ms) {
        store.del(args[1]);
    } else {
        store.set_expiry(args[1], at_ms);
    }
    return integer_reply(1);
}

}  // namespace

// shortcut: EXPIRE's NX/XX/GT/LT options (Redis 7) aren't supported yet.
std::string cmd_expire(Store& store, const Args& args, std::int64_t now_ms) {
    return expire_generic(store, args, now_ms, true, now_ms, "expire");
}

// The absolute form that EXPIRE is rewritten into for the log.
std::string cmd_pexpireat(Store& store, const Args& args, std::int64_t now_ms) {
    return expire_generic(store, args, now_ms, false, 0, "pexpireat");
}

// -2: no such key; -1: no expiry; otherwise seconds left, rounded to nearest like Redis.
std::string cmd_ttl(Store& store, const Args& args, std::int64_t now_ms) {
    if (store.find(args[1], now_ms) == nullptr) return integer_reply(-2);
    std::int64_t at_ms = store.expiry(args[1]);
    if (at_ms == kNoExpiry) return integer_reply(-1);
    std::int64_t left_ms = at_ms - now_ms;
    if (left_ms < 0) left_ms = 0;
    return integer_reply((left_ms + 500) / 1000);
}

std::string cmd_persist(Store& store, const Args& args, std::int64_t now_ms) {
    if (store.find(args[1], now_ms) == nullptr) return integer_reply(0);
    return integer_reply(store.clear_expiry(args[1]) ? 1 : 0);
}

std::string cmd_type(Store& store, const Args& args, std::int64_t now_ms) {
    const Value* value = store.find(args[1], now_ms);
    if (value == nullptr) return simple_string_reply("none");
    // holds_alternative<T>: does the variant currently hold a T?
    if (std::holds_alternative<std::string>(*value)) return simple_string_reply("string");
    if (std::holds_alternative<Hash>(*value)) return simple_string_reply("hash");
    return simple_string_reply("list");
}

std::string cmd_dbsize(Store& store, const Args&, std::int64_t) {
    return integer_reply(static_cast<long long>(store.size()));
}

// FLUSHALL [SYNC | ASYNC]. Both empty the store at once; there's no background thread here.
std::string cmd_flushall(Store& store, const Args& args, std::int64_t) {
    if (args.size() > 2) return syntax_error_reply();
    if (args.size() == 2) {
        const std::string mode = to_upper(args[1]);
        if (mode != "SYNC" && mode != "ASYNC") return syntax_error_reply();
    }
    store.clear();
    return simple_string_reply("OK");
}

}  // namespace tachyon
