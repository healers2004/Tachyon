// In-memory keyspace: every key holds a string, a hash or a list, and may have an expiry time.
// Knows nothing about RESP or sockets, so storage and commands can change it without touching
// the protocol code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <variant>

namespace tachyon {

// A hash: field -> value. Average O(1) lookup, insert and erase.
using Hash = std::unordered_map<std::string, std::string>;
// A list. std::deque: O(1) push and pop at both ends, and O(1) access by index.
using List = std::deque<std::string>;
// std::variant holds exactly one of these at a time and remembers which one, so a key can be
// any type and a command can ask "is this a list?" (std::get_if) before using it.
using Value = std::variant<std::string, Hash, List>;

// Expiry times are Unix time in milliseconds, like Redis, so they mean the same after a
// restart. kNoExpiry: the key lives until it's deleted.
const std::int64_t kNoExpiry = -1;

// The current Unix time in milliseconds, from the system clock.
std::int64_t current_time_ms();

class Store {
public:
    // The key's value, or nullptr if there's no such key. A key whose expiry time has passed is
    // deleted here and reported missing (lazy expiry). The pointer stays valid until the next
    // call that adds or removes a key.
    Value* find(const std::string& key, std::int64_t now_ms);

    // Stores `value` under `key`, replacing whatever was there (of any type). Clears the key's
    // expiry unless keep_expiry is true. Returns the stored value.
    Value& set(const std::string& key, Value value, bool keep_expiry = false);

    // Removes the key and its expiry. Returns true if the key existed.
    bool del(const std::string& key);

    // The key's expiry time, or kNoExpiry. Doesn't check whether the key exists.
    std::int64_t expiry(const std::string& key) const;
    void set_expiry(const std::string& key, std::int64_t at_ms);  // the key must exist
    bool clear_expiry(const std::string& key);                    // true if the key had an expiry

    // Number of keys, including expired ones not yet removed (Redis's DBSIZE does the same).
    std::size_t size() const { return data_.size(); }
    void clear();

    // Active expiry: deletes every key whose time has passed, so expired keys nobody reads
    // don't use memory forever. Returns how many were removed.
    std::size_t remove_expired(std::int64_t now_ms);

private:
    std::unordered_map<std::string, Value> data_;
    // Only keys with an expiry are here, so the sweep doesn't have to look at every key.
    std::unordered_map<std::string, std::int64_t> expires_;
};

}  // namespace tachyon
