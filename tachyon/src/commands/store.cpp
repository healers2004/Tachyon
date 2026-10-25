#include "commands/store.h"

#include <chrono>
#include <utility>

namespace tachyon {

std::int64_t current_time_ms() {
    // system_clock is wall-clock time; time_since_epoch() is the time since 1970-01-01, and
    // duration_cast converts it to whole milliseconds.
    std::chrono::system_clock::duration since_epoch =
        std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
}

Value* Store::find(const std::string& key, std::int64_t now_ms) {
    // find() returns an iterator: a position in the map, or end() if the key isn't there.
    std::unordered_map<std::string, Value>::iterator it = data_.find(key);
    if (it == data_.end()) return nullptr;

    std::int64_t at = expiry(key);
    if (at != kNoExpiry && at <= now_ms) {
        del(key);
        return nullptr;
    }
    return &it->second;  // it->first is the key, it->second the value
}

Value& Store::set(const std::string& key, Value value, bool keep_expiry) {
    if (!keep_expiry) expires_.erase(key);
    // std::move: hand the value's memory over instead of copying it.
    Value& stored = data_[key];
    stored = std::move(value);
    return stored;
}

bool Store::del(const std::string& key) {
    expires_.erase(key);
    return data_.erase(key) > 0;  // erase() returns how many entries it removed: 0 or 1
}

std::int64_t Store::expiry(const std::string& key) const {
    std::unordered_map<std::string, std::int64_t>::const_iterator it = expires_.find(key);
    return it == expires_.end() ? kNoExpiry : it->second;
}

void Store::set_expiry(const std::string& key, std::int64_t at_ms) { expires_[key] = at_ms; }

bool Store::clear_expiry(const std::string& key) { return expires_.erase(key) > 0; }

void Store::clear() {
    data_.clear();
    expires_.clear();
}

std::size_t Store::remove_expired(std::int64_t now_ms) {
    // shortcut: checks every key with an expiry on each call. Redis samples 20 at random and
    // repeats while many are expired, which bounds the work per call for huge keyspaces.
    std::size_t removed = 0;
    std::unordered_map<std::string, std::int64_t>::iterator it = expires_.begin();
    while (it != expires_.end()) {
        if (it->second <= now_ms) {
            data_.erase(it->first);
            // erase() invalidates `it` but returns the position of the next entry.
            it = expires_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

}  // namespace tachyon
