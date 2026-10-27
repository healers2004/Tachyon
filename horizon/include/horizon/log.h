// Horizon's write-ahead log: an append-only file of records. Horizon doesn't know what a
// record means; the program using it stores whatever bytes it needs to replay its state.
//
// File format, one record after another, integers little-endian:
//   [length: 4 bytes][CRC32 of payload: 4 bytes][payload: `length` bytes]
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <string>

namespace horizon {

// Declared, not defined: the OS-specific file class stays private to Horizon's .cpp files.
// unique_ptr can hold a pointer to it here because only log.cpp creates or deletes one.
class File;

// The biggest payload one record may hold.
const std::uint32_t kMaxRecordBytes = 64 * 1024 * 1024;

// When commit() forces records onto the disk.
enum class FlushPolicy {
    kEveryWrite,   // before every acknowledgement: nothing acknowledged is ever lost
    kEverySecond,  // at most once a second: faster, but a power cut can lose ~1 s of writes
};

// Appends records. Typical use: append() each change, then commit() before telling anyone
// the changes are saved. One commit can cover many appends (group commit).
class Log {
public:
    Log();
    ~Log();  // defined in log.cpp, where File is complete, so unique_ptr can delete it

    // Opens the log at `path`, creating it if missing. The file is first cut to `keep_bytes`,
    // which removes a half-written last record found by LogReader. false + `error` on failure.
    bool open(const std::string& path, std::uint64_t keep_bytes, FlushPolicy policy,
              std::string& error);

    // Adds one record to an in-memory batch; nothing reaches the file until commit().
    // false if the payload is over kMaxRecordBytes.
    bool append(std::span<const char> payload);

    // Writes the batch to the file and, if the policy says it's time, waits until it's on the
    // disk. Call it before acknowledging appended changes, and regularly (it also runs the
    // once-a-second flush). After a false return the file can't be trusted: stop writing.
    bool commit(std::string& error);

    // Like commit(), but always waits for the disk. Use it before a clean shutdown.
    bool sync(std::string& error);

    // Bytes in the file, including the ones written but not yet on the disk.
    std::uint64_t size() const { return size_; }
    // Bytes appended but not yet written to the file (they wait for commit()).
    std::size_t pending_bytes() const { return batch_.size(); }
    // True if bytes have been written to the OS but not yet flushed to the disk.
    bool unsynced() const { return unsynced_; }

private:
    bool write_batch(std::string& error);

    std::unique_ptr<File> file_;
    FlushPolicy policy_ = FlushPolicy::kEveryWrite;
    std::string batch_;       // encoded records waiting for commit()
    std::uint64_t size_ = 0;  // bytes written to the file so far
    bool unsynced_ = false;   // written to the OS but maybe not yet on the disk
    // steady_clock: a clock that only moves forward, unaffected by changes to the system time.
    std::chrono::steady_clock::time_point last_sync_;
};

enum class ReadStatus {
    kRecord,    // a valid record was read into `payload`
    kEnd,       // the clean end of the log
    kTornTail,  // the last record is incomplete or damaged: a write interrupted by a crash
    kCorrupt,   // a damaged record with more data after it: not a crash, so don't guess
    kError,     // the file couldn't be read
};

// Reads a log from the start, one record at a time.
class LogReader {
public:
    // A missing file reads as an empty log. false + `error` if it exists but can't be read.
    bool open(const std::string& path, std::string& error);

    ReadStatus next(std::string& payload);

    // Bytes of valid records read so far: after kTornTail, pass this to Log::open to cut the
    // half-written record off.
    std::uint64_t valid_bytes() const { return valid_bytes_; }
    std::uint64_t file_size() const { return file_size_; }

private:
    std::ifstream file_;
    std::uint64_t file_size_ = 0;
    std::uint64_t valid_bytes_ = 0;
};

}  // namespace horizon
