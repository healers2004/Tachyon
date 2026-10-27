#include "horizon/log.h"

#include "horizon/crc32.h"
#include "platform/file.h"

namespace horizon {

namespace {

const std::size_t kHeaderBytes = 8;  // 4-byte length + 4-byte CRC32

// Little-endian: lowest byte first, written with shifts so the file is the same on every CPU,
// whatever byte order it uses in memory.
void put_u32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
    }
}

std::uint32_t get_u32(const char* bytes) {
    // unsigned char: a byte above 127 must not become a negative number when shifted.
    const unsigned char* b = reinterpret_cast<const unsigned char*>(bytes);
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}

}  // namespace

Log::Log() = default;
Log::~Log() = default;  // the unique_ptr closes the file

bool Log::open(const std::string& path, std::uint64_t keep_bytes, FlushPolicy policy,
               std::string& error) {
    File file = open_for_append(path, keep_bytes, error);
    if (!file.valid()) return false;
    // make_unique<File>(std::move(file)): a heap File that takes over the open handle. If a
    // file was already open (the engine reuses a Log for each new log file), it's closed here.
    file_ = std::make_unique<File>(std::move(file));
    policy_ = policy;
    batch_.clear();
    unsynced_ = false;
    size_ = keep_bytes;
    last_sync_ = std::chrono::steady_clock::now();
    return true;
}

bool Log::append(std::span<const char> payload) {
    if (payload.size() > kMaxRecordBytes) return false;
    put_u32(batch_, static_cast<std::uint32_t>(payload.size()));
    put_u32(batch_, crc32(payload));
    batch_.append(payload.data(), payload.size());
    return true;
}

bool Log::write_batch(std::string& error) {
    if (batch_.empty()) return true;
    if (!file_ || !write_all(file_->get(), batch_)) {
        error = "log write failed (" + last_file_error() + ")";
        return false;
    }
    size_ += batch_.size();
    batch_.clear();
    unsynced_ = true;
    return true;
}

bool Log::commit(std::string& error) {
    // Always hand the batch to the OS: then even kEverySecond survives the process being
    // killed, because the OS writes its cache to disk later on its own.
    if (!write_batch(error)) return false;
    if (!unsynced_) return true;
    if (policy_ == FlushPolicy::kEverySecond &&
        std::chrono::steady_clock::now() - last_sync_ < std::chrono::seconds(1)) {
        return true;
    }
    return sync(error);
}

bool Log::sync(std::string& error) {
    if (!write_batch(error)) return false;
    if (unsynced_) {
        if (!sync_file(file_->get())) {
            error = "log flush to disk failed (" + last_file_error() + ")";
            return false;
        }
        unsynced_ = false;
    }
    last_sync_ = std::chrono::steady_clock::now();
    return true;
}

bool LogReader::open(const std::string& path, std::string& error) {
    file_.open(path, std::ios::binary);  // binary: no newline translation on Windows
    if (!file_.is_open()) {
        // shortcut: ifstream can't tell "missing" from "no permission"; both read as an empty
        // log here, and an unreadable file then fails in Log::open instead.
        return true;
    }
    // Seek to the end to learn the size, then back to the start.
    file_.seekg(0, std::ios::end);
    std::streamoff size = file_.tellg();
    file_.seekg(0, std::ios::beg);
    if (size < 0 || !file_) {
        error = "could not read " + path;
        return false;
    }
    file_size_ = static_cast<std::uint64_t>(size);
    return true;
}

ReadStatus LogReader::next(std::string& payload) {
    std::uint64_t remaining = file_size_ - valid_bytes_;
    if (remaining == 0) return ReadStatus::kEnd;
    if (remaining < kHeaderBytes) return ReadStatus::kTornTail;  // the crash cut the header

    char header[kHeaderBytes];
    if (!file_.read(header, sizeof(header))) return ReadStatus::kError;
    std::uint32_t length = get_u32(header);
    std::uint32_t checksum = get_u32(header + 4);

    // Checked before allocating, so a damaged length can't make us allocate gigabytes.
    // shortcut: a damaged length in the middle of the log also lands here and is treated as a
    // torn tail; a checksum over the header would tell the two apart.
    if (length > remaining - kHeaderBytes) return ReadStatus::kTornTail;

    payload.resize(length);
    if (length > 0 && !file_.read(payload.data(), length)) return ReadStatus::kError;

    std::uint64_t record_bytes = kHeaderBytes + length;
    if (crc32(payload) != checksum) {
        // Damage in the very last record is what an interrupted write looks like. Damage with
        // more records after it isn't, and dropping them would lose acknowledged writes.
        return record_bytes == remaining ? ReadStatus::kTornTail : ReadStatus::kCorrupt;
    }
    valid_bytes_ += record_bytes;
    return ReadStatus::kRecord;
}

}  // namespace horizon
