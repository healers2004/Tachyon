// Horizon's storage engine: a snapshot plus a write-ahead log, kept in one directory.
//
// Files, for the name "tachyon":
//   tachyon.snapshot         the latest complete snapshot (missing until the first one)
//   tachyon.snapshot.tmp     a snapshot being written; a leftover one is deleted on open
//   tachyon.log.<N>          the log of changes made after the snapshot, generation N
//
// Generations make compaction crash-safe. A snapshot records the generation of the log that
// continues after it. Startup loads the snapshot, replays only that generation's log and
// deletes older ones. A crash at any step of a snapshot therefore either keeps the old
// snapshot and old log, or uses the new snapshot and new log: nothing is lost or applied twice.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "horizon/log.h"

namespace horizon {

enum class RecoveryStatus {
    kRecord,  // a stored record was read into `payload`
    kDone,    // every record has been returned
    kError,   // the files are damaged or inconsistent; see `error`
};

enum class SnapshotResult {
    kSaved,   // the new snapshot is in charge and a new, empty log has started
    kFailed,  // nothing changed: the old snapshot and log are still in charge (e.g. disk full)
    kFatal,   // the new snapshot is in place but the new log couldn't start: stop writing
};

class Engine {
public:
    Engine();
    ~Engine();

    // 1. Opens `dir` (which must exist) and works out which files hold the current state.
    //    false + `error` if they're damaged or inconsistent, e.g. a log newer than the snapshot.
    bool open(const std::string& dir, const std::string& name, FlushPolicy policy,
              std::string& error);

    // 2. Returns the stored records in order: the snapshot's, then the log's. Call until kDone.
    //    A half-written last log record (a crash mid-write) is skipped and counted.
    RecoveryStatus next(std::string& payload, std::string& error);

    // 3. Opens the log for new records, cutting off any half-written record found above.
    bool start(std::string& error);

    // Writing, after start(); the same as Log's.
    bool append(std::span<const char> payload) { return log_.append(payload); }
    bool commit(std::string& error) { return log_.commit(error); }
    bool sync(std::string& error) { return log_.sync(error); }
    std::uint64_t log_size() const { return log_.size(); }

    // A snapshot: begin, add records that together rebuild the current state, then finish.
    // Done synchronously, so nothing is appended to the log in between. A failure before
    // finish() succeeds leaves the old snapshot and log in charge, and removes the temp file.
    bool begin_snapshot(std::string& error);
    // Records are written to the temp file every few megabytes as they're added, so a big
    // snapshot is never held in memory all at once. false + `error` if the payload is too big
    // or the write fails (e.g. a full disk); then abandon_snapshot().
    bool add_to_snapshot(std::span<const char> payload, std::string& error);
    SnapshotResult finish_snapshot(std::string& error);
    // Gives up on a snapshot after begin_snapshot(): the temp file is removed, nothing changes.
    void abandon_snapshot();

    std::uint64_t generation() const { return generation_; }
    std::uint64_t snapshot_records() const { return snapshot_records_; }
    // Size in bytes of the snapshot in charge, 0 if there is none.
    std::uint64_t snapshot_bytes() const { return snapshot_bytes_; }
    std::uint64_t dropped_bytes() const { return dropped_bytes_; }

    // Test hook for the crash test: makes the process abort right after the named step of a
    // snapshot ("snapshot-partial", "snapshot-written", "snapshot-renamed", "new-log-created").
    void set_crash_point(const std::string& step) { crash_point_ = step; }

private:
    enum class Phase { kSnapshot, kLog, kDone };

    std::string path(const std::string& suffix) const;  // dir_/name_<suffix>
    std::string log_path(std::uint64_t generation) const;
    bool fail(std::string& error, const std::string& message);
    void crash_if(const char* step) const;

    std::string dir_;
    std::string name_;
    FlushPolicy policy_ = FlushPolicy::kEveryWrite;
    std::uint64_t generation_ = 1;  // of the active log
    Log log_;

    // Recovery.
    Phase phase_ = Phase::kDone;
    LogReader reader_;
    std::uint64_t snapshot_records_ = 0;
    std::uint64_t valid_log_bytes_ = 0;
    std::uint64_t dropped_bytes_ = 0;

    // A snapshot being written (to the temp file), or nullptr.
    std::unique_ptr<Log> snapshot_;
    std::uint64_t snapshot_count_ = 0;
    std::uint64_t snapshot_bytes_ = 0;

    std::string crash_point_;
};

}  // namespace horizon
