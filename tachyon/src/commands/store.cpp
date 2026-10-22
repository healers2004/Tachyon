#include "commands/store.h"

namespace tachyon {

std::optional<std::string> Store::get(const std::string& key) const {
    // count() is 0 or 1 for a map. at() returns the value; it would throw on a missing key.
    if (data_.count(key) == 0) {
        return std::nullopt;
    }
    return data_.at(key);
}

// operator[] inserts the key if it's missing, then assigns the value.
void Store::set(const std::string& key, const std::string& value) { data_[key] = value; }

// erase() returns how many entries it removed: 0 or 1.
bool Store::del(const std::string& key) { return data_.erase(key) > 0; }

bool Store::exists(const std::string& key) const { return data_.count(key) > 0; }

}  // namespace tachyon
