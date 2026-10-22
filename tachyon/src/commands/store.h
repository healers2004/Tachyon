// In-memory key-value data. Knows nothing about RESP or sockets, so storage (Phase 3) and new
// data types (Phase 4) can change it without touching the protocol code.
#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace tachyon {

class Store {
public:
    // std::optional holds either a value or std::nullopt, which here means "no such key".
    std::optional<std::string> get(const std::string& key) const;

    // Creates the key or overwrites its value.
    void set(const std::string& key, const std::string& value);

    // Returns true if the key existed and was removed.
    bool del(const std::string& key);

    bool exists(const std::string& key) const;

private:
    // Hash table from key to value; average O(1) lookup, insert and erase.
    std::unordered_map<std::string, std::string> data_;
};

}  // namespace tachyon
