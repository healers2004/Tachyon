#include "horizon/engine.h"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

#include "platform/file.h"

namespace horizon {

namespace {

// std::filesystem: portable paths, directory listing and file removal. Every call here takes
// a std::error_code, which it sets on failure instead of throwing an exception.
namespace fs = std::filesystem;

// Snapshot records start with a tag byte, so the header and the end marker can't be mistaken
// for the caller's records:
//   H "horizon-snapshot 1 " <generation>   first: the log generation that continues after it
//   D <payload>                            one per caller record
//   E <count>                              last: proves the snapshot is complete
const char kHeader = 'H';
const char kData = 'D';
const char kEnd = 'E';
const std::string kHeaderText = "horizon-snapshot 1 ";

// How much of a snapshot is kept in memory before it's written to the temp file.
const std::size_t kSnapshotChunkBytes = 4 * 1024 * 1024;

bool parse_number(const std::string& text, std::uint64_t& out) {
    const char* end = text.data() + text.size();
    std::from_chars_result result = std::from_chars(text.data(), end, out);
    return !text.empty() && result.ec == std::errc() && result.ptr == end;
}

}  // namespace

Engine::Engine() = default;
Engine::~Engine() = default;

std::string Engine::path(const std::string& suffix) const { return dir_ + "/" + name_ + suffix; }

std::string Engine::log_path(std::uint64_t generation) const {
    return path(".log." + std::to_string(generation));
}

bool Engine::fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

void Engine::crash_if(const char* step) const {
    if (crash_point_ == step) std::abort();  // ends the process at once, like a crash
}

bool Engine::open(const std::string& dir, const std::string& name, FlushPolicy policy,
                  std::string& error) {
    dir_ = dir;
    name_ = name;
    policy_ = policy;
    std::error_code ec;

    if (!fs::is_directory(dir_, ec)) return fail(error, "data directory " + dir_ + " not found");

    // A leftover temp file is a snapshot that was being written when the process stopped.
    // It was never renamed into place, so the old snapshot and log are still complete.
    fs::remove(path(".snapshot.tmp"), ec);

    // The snapshot's header names the log generation to replay after it.
    generation_ = 1;
    phase_ = Phase::kLog;
    if (fs::exists(path(".snapshot"), ec)) {
        if (!reader_.open(path(".snapshot"), error)) return false;
        std::string header;
        std::uint64_t generation = 0;
        if (reader_.next(header) != ReadStatus::kRecord || header.size() < 2 ||
            header[0] != kHeader || header.compare(1, kHeaderText.size(), kHeaderText) != 0 ||
            !parse_number(header.substr(1 + kHeaderText.size()), generation)) {
            return fail(error, path(".snapshot") + " has no valid header; refusing to start");
        }
        generation_ = generation;
        snapshot_bytes_ = reader_.file_size();
        phase_ = Phase::kSnapshot;
    }

    // Make the snapshot's rename durable before deleting the logs it replaces.
    if (!sync_directory(dir_)) return fail(error, "could not sync " + dir_);

    // Look at every log file: older generations are covered by the snapshot and can go; a
    // newer one can't happen with the snapshot order in finish_snapshot(), so something is
    // wrong (e.g. the snapshot was deleted by hand) and guessing could lose data.
    const std::string prefix = name_ + ".log.";
    std::vector<fs::path> stale;
    fs::directory_iterator it(dir_, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string file = it->path().filename().string();
        std::uint64_t generation = 0;
        if (file.compare(0, prefix.size(), prefix) != 0 ||
            !parse_number(file.substr(prefix.size()), generation)) {
            continue;  // not one of our log files
        }
        if (generation > generation_) {
            return fail(error, file + " is newer than the snapshot expects (generation " +
                                   std::to_string(generation_) + "); refusing to start");
        }
        if (generation < generation_) stale.push_back(it->path());
    }
    if (ec) return fail(error, "could not list " + dir_ + " (" + ec.message() + ")");
    // Deleted after the listing, since removing entries while listing them is unreliable.
    for (const fs::path& file : stale) fs::remove(file, ec);

    if (phase_ == Phase::kLog) return reader_.open(log_path(generation_), error);
    return true;
}

RecoveryStatus Engine::next(std::string& payload, std::string& error) {
    while (true) {
        if (phase_ == Phase::kDone) return RecoveryStatus::kDone;

        if (phase_ == Phase::kSnapshot) {
            // A snapshot is renamed into place only once complete and on disk, so anything
            // short of a clean end marker means damage, never a crash: refuse to guess.
            if (reader_.next(payload) != ReadStatus::kRecord || payload.empty()) {
                error = path(".snapshot") + " is incomplete or damaged; refusing to start";
                return RecoveryStatus::kError;
            }
            if (payload[0] == kData) {
                payload.erase(0, 1);
                ++snapshot_records_;
                return RecoveryStatus::kRecord;
            }
            std::uint64_t count = 0;
            std::string rest;
            if (payload[0] != kEnd || !parse_number(payload.substr(1), count) ||
                count != snapshot_records_ || reader_.next(rest) != ReadStatus::kEnd) {
                error = path(".snapshot") + " has a bad end marker; refusing to start";
                return RecoveryStatus::kError;
            }
            // Snapshot done: continue with the log of the generation after it.
            reader_ = LogReader();
            if (!reader_.open(log_path(generation_), error)) return RecoveryStatus::kError;
            phase_ = Phase::kLog;
            continue;
        }

        ReadStatus status = reader_.next(payload);
        if (status == ReadStatus::kRecord) return RecoveryStatus::kRecord;
        if (status == ReadStatus::kCorrupt || status == ReadStatus::kError) {
            error = log_path(generation_) + " is damaged at byte " +
                    std::to_string(reader_.valid_bytes()) +
                    "; refusing to start rather than lose the records after it";
            return RecoveryStatus::kError;
        }
        // kEnd or kTornTail: the log is done. A torn tail was a write cut short by a crash.
        valid_log_bytes_ = reader_.valid_bytes();
        dropped_bytes_ = reader_.file_size() - valid_log_bytes_;
        phase_ = Phase::kDone;
    }
}

bool Engine::start(std::string& error) {
    reader_ = LogReader();  // close the file being read, so the log can reopen it to write
    if (!log_.open(log_path(generation_), valid_log_bytes_, policy_, error)) return false;
    // The log file may have just been created; make its name durable.
    return sync_directory(dir_) || fail(error, "could not sync " + dir_);
}

bool Engine::begin_snapshot(std::string& error) {
    // Everything appended so far goes to the log first, so if this snapshot fails the old
    // snapshot plus the log still describe exactly the current state.
    if (!log_.commit(error)) return false;

    // kEverySecond: the chunks written while records are added aren't each flushed to disk;
    // finish_snapshot()'s sync() flushes everything once at the end.
    snapshot_ = std::make_unique<Log>();
    if (!snapshot_->open(path(".snapshot.tmp"), 0, FlushPolicy::kEverySecond, error)) {
        abandon_snapshot();
        return false;
    }
    const std::string header = kHeader + kHeaderText + std::to_string(generation_ + 1);
    snapshot_->append(header);
    snapshot_count_ = 0;
    return true;
}

bool Engine::add_to_snapshot(std::span<const char> payload, std::string& error) {
    std::string record(1, kData);
    record.append(payload.data(), payload.size());
    if (!snapshot_ || !snapshot_->append(record)) {
        error = "a snapshot record is over the size limit";
        return false;
    }
    ++snapshot_count_;
    // Hand the records to the OS every few megabytes rather than keeping them all in memory:
    // otherwise a 2 GB dataset would briefly need another 2 GB for its snapshot.
    if (snapshot_->pending_bytes() >= kSnapshotChunkBytes && !snapshot_->commit(error)) {
        error = "could not write " + path(".snapshot.tmp") + ": " + error;
        return false;
    }
    if (crash_point_ == "snapshot-partial" && snapshot_count_ == 1) {
        std::string ignored;
        snapshot_->commit(ignored);  // get the first records into the temp file, then die
        crash_if("snapshot-partial");
    }
    return true;
}

SnapshotResult Engine::finish_snapshot(std::string& error) {
    // 1. Finish the temp file and wait until it's on disk.
    if (!snapshot_ || !snapshot_->append(kEnd + std::to_string(snapshot_count_)) ||
        !snapshot_->sync(error)) {
        // The temp file is written with a Log, whose message says "log"; say which file it was.
        error = "could not write " + path(".snapshot.tmp") + (error.empty() ? "" : ": " + error);
        abandon_snapshot();
        return SnapshotResult::kFailed;
    }
    const std::uint64_t new_snapshot_bytes = snapshot_->size();
    snapshot_.reset();  // close it: Windows can't rename a file that's still open
    crash_if("snapshot-written");

    // 2. Swap it in. Until this succeeds, the old snapshot and log are still in charge.
    if (!replace_file(path(".snapshot.tmp"), path(".snapshot"))) {
        error = "could not replace the snapshot (" + last_file_error() + ")";
        abandon_snapshot();
        return SnapshotResult::kFailed;
    }
    // From here on the new snapshot is in charge, whatever happens: startup will ignore the
    // old log, so new writes must go to the new one.
    sync_directory(dir_);  // makes the rename itself survive a power cut
    snapshot_bytes_ = new_snapshot_bytes;
    crash_if("snapshot-renamed");

    // 3. Start the next log generation. The old log is now redundant.
    const std::uint64_t old_generation = generation_;
    ++generation_;
    if (!log_.open(log_path(generation_), 0, policy_, error) || !sync_directory(dir_)) {
        if (error.empty()) error = "could not sync " + dir_;
        return SnapshotResult::kFatal;
    }
    crash_if("new-log-created");

    // 4. Delete the old log. If this fails, the next open() deletes it instead.
    std::error_code ec;
    fs::remove(log_path(old_generation), ec);
    return SnapshotResult::kSaved;
}

void Engine::abandon_snapshot() {
    snapshot_.reset();
    std::error_code ec;
    fs::remove(path(".snapshot.tmp"), ec);
}

}  // namespace horizon
