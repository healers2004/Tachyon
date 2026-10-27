#include "horizon/log.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using horizon::FlushPolicy;
using horizon::Log;
using horizon::LogReader;
using horizon::ReadStatus;

namespace {

// ctest runs each test from the build folder. Every test calls start_fresh() first.
const std::string kPath = "horizon_test.log";

void start_fresh() { std::remove(kPath.c_str()); }  // std::remove deletes a file

// Appends `records` and commits them, as a program would before acknowledging.
void write_records(const std::vector<std::string>& records, std::uint64_t keep_bytes = 0) {
    Log log;
    std::string error;
    ASSERT_TRUE(log.open(kPath, keep_bytes, FlushPolicy::kEveryWrite, error)) << error;
    for (std::size_t i = 0; i < records.size(); ++i) {
        ASSERT_TRUE(log.append(records[i]));
    }
    ASSERT_TRUE(log.commit(error)) << error;
}

struct ReadResult {
    std::vector<std::string> records;
    ReadStatus last = ReadStatus::kError;  // the status that ended the read
    std::uint64_t valid_bytes = 0;
};

ReadResult read_records() {
    ReadResult result;
    LogReader reader;
    std::string error;
    EXPECT_TRUE(reader.open(kPath, error)) << error;
    std::string payload;
    while ((result.last = reader.next(payload)) == ReadStatus::kRecord) {
        result.records.push_back(payload);
    }
    result.valid_bytes = reader.valid_bytes();
    return result;
}

std::string read_file() {
    std::ifstream in(kPath, std::ios::binary);
    in.seekg(0, std::ios::end);
    std::string bytes(static_cast<std::size_t>(in.tellg()), '\0');
    in.seekg(0, std::ios::beg);
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

void write_file(const std::string& bytes) {
    std::ofstream out(kPath, std::ios::binary | std::ios::trunc);
    out << bytes;
}

}  // namespace

TEST(Log, MissingFileIsAnEmptyLog) {
    start_fresh();
    ReadResult result = read_records();
    EXPECT_EQ(result.last, ReadStatus::kEnd);
    EXPECT_TRUE(result.records.empty());
}

TEST(Log, EmptyFileIsAnEmptyLog) {
    start_fresh();
    write_file("");
    EXPECT_EQ(read_records().last, ReadStatus::kEnd);
}

TEST(Log, RecordsReadBackInOrder) {
    start_fresh();
    // Includes an empty record and bytes that look like line endings and NULs.
    std::vector<std::string> records = {"first", "", std::string("a\r\n\0b", 5), "last"};
    write_records(records);
    ReadResult result = read_records();
    EXPECT_EQ(result.last, ReadStatus::kEnd);
    EXPECT_EQ(result.records, records);
}

TEST(Log, FileFormatIsLengthChecksumPayload) {
    start_fresh();
    write_records({"123456789"});
    // Little-endian length 9, then CRC32 0xCBF43926 lowest byte first, then the payload.
    EXPECT_EQ(read_file(), std::string("\x09\x00\x00\x00\x26\x39\xF4\xCB", 8) + "123456789");
}

TEST(Log, ReopeningAppendsAfterExistingRecords) {
    start_fresh();
    write_records({"one"});
    write_records({"two"}, read_records().valid_bytes);
    EXPECT_EQ(read_records().records, (std::vector<std::string>{"one", "two"}));
}

TEST(Log, NothingReachesTheFileBeforeCommit) {
    start_fresh();
    Log log;
    std::string error;
    ASSERT_TRUE(log.open(kPath, 0, FlushPolicy::kEveryWrite, error)) << error;
    log.append(std::string("pending"));
    EXPECT_EQ(read_file(), "");
    ASSERT_TRUE(log.commit(error)) << error;
    EXPECT_EQ(read_records().records, std::vector<std::string>{"pending"});
}

// kEverySecond delays only the disk flush; the bytes still reach the OS on every commit, so a
// killed process loses nothing it acknowledged.
TEST(Log, EverySecondPolicyStillWritesOnCommit) {
    start_fresh();
    Log log;
    std::string error;
    ASSERT_TRUE(log.open(kPath, 0, FlushPolicy::kEverySecond, error)) << error;
    log.append(std::string("fast"));
    ASSERT_TRUE(log.commit(error)) << error;
    EXPECT_EQ(read_records().records, std::vector<std::string>{"fast"});
}

// A crash can stop a write after any byte. Every cut inside the last record must keep the
// earlier records, report a torn tail, and give the length to cut back to.
TEST(Log, TornTailAtEveryBytePosition) {
    start_fresh();
    write_records({"kept"});
    const std::uint64_t kept_bytes = read_file().size();
    write_records({"half-written"}, kept_bytes);
    const std::string full = read_file();

    for (std::size_t cut = kept_bytes + 1; cut < full.size(); ++cut) {
        write_file(full.substr(0, cut));
        ReadResult result = read_records();
        EXPECT_EQ(result.last, ReadStatus::kTornTail) << "cut at " << cut;
        EXPECT_EQ(result.records, std::vector<std::string>{"kept"}) << "cut at " << cut;
        EXPECT_EQ(result.valid_bytes, kept_bytes) << "cut at " << cut;
    }
}

TEST(Log, CuttingTheTornTailLetsWritesContinue) {
    start_fresh();
    write_records({"kept", "half-written"});
    std::string bytes = read_file();
    write_file(bytes.substr(0, bytes.size() - 3));

    ReadResult torn = read_records();
    ASSERT_EQ(torn.last, ReadStatus::kTornTail);
    write_records({"after crash"}, torn.valid_bytes);

    ReadResult result = read_records();
    EXPECT_EQ(result.last, ReadStatus::kEnd);
    EXPECT_EQ(result.records, (std::vector<std::string>{"kept", "after crash"}));
}

TEST(Log, DamagedLastRecordIsATornTail) {
    start_fresh();
    write_records({"good", "damaged"});
    std::string bytes = read_file();
    bytes.back() ^= 0x01;  // flip one bit in the last payload byte
    write_file(bytes);
    ReadResult result = read_records();
    EXPECT_EQ(result.last, ReadStatus::kTornTail);
    EXPECT_EQ(result.records, std::vector<std::string>{"good"});
}

// Damage with valid records after it can't be a half-finished write. Reporting it, rather than
// dropping everything after it, keeps acknowledged writes from vanishing silently.
TEST(Log, DamagedMiddleRecordIsCorrupt) {
    start_fresh();
    write_records({"good", "damaged", "after"});
    std::string bytes = read_file();
    bytes[8 + 4 + 8] ^= 0x01;  // first payload byte of the second record
    write_file(bytes);
    ReadResult result = read_records();
    EXPECT_EQ(result.last, ReadStatus::kCorrupt);
    EXPECT_EQ(result.records, std::vector<std::string>{"good"});
}

TEST(Log, OversizedRecordIsRejected) {
    start_fresh();
    Log log;
    std::string error;
    ASSERT_TRUE(log.open(kPath, 0, FlushPolicy::kEveryWrite, error)) << error;
    std::string huge(horizon::kMaxRecordBytes + 1, 'x');
    EXPECT_FALSE(log.append(huge));
}

// --fsync everysec: a commit hands the bytes to the OS but leaves them unflushed until a second
// has passed since the last flush; then the next commit flushes them.
TEST(Log, EverySecondPolicyFlushesOnceASecond) {
    start_fresh();
    Log log;
    std::string error;
    ASSERT_TRUE(log.open(kPath, 0, FlushPolicy::kEverySecond, error)) << error;
    log.append(std::string("one"));
    ASSERT_TRUE(log.commit(error)) << error;
    EXPECT_TRUE(log.unsynced());  // written, not yet flushed

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    ASSERT_TRUE(log.commit(error)) << error;  // nothing new, but a second has passed
    EXPECT_FALSE(log.unsynced());

    Log always;
    ASSERT_TRUE(always.open(kPath, 0, FlushPolicy::kEveryWrite, error)) << error;
    always.append(std::string("two"));
    ASSERT_TRUE(always.commit(error)) << error;
    EXPECT_FALSE(always.unsynced());  // kEveryWrite flushes on every commit
}
