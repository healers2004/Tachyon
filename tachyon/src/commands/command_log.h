// Durability for commands, on top of Horizon's storage engine (a snapshot plus a write-ahead
// log). At startup it loads the snapshot and replays the log into the store; while running it
// records each write command, and commit() puts the records on disk before their replies may
// be sent. save() writes a snapshot of the whole store so the log can start again empty.
// This is the only tachyon code that uses Horizon.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "commands/store.h"
#include "horizon/engine.h"

namespace tachyon {

// The data files' names in the data directory: tachyon.snapshot, tachyon.log.<N>.
const char* const kDataFileName = "tachyon";

class CommandLog {
public:
    // Loads the snapshot and log in `dir` into `store`, cuts off a half-written last record,
    // and opens the log for appending. false + `error` if the files are damaged or can't be
    // read: starting anyway could lose acknowledged writes.
    bool open(const std::string& dir, horizon::FlushPolicy policy, Store& store,
              std::string& error);

    // Adds the command to the log if it's a write. Its reply must not reach the client until
    // commit() has succeeded. false if the command is too big to log; don't run it then.
    bool record(const std::vector<std::string>& args);

    // See horizon::Log::commit() and sync(). After a false return, stop the server.
    bool commit(std::string& error) { return engine_.commit(error); }
    bool sync(std::string& error) { return engine_.sync(error); }

    // Writes a snapshot of `store` (keys already expired at `now_ms` are left out) and starts
    // a new, empty log. kFailed leaves everything as it was; kFatal means stop the server.
    horizon::SnapshotResult save(const Store& store, std::int64_t now_ms, std::string& error);

    std::uint64_t log_size() const { return engine_.log_size(); }
    std::uint64_t generation() const { return engine_.generation(); }
    std::size_t snapshot_commands() const { return snapshot_commands_; }
    std::size_t replayed_commands() const { return replayed_commands_; }
    std::uint64_t dropped_bytes() const { return engine_.dropped_bytes(); }
    // Size of the snapshot file in charge (0 if none); the next automatic snapshot waits until
    // the log is at least this big.
    std::uint64_t snapshot_bytes() const { return engine_.snapshot_bytes(); }
    // True if open() renamed a pre-1.0 tachyon.wal into tachyon.log.1.
    bool migrated_legacy_log() const { return migrated_legacy_log_; }

    // Test hook, see horizon::Engine::set_crash_point.
    void set_crash_point(const std::string& step) { engine_.set_crash_point(step); }

private:
    horizon::Engine engine_;
    std::size_t snapshot_commands_ = 0;
    bool migrated_legacy_log_ = false;
    std::size_t replayed_commands_ = 0;
};

// The commands that recreate one key: SET, or HSET / RPUSH split into chunks that stay within
// the parser's limits on replay, then PEXPIREAT if the key has an expiry.
std::vector<std::vector<std::string>> rebuild_commands(const std::string& key, const Value& value,
                                                       std::int64_t expiry_ms);

}  // namespace tachyon
