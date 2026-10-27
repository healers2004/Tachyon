// Snapshots at the tachyon level: the store written as commands, and loaded back.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "commands/command_log.h"
#include "commands/commands.h"
#include "protocol/resp_parser.h"
#include "protocol/resp_writer.h"
#include "server/connection.h"
#include "server/context.h"
#include "server/server_commands.h"

using horizon::FlushPolicy;
using horizon::SnapshotResult;
using tachyon::CommandLog;
using tachyon::Store;

namespace {

const std::string kDir = "snapshot_test_dir";

void start_fresh() {
    std::error_code ec;
    std::filesystem::remove_all(kDir, ec);
    std::filesystem::create_directory(kDir, ec);
}

std::string run(Store& store, const std::vector<std::string>& args) {
    return tachyon::execute_command(store, args, tachyon::current_time_ms());
}

// Runs commands the way the server does: record, run, commit.
void run_logged(CommandLog& log, Store& store, const std::vector<std::vector<std::string>>& cmds) {
    std::string error;
    for (const std::vector<std::string>& command : cmds) {
        const std::int64_t now = tachyon::current_time_ms();
        std::vector<std::string> args = command;
        tachyon::make_times_absolute(args, now);
        ASSERT_TRUE(log.record(args));
        tachyon::execute_command(store, args, now);
    }
    ASSERT_TRUE(log.commit(error)) << error;
}

void save(CommandLog& log, const Store& store) {
    std::string error;
    ASSERT_EQ(log.save(store, tachyon::current_time_ms(), error), SnapshotResult::kSaved) << error;
}

}  // namespace

TEST(Snapshot, RestartLoadsSnapshotThenNewerLog) {
    start_fresh();
    std::string error;
    {
        Store store;
        CommandLog log;
        ASSERT_TRUE(log.open(kDir, FlushPolicy::kEveryWrite, store, error)) << error;
        run_logged(log, store,
                   {{"SET", "s", "v"},
                    {"HSET", "h", "f", "1", "g", "2"},
                    {"RPUSH", "l", "a", "b", "c"},
                    {"SET", "ttl", "v", "EX", "1000"}});
        save(log, store);
        EXPECT_EQ(log.log_size(), 0u);  // the log starts again empty
        // After the snapshot: these must be replayed on top of it, exactly once.
        run_logged(log, store, {{"RPUSH", "l", "d"}, {"HDEL", "h", "g"}, {"DEL", "s"}});
    }
    Store store;
    CommandLog log;
    ASSERT_TRUE(log.open(kDir, FlushPolicy::kEveryWrite, store, error)) << error;
    EXPECT_EQ(log.replayed_commands(), 3u);
    EXPECT_EQ(run(store, {"LRANGE", "l", "0", "-1"}),
              "*4\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n$1\r\nd\r\n");  // d once, not twice
    EXPECT_EQ(run(store, {"HGETALL", "h"}), "*2\r\n$1\r\nf\r\n$1\r\n1\r\n");
    EXPECT_EQ(run(store, {"EXISTS", "s"}), ":0\r\n");
    const std::string ttl = run(store, {"TTL", "ttl"});
    EXPECT_TRUE(ttl == ":1000\r\n" || ttl == ":999\r\n") << ttl;
}

TEST(Snapshot, ExpiredKeysAreLeftOut) {
    start_fresh();
    std::string error;
    Store store;
    store.set("old", std::string("v"));
    store.set_expiry("old", tachyon::current_time_ms() - 1);
    store.set("new", std::string("v"));
    CommandLog log;
    ASSERT_TRUE(log.open(kDir, FlushPolicy::kEveryWrite, store, error)) << error;
    save(log, store);

    Store loaded;
    CommandLog reopened;
    ASSERT_TRUE(reopened.open(kDir, FlushPolicy::kEveryWrite, loaded, error)) << error;
    EXPECT_EQ(reopened.snapshot_commands(), 1u);  // just SET new
    EXPECT_EQ(loaded.size(), 1u);
}

// A big list or hash is written as several commands, each within the parser's limits, so
// replay can read it back.
TEST(Snapshot, BigValuesAreSplitIntoChunks) {
    tachyon::List list;
    for (int i = 0; i < 2500; ++i) list.push_back("element-" + std::to_string(i));
    std::vector<std::vector<std::string>> commands = tachyon::rebuild_commands("l", list, 12345);
    ASSERT_EQ(commands.size(), 6u);  // 5 RPUSH chunks of 500, then PEXPIREAT
    for (std::size_t i = 0; i + 1 < commands.size(); ++i) {
        EXPECT_EQ(commands[i][0], "RPUSH");
        EXPECT_LE(commands[i].size(), tachyon::kMaxArgs);
        tachyon::RespParser parser;  // the replay path must accept every chunk
        parser.feed(tachyon::encode_command(commands[i]));
        EXPECT_EQ(parser.next().status, tachyon::ParseStatus::kCommand);
    }
    EXPECT_EQ(commands.back(), (std::vector<std::string>{"PEXPIREAT", "l", "12345"}));

    tachyon::Hash hash;
    for (int i = 0; i < 1200; ++i) hash["field-" + std::to_string(i)] = std::string(600, 'x');
    commands = tachyon::rebuild_commands("h", hash, tachyon::kNoExpiry);
    std::size_t pairs = 0;
    for (const std::vector<std::string>& command : commands) {
        EXPECT_EQ(command[0], "HSET");
        EXPECT_EQ(command.size() % 2, 0u);  // name, key, then whole field/value pairs
        EXPECT_LE(tachyon::encode_command(command).size(), tachyon::kMaxCommandBytes);
        pairs += (command.size() - 2) / 2;
    }
    EXPECT_EQ(pairs, 1200u);
}

TEST(Snapshot, BigValuesSurviveARestart) {
    start_fresh();
    std::string error;
    {
        Store store;
        CommandLog log;
        ASSERT_TRUE(log.open(kDir, FlushPolicy::kEveryWrite, store, error)) << error;
        std::vector<std::string> push = {"RPUSH", "l"};
        for (int i = 0; i < 1000; ++i) push.push_back(std::to_string(i));
        run_logged(log, store, {push, push, push});  // 3000 elements
        save(log, store);
    }
    Store store;
    CommandLog log;
    ASSERT_TRUE(log.open(kDir, FlushPolicy::kEveryWrite, store, error)) << error;
    EXPECT_EQ(run(store, {"LLEN", "l"}), ":3000\r\n");
    EXPECT_EQ(run(store, {"LRANGE", "l", "1000", "1001"}), "*2\r\n$1\r\n0\r\n$1\r\n1\r\n");
}

// SAVE through the real command path, and what it reports.
TEST(Snapshot, SaveCommand) {
    start_fresh();
    std::string error;
    tachyon::ServerContext context;
    CommandLog log;
    ASSERT_TRUE(log.open(kDir, FlushPolicy::kEveryWrite, context.store, error)) << error;
    context.log = &log;
    tachyon::Connection client{tachyon::Socket()};
    tachyon::handle_input(client, context,
                          tachyon::encode_command({"SET", "k", "v"}) +
                              tachyon::encode_command({"SAVE"}) +
                              tachyon::encode_command({"SAVE", "extra"}));
    EXPECT_EQ(client.pending_reply,
              "+OK\r\n+OK\r\n-ERR wrong number of arguments for 'save' command\r\n");
    EXPECT_EQ(context.stats.saves, 1u);
    EXPECT_EQ(context.stats.changes_since_save, 0u);
    EXPECT_EQ(log.generation(), 2u);
    // A successful snapshot sets when the next automatic one is due: here the threshold, since
    // the snapshot is smaller than it. (This also resets any back-off from an earlier failure.)
    EXPECT_EQ(context.next_snapshot_at, context.options.snapshot_after_bytes);
    EXPECT_GT(log.snapshot_bytes(), 0u);
}

TEST(Snapshot, SaveWithoutPersistence) {
    tachyon::ServerContext context;  // context.log stays nullptr, as with --no-persist
    tachyon::Connection client{tachyon::Socket()};
    tachyon::handle_input(client, context, tachyon::encode_command({"SAVE"}));
    EXPECT_EQ(client.pending_reply, "-ERR persistence is disabled (--no-persist)\r\n");
}

// A big dataset shouldn't be rewritten after every few writes: the next automatic snapshot
// waits until the log is at least as big as the snapshot just written.
TEST(Snapshot, NextSnapshotWaitsForTheBiggerOfThresholdAndSnapshot) {
    start_fresh();
    std::string error;
    tachyon::ServerContext context;
    context.options.snapshot_after_bytes = 1000;  // far smaller than the data below
    CommandLog log;
    ASSERT_TRUE(log.open(kDir, FlushPolicy::kEveryWrite, context.store, error)) << error;
    context.log = &log;
    for (int i = 0; i < 100; ++i)
        context.store.set("key" + std::to_string(i), std::string(100, 'x'));
    ASSERT_EQ(tachyon::save_snapshot(context), "");
    EXPECT_GT(log.snapshot_bytes(), 10000u);
    EXPECT_EQ(context.next_snapshot_at, log.snapshot_bytes());
}

// Before 1.0 the log was one file, tachyon.wal, in the same record format. Upgrading must load
// it, not silently start empty.
TEST(Snapshot, PreReleaseWalIsCarriedForward) {
    start_fresh();
    std::string error;
    {
        horizon::Log old_log;
        ASSERT_TRUE(old_log.open(kDir + "/tachyon.wal", 0, FlushPolicy::kEveryWrite, error));
        old_log.append(tachyon::encode_command({"SET", "old", "data"}));
        ASSERT_TRUE(old_log.commit(error)) << error;
    }
    Store store;
    CommandLog log;
    ASSERT_TRUE(log.open(kDir, FlushPolicy::kEveryWrite, store, error)) << error;
    EXPECT_TRUE(log.migrated_legacy_log());
    EXPECT_EQ(run(store, {"GET", "old"}), "$4\r\ndata\r\n");
    EXPECT_FALSE(std::filesystem::exists(kDir + "/tachyon.wal"));
    EXPECT_TRUE(std::filesystem::exists(kDir + "/tachyon.log.1"));
}
