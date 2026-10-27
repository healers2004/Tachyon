#include "horizon/engine.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using horizon::Engine;
using horizon::FlushPolicy;
using horizon::RecoveryStatus;
using horizon::SnapshotResult;

namespace {

namespace fs = std::filesystem;

// Each test starts with an empty directory in the build folder.
const std::string kDir = "engine_test_dir";
const std::string kName = "db";

void start_fresh() {
    std::error_code ec;
    fs::remove_all(kDir, ec);
    fs::create_directory(kDir, ec);
}

std::string file(const std::string& suffix) { return kDir + "/" + kName + suffix; }

bool exists(const std::string& suffix) { return fs::exists(file(suffix)); }

// Opens the directory and returns every recovered record, as a program would at startup.
std::vector<std::string> recover(Engine& engine) {
    std::string error;
    EXPECT_TRUE(engine.open(kDir, kName, FlushPolicy::kEveryWrite, error)) << error;
    std::vector<std::string> records;
    std::string payload;
    RecoveryStatus status;
    while ((status = engine.next(payload, error)) == RecoveryStatus::kRecord) {
        records.push_back(payload);
    }
    EXPECT_EQ(status, RecoveryStatus::kDone) << error;
    EXPECT_TRUE(engine.start(error)) << error;
    return records;
}

std::vector<std::string> recover() {
    Engine engine;
    return recover(engine);
}

void write(Engine& engine, const std::vector<std::string>& records) {
    std::string error;
    for (const std::string& record : records) ASSERT_TRUE(engine.append(record));
    ASSERT_TRUE(engine.commit(error)) << error;
}

void snapshot(Engine& engine, const std::vector<std::string>& records) {
    std::string error;
    ASSERT_TRUE(engine.begin_snapshot(error)) << error;
    for (const std::string& record : records) {
        ASSERT_TRUE(engine.add_to_snapshot(record, error)) << error;
    }
    ASSERT_EQ(engine.finish_snapshot(error), SnapshotResult::kSaved) << error;
}

std::string read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string bytes;
    char c;
    while (in.get(c)) bytes.push_back(c);
    return bytes;
}

void write_bytes(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

bool open_fails(std::string& error) {
    Engine engine;
    if (!engine.open(kDir, kName, FlushPolicy::kEveryWrite, error)) return true;
    std::string payload;
    RecoveryStatus status;
    while ((status = engine.next(payload, error)) == RecoveryStatus::kRecord) {
    }
    return status == RecoveryStatus::kError;
}

}  // namespace

TEST(Engine, EmptyDirectoryStartsEmpty) {
    start_fresh();
    EXPECT_TRUE(recover().empty());
    EXPECT_TRUE(exists(".log.1"));
}

TEST(Engine, MissingDirectoryIsAnError) {
    Engine engine;
    std::string error;
    EXPECT_FALSE(engine.open("no_such_dir_here", kName, FlushPolicy::kEveryWrite, error));
    EXPECT_NE(error.find("not found"), std::string::npos) << error;
}

TEST(Engine, LogSurvivesReopen) {
    start_fresh();
    {
        Engine engine;
        recover(engine);
        write(engine, {"a", "b"});
    }
    EXPECT_EQ(recover(), (std::vector<std::string>{"a", "b"}));
}

// The normal path: after a snapshot, startup reads the snapshot and only the newer log.
TEST(Engine, SnapshotReplacesTheOldLog) {
    start_fresh();
    {
        Engine engine;
        recover(engine);
        write(engine, {"a", "b"});
        snapshot(engine, {"state-1", "state-2"});
        EXPECT_EQ(engine.generation(), 2u);
        EXPECT_EQ(engine.log_size(), 0u);
        write(engine, {"c"});
    }
    EXPECT_FALSE(exists(".log.1"));
    EXPECT_FALSE(exists(".snapshot.tmp"));
    Engine engine;
    EXPECT_EQ(recover(engine), (std::vector<std::string>{"state-1", "state-2", "c"}));
    EXPECT_EQ(engine.snapshot_records(), 2u);
    // The size of the snapshot in charge, read back from the file after a restart.
    EXPECT_EQ(engine.snapshot_bytes(), fs::file_size(file(".snapshot")));
    EXPECT_GT(engine.snapshot_bytes(), 0u);
}

TEST(Engine, RepeatedSnapshots) {
    start_fresh();
    {
        Engine engine;
        recover(engine);
        write(engine, {"a"});
        snapshot(engine, {"s1"});
        write(engine, {"b"});
        snapshot(engine, {"s2"});
        write(engine, {"c"});
        snapshot(engine, {});  // an empty state is a valid snapshot too
        write(engine, {"d"});
    }
    Engine engine;
    EXPECT_EQ(recover(engine), (std::vector<std::string>{"d"}));
    EXPECT_EQ(engine.generation(), 4u);
}

// Crash while the temp file was being written: the old snapshot and log are still complete.
TEST(Engine, CrashWhileWritingTheSnapshot) {
    start_fresh();
    {
        Engine engine;
        recover(engine);
        write(engine, {"a", "b"});
    }
    write_bytes(file(".snapshot.tmp"), "half a snapshot");
    EXPECT_EQ(recover(), (std::vector<std::string>{"a", "b"}));
    EXPECT_FALSE(exists(".snapshot.tmp"));
}

// Crash after the rename but before the new log existed: the old log must be ignored, or its
// changes would be applied twice on top of a snapshot that already contains them.
TEST(Engine, CrashAfterRenameBeforeNewLog) {
    start_fresh();
    std::string old_log;
    {
        Engine engine;
        recover(engine);
        write(engine, {"push x"});
        old_log = read_bytes(file(".log.1"));
        snapshot(engine, {"state with x"});
    }
    // Put the files back as a crash right after the rename would have left them.
    write_bytes(file(".log.1"), old_log);
    fs::remove(file(".log.2"));

    EXPECT_EQ(recover(), std::vector<std::string>{"state with x"});
    EXPECT_FALSE(exists(".log.1"));  // the stale log is cleaned up
}

// Crash after the new log was created but before the old one was deleted.
TEST(Engine, CrashBeforeOldLogDeleted) {
    start_fresh();
    std::string old_log;
    {
        Engine engine;
        recover(engine);
        write(engine, {"push x"});
        old_log = read_bytes(file(".log.1"));
        snapshot(engine, {"state with x"});
        write(engine, {"push y"});
    }
    write_bytes(file(".log.1"), old_log);
    EXPECT_EQ(recover(), (std::vector<std::string>{"state with x", "push y"}));
    EXPECT_FALSE(exists(".log.1"));
}

TEST(Engine, TornTailAfterSnapshotIsDropped) {
    start_fresh();
    {
        Engine engine;
        recover(engine);
        snapshot(engine, {"s"});
        write(engine, {"kept"});
    }
    std::ofstream(file(".log.2"), std::ios::binary | std::ios::app) << std::string("\x05\x00", 2);
    Engine engine;
    EXPECT_EQ(recover(engine), (std::vector<std::string>{"s", "kept"}));
    EXPECT_EQ(engine.dropped_bytes(), 2u);
    write(engine, {"after"});
    EXPECT_EQ(recover(), (std::vector<std::string>{"s", "kept", "after"}));
}

// A snapshot is only ever renamed into place complete, so damage means refusing to start.
TEST(Engine, DamagedSnapshotRefusesToStart) {
    start_fresh();
    {
        Engine engine;
        recover(engine);
        snapshot(engine, {"one", "two"});
    }
    std::string bytes = read_bytes(file(".snapshot"));
    std::string error;

    write_bytes(file(".snapshot"), bytes.substr(0, bytes.size() - 3));  // end marker cut off
    EXPECT_TRUE(open_fails(error));
    EXPECT_NE(error.find("snapshot"), std::string::npos) << error;

    bytes[bytes.size() / 2] ^= 0x01;  // one flipped bit in the middle
    write_bytes(file(".snapshot"), bytes);
    EXPECT_TRUE(open_fails(error));
}

TEST(Engine, LogNewerThanSnapshotRefusesToStart) {
    start_fresh();
    write_bytes(file(".log.3"), "");  // no snapshot, yet a third-generation log
    std::string error;
    EXPECT_TRUE(open_fails(error));
    EXPECT_NE(error.find("newer"), std::string::npos) << error;
}

// If the snapshot can't even start (here the temp path is taken by a folder, which can't be
// opened as a file), nothing changes and writing carries on.
TEST(Engine, FailedSnapshotKeepsEverything) {
    start_fresh();
    {
        Engine engine;
        recover(engine);
        write(engine, {"a"});
        fs::create_directory(file(".snapshot.tmp"));
        fs::create_directory(file(".snapshot.tmp") + "/keep");  // so it can't be removed
        std::string error;
        EXPECT_FALSE(engine.begin_snapshot(error));
        write(engine, {"b"});
    }
    fs::remove_all(file(".snapshot.tmp"));
    EXPECT_EQ(recover(), (std::vector<std::string>{"a", "b"}));
}

// A big snapshot goes to the temp file in chunks as records are added, so it's never held in
// memory all at once.
TEST(Engine, BigSnapshotIsWrittenAsItGoes) {
    start_fresh();
    Engine engine;
    recover(engine);
    std::string error;
    ASSERT_TRUE(engine.begin_snapshot(error)) << error;
    const std::string record(64 * 1024, 'x');
    for (int i = 0; i < 100; ++i) ASSERT_TRUE(engine.add_to_snapshot(record, error)) << error;
    // 6.4 MB added: at least one 4 MB chunk must already be in the file.
    EXPECT_GE(fs::file_size(file(".snapshot.tmp")), 4u * 1024 * 1024);
    ASSERT_EQ(engine.finish_snapshot(error), SnapshotResult::kSaved) << error;
    Engine reopened;
    EXPECT_EQ(recover(reopened).size(), 100u);
}

// The one dangerous step: the new snapshot is in place, so startup will ignore the old log,
// but the new log can't be created (here a folder is in the way). The server must stop rather
// than keep writing to a log that will be ignored.
TEST(Engine, NewLogFailureIsFatal) {
    start_fresh();
    Engine engine;
    recover(engine);
    write(engine, {"a"});
    fs::create_directory(file(".log.2"));
    std::string error;
    ASSERT_TRUE(engine.begin_snapshot(error)) << error;
    ASSERT_TRUE(engine.add_to_snapshot(std::string("state"), error)) << error;
    EXPECT_EQ(engine.finish_snapshot(error), SnapshotResult::kFatal);
    EXPECT_EQ(engine.generation(), 2u);  // the new snapshot is the one in charge
    EXPECT_TRUE(exists(".snapshot"));
}
