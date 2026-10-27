#include "commands/command_log.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "commands/commands.h"
#include "server/connection.h"
#include "server/pubsub.h"

using horizon::FlushPolicy;
using tachyon::CommandLog;
using tachyon::Connection;
using tachyon::handle_input;
using tachyon::Socket;
using tachyon::Store;

namespace {

// ctest runs each test from the build folder. Every test calls start_fresh() first.
const std::string kPath = "command_log_test.wal";

void start_fresh() { std::remove(kPath.c_str()); }

// Runs commands the way the server does: record, run, then commit before "replying".
void run_logged(CommandLog& log, Store& store, const std::vector<std::vector<std::string>>& cmds) {
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        const std::int64_t now = tachyon::current_time_ms();
        const std::vector<std::string> args = tachyon::with_absolute_times(cmds[i], now);
        ASSERT_TRUE(log.record(args));
        tachyon::execute_command(store, args, now);
    }
    std::string error;
    ASSERT_TRUE(log.commit(error)) << error;
}

bool has(Store& store, const std::string& key) {
    return store.find(key, tachyon::current_time_ms()) != nullptr;
}

std::uint64_t file_size() {
    std::ifstream in(kPath, std::ios::binary | std::ios::ate);  // ate: start at the end
    return in ? static_cast<std::uint64_t>(in.tellg()) : 0;
}

}  // namespace

// The core promise: a restarted server sees every committed write.
TEST(CommandLog, RestartReplaysCommittedWrites) {
    start_fresh();
    std::string error;
    {
        Store store;
        CommandLog log;
        ASSERT_TRUE(log.open(kPath, FlushPolicy::kEveryWrite, store, error)) << error;
        run_logged(log, store, {{"SET", "a", "1"}, {"SET", "b", "2"}, {"del", "a"}});
    }  // "crash": the objects go away without any shutdown step

    Store store;
    CommandLog log;
    ASSERT_TRUE(log.open(kPath, FlushPolicy::kEveryWrite, store, error)) << error;
    EXPECT_EQ(log.replayed_commands(), 3u);
    EXPECT_FALSE(has(store, "a"));
    EXPECT_EQ(tachyon::execute_command(store, {"GET", "b"}, tachyon::current_time_ms()),
              "$1\r\n2\r\n");
}

TEST(CommandLog, ReadsAreNotLogged) {
    start_fresh();
    std::string error;
    Store store;
    CommandLog log;
    ASSERT_TRUE(log.open(kPath, FlushPolicy::kEveryWrite, store, error)) << error;
    run_logged(log, store, {{"GET", "a"}, {"PING"}, {"EXISTS", "a"}});
    EXPECT_EQ(file_size(), 0u);
}

TEST(CommandLog, TornTailIsDroppedAndLoggingContinues) {
    start_fresh();
    std::string error;
    {
        Store store;
        CommandLog log;
        ASSERT_TRUE(log.open(kPath, FlushPolicy::kEveryWrite, store, error)) << error;
        run_logged(log, store, {{"SET", "kept", "1"}});
    }
    // Simulate a crash in the middle of writing a record: a few bytes of a header.
    {
        std::ofstream out(kPath, std::ios::binary | std::ios::app);
        out << std::string("\x20\x00\x00", 3);
    }
    {
        Store store;
        CommandLog log;
        ASSERT_TRUE(log.open(kPath, FlushPolicy::kEveryWrite, store, error)) << error;
        EXPECT_EQ(log.dropped_bytes(), 3u);
        EXPECT_TRUE(has(store, "kept"));
        run_logged(log, store, {{"SET", "after", "2"}});
    }
    Store store;
    CommandLog log;
    ASSERT_TRUE(log.open(kPath, FlushPolicy::kEveryWrite, store, error)) << error;
    EXPECT_EQ(log.dropped_bytes(), 0u);
    EXPECT_TRUE(has(store, "kept"));
    EXPECT_TRUE(has(store, "after"));
}

TEST(CommandLog, DamagedLogRefusesToStart) {
    start_fresh();
    std::string error;
    {
        Store store;
        CommandLog log;
        ASSERT_TRUE(log.open(kPath, FlushPolicy::kEveryWrite, store, error)) << error;
        run_logged(log, store, {{"SET", "a", "1"}, {"SET", "b", "2"}});
    }
    // Flip a payload byte of the first record; the second record is still intact after it.
    std::fstream file(kPath, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(10);
    file.put('X');
    file.close();

    Store store;
    CommandLog log;
    EXPECT_FALSE(log.open(kPath, FlushPolicy::kEveryWrite, store, error));
    EXPECT_NE(error.find("damaged"), std::string::npos) << error;
}

// The server's path: records go in during handle_input, and nothing is on disk until commit.
TEST(CommandLog, HandleInputRecordsWritesForGroupCommit) {
    start_fresh();
    std::string error;
    Store store;
    CommandLog log;
    ASSERT_TRUE(log.open(kPath, FlushPolicy::kEveryWrite, store, error)) << error;
    tachyon::PubSub pubsub;
    Connection connection{Socket()};
    handle_input(connection, store, &log, pubsub,
                 "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    EXPECT_EQ(connection.pending_reply, "+OK\r\n$1\r\nv\r\n");
    EXPECT_EQ(file_size(), 0u);  // queued, not yet written
    ASSERT_TRUE(log.commit(error)) << error;
    EXPECT_GT(file_size(), 0u);
}

// Replay rebuilds every type, and an expiry keeps its original deadline instead of restarting.
TEST(CommandLog, ReplayRebuildsEveryTypeAndExpiry) {
    start_fresh();
    std::string error;
    std::int64_t deadline = 0;
    {
        Store store;
        CommandLog log;
        ASSERT_TRUE(log.open(kPath, FlushPolicy::kEveryWrite, store, error)) << error;
        run_logged(log, store,
                   {{"HSET", "h", "f", "v", "g", "w"},
                    {"HDEL", "h", "g"},
                    {"RPUSH", "l", "a", "b", "c"},
                    {"LPOP", "l"},
                    {"SET", "s", "v", "EX", "1000"},
                    {"SET", "gone", "x"},
                    {"EXPIRE", "gone", "-1"}});
        deadline = store.expiry("s");
    }
    Store store;
    CommandLog log;
    ASSERT_TRUE(log.open(kPath, FlushPolicy::kEveryWrite, store, error)) << error;
    const std::int64_t now = tachyon::current_time_ms();
    EXPECT_EQ(tachyon::execute_command(store, {"HGETALL", "h"}, now),
              "*2\r\n$1\r\nf\r\n$1\r\nv\r\n");
    EXPECT_EQ(tachyon::execute_command(store, {"LRANGE", "l", "0", "-1"}, now),
              "*2\r\n$1\r\nb\r\n$1\r\nc\r\n");
    EXPECT_EQ(store.expiry("s"), deadline);  // the same millisecond, not "1000 s from replay"
    EXPECT_EQ(store.find("gone", now), nullptr);
}
