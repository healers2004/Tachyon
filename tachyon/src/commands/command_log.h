// Durability for commands, on top of Horizon's write-ahead log. At startup it replays the log
// into the store; while running it records each write command, and commit() puts the records
// on disk before their replies may be sent. This is the only tachyon code that uses Horizon.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "commands/store.h"
#include "horizon/log.h"

namespace tachyon {

class CommandLog {
public:
    // Replays the log at `path` (if it exists) into `store`, cuts off a half-written last
    // record, and opens the log for appending. false + `error` if the log is damaged anywhere
    // but the end, or can't be read or opened: starting anyway could lose acknowledged writes.
    bool open(const std::string& path, horizon::FlushPolicy policy, Store& store,
              std::string& error);

    // Adds the command to the log if it's a write. Its reply must not reach the client until
    // commit() has succeeded. false if the command is too big to log; don't run it then.
    bool record(const std::vector<std::string>& args);

    // See horizon::Log::commit() and sync(). After a false return, stop the server.
    bool commit(std::string& error) { return log_.commit(error); }
    bool sync(std::string& error) { return log_.sync(error); }

    std::size_t replayed_commands() const { return replayed_commands_; }
    std::uint64_t dropped_bytes() const { return dropped_bytes_; }  // size of a torn tail

private:
    // Reads every record into `store`; returns the byte count of the valid ones, or false.
    bool replay(const std::string& path, Store& store, std::uint64_t& valid_bytes,
                std::string& error);

    horizon::Log log_;
    std::size_t replayed_commands_ = 0;
    std::uint64_t dropped_bytes_ = 0;
};

}  // namespace tachyon
