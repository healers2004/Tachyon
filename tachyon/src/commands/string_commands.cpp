// GET, SET, DEL, EXISTS.
#include "commands/command_util.h"
#include "protocol/resp_writer.h"

namespace tachyon {

std::string cmd_get(Store& store, const Args& args, std::int64_t now_ms) {
    bool wrong_type = false;
    const std::string* value = find_as<std::string>(store, args[1], now_ms, wrong_type);
    if (wrong_type) return wrong_type_reply();
    if (value == nullptr) return null_reply();
    return bulk_reply(*value);
}

// SET key value [NX | XX] [EX s | PX ms | EXAT unix-s | PXAT unix-ms | KEEPTTL]
std::string cmd_set(Store& store, const Args& args, std::int64_t now_ms) {
    bool nx = false;  // only if the key doesn't exist
    bool xx = false;  // only if it does
    bool keep_ttl = false;
    std::string expire_option;  // "EX", "PX", "EXAT" or "PXAT"; empty if none
    std::string expire_value;

    // Like Redis: any repeated or conflicting option is a syntax error, found before the
    // expiry number itself is checked.
    for (std::size_t i = 3; i < args.size(); ++i) {
        const std::string option = to_upper(args[i]);
        const bool is_expire =
            option == "EX" || option == "PX" || option == "EXAT" || option == "PXAT";
        if (option == "NX" && !xx) {
            nx = true;
        } else if (option == "XX" && !nx) {
            xx = true;
        } else if (option == "KEEPTTL" && expire_option.empty()) {
            keep_ttl = true;
        } else if (is_expire && expire_option.empty() && !keep_ttl && i + 1 < args.size()) {
            expire_option = option;
            expire_value = args[++i];
        } else {
            return syntax_error_reply();
        }
    }

    std::int64_t at_ms = kNoExpiry;
    if (!expire_option.empty()) {
        std::string error = set_expiry_deadline(expire_option, expire_value, now_ms, at_ms);
        if (!error.empty()) return error;
    }

    // SET works on a key of any type; NX/XX only care whether it exists.
    const bool exists = store.find(args[1], now_ms) != nullptr;
    if ((nx && exists) || (xx && !exists)) return null_reply();

    store.set(args[1], args[2], keep_ttl);  // a std::string converts to the Value variant
    // A deadline already in the past (e.g. PXAT 1) is allowed: the key is gone on next access.
    if (at_ms != kNoExpiry) store.set_expiry(args[1], at_ms);
    return simple_string_reply("OK");
}

// DEL counts the keys it removed; an expired key counts as already gone.
std::string cmd_del(Store& store, const Args& args, std::int64_t now_ms) {
    long long count = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (store.find(args[i], now_ms) != nullptr && store.del(args[i])) ++count;
    }
    return integer_reply(count);
}

// Redis counts a key named twice twice, and so does this.
std::string cmd_exists(Store& store, const Args& args, std::int64_t now_ms) {
    long long count = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (store.find(args[i], now_ms) != nullptr) ++count;
    }
    return integer_reply(count);
}

}  // namespace tachyon
