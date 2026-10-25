// HSET, HGET, HGETALL, HDEL, HLEN.
#include "commands/command_util.h"
#include "protocol/resp_writer.h"

namespace tachyon {

// HSET key field value [field value ...]: returns how many fields were new.
std::string cmd_hset(Store& store, const Args& args, std::int64_t now_ms) {
    if (args.size() % 2 != 0) return wrong_arg_count("hset");  // fields and values must pair up
    bool wrong_type = false;
    Hash* hash = find_or_create<Hash>(store, args[1], now_ms, wrong_type);
    if (wrong_type) return wrong_type_reply();

    long long added = 0;
    for (std::size_t i = 2; i < args.size(); i += 2) {
        // insert_or_assign returns {position, inserted}; .second is true for a new field.
        if (hash->insert_or_assign(args[i], args[i + 1]).second) ++added;
    }
    return integer_reply(added);
}

std::string cmd_hget(Store& store, const Args& args, std::int64_t now_ms) {
    bool wrong_type = false;
    const Hash* hash = find_as<Hash>(store, args[1], now_ms, wrong_type);
    if (wrong_type) return wrong_type_reply();
    if (hash == nullptr) return null_reply();
    Hash::const_iterator field = hash->find(args[2]);
    if (field == hash->end()) return null_reply();
    return bulk_reply(field->second);
}

// Returns field1, value1, field2, value2, ... A missing key is an empty hash.
// shortcut: the order is the hash table's; Redis keeps insertion order for small hashes.
std::string cmd_hgetall(Store& store, const Args& args, std::int64_t now_ms) {
    bool wrong_type = false;
    const Hash* hash = find_as<Hash>(store, args[1], now_ms, wrong_type);
    if (wrong_type) return wrong_type_reply();
    if (hash == nullptr) return array_header(0);

    std::string reply = array_header(hash->size() * 2);
    // Each element of a map is a pair: .first is the field, .second the value.
    for (const std::pair<const std::string, std::string>& entry : *hash) {
        reply += bulk_reply(entry.first);
        reply += bulk_reply(entry.second);
    }
    return reply;
}

// HDEL key field [field ...]: returns how many were removed. An emptied hash is deleted, as
// Redis never keeps empty hashes or lists.
std::string cmd_hdel(Store& store, const Args& args, std::int64_t now_ms) {
    bool wrong_type = false;
    Hash* hash = find_as<Hash>(store, args[1], now_ms, wrong_type);
    if (wrong_type) return wrong_type_reply();
    if (hash == nullptr) return integer_reply(0);

    long long removed = 0;
    for (std::size_t i = 2; i < args.size(); ++i) {
        removed += static_cast<long long>(hash->erase(args[i]));
    }
    if (hash->empty()) store.del(args[1]);  // `hash` dangles after this; don't use it again
    return integer_reply(removed);
}

std::string cmd_hlen(Store& store, const Args& args, std::int64_t now_ms) {
    bool wrong_type = false;
    const Hash* hash = find_as<Hash>(store, args[1], now_ms, wrong_type);
    if (wrong_type) return wrong_type_reply();
    return integer_reply(hash == nullptr ? 0 : static_cast<long long>(hash->size()));
}

}  // namespace tachyon
