#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "command_test_util.h"

using tachyon::Store;
using test::run;

TEST(Commands, Ping) {
    Store store;
    EXPECT_EQ(run(store, {"PING"}), "+PONG\r\n");
    EXPECT_EQ(run(store, {"PING", "hi"}), "$2\r\nhi\r\n");
    EXPECT_EQ(run(store, {"PING", "a", "b"}),
              "-ERR wrong number of arguments for 'ping' command\r\n");
}

TEST(Commands, Echo) {
    Store store;
    EXPECT_EQ(run(store, {"ECHO", "hello"}), "$5\r\nhello\r\n");
    EXPECT_EQ(run(store, {"ECHO"}), "-ERR wrong number of arguments for 'echo' command\r\n");
}

TEST(Commands, SetGetRoundTrip) {
    Store store;
    EXPECT_EQ(run(store, {"SET", "foo", "bar"}), "+OK\r\n");
    EXPECT_EQ(run(store, {"GET", "foo"}), "$3\r\nbar\r\n");
}

TEST(Commands, GetMissingKeyIsNil) {
    Store store;
    EXPECT_EQ(run(store, {"GET", "nope"}), "$-1\r\n");
}

TEST(Commands, DelCountsRemovedKeys) {
    Store store;
    run(store, {"SET", "a", "1"});
    run(store, {"SET", "b", "2"});
    EXPECT_EQ(run(store, {"DEL", "a", "b", "missing"}), ":2\r\n");
    EXPECT_EQ(run(store, {"DEL", "a"}), ":0\r\n");
}

TEST(Commands, ExistsCountsRepeatedKeysTwice) {
    Store store;
    run(store, {"SET", "a", "1"});
    EXPECT_EQ(run(store, {"EXISTS", "a", "a", "missing"}), ":2\r\n");
}

TEST(Commands, NamesAreCaseInsensitive) {
    Store store;
    EXPECT_EQ(run(store, {"set", "foo", "bar"}), "+OK\r\n");
    EXPECT_EQ(run(store, {"GeT", "foo"}), "$3\r\nbar\r\n");
}

TEST(Commands, UnknownCommand) {
    Store store;
    // Redis 7's exact wording, trailing space included.
    EXPECT_EQ(run(store, {"FLY", "away", "now"}),
              "-ERR unknown command 'FLY', with args beginning with: 'away' 'now' \r\n");
    EXPECT_EQ(run(store, {"FLY"}), "-ERR unknown command 'FLY', with args beginning with: \r\n");
}

// The command name is echoed in the error; a \r\n in it must not start a second reply.
TEST(Commands, UnknownCommandCannotInjectReply) {
    Store store;
    EXPECT_EQ(run(store, {"X\r\n+OK"}),
              "-ERR unknown command 'X  +OK', with args beginning with: \r\n");
}

TEST(Commands, EmptyArgsIsAnError) {
    Store store;
    EXPECT_EQ(run(store, {}), "-ERR empty command\r\n");
}

TEST(Commands, TypeDbsizeFlushall) {
    Store store;
    run(store, {"SET", "s", "v"});
    run(store, {"HSET", "h", "f", "v"});
    run(store, {"RPUSH", "l", "a"});
    EXPECT_EQ(run(store, {"TYPE", "s"}), "+string\r\n");
    EXPECT_EQ(run(store, {"TYPE", "h"}), "+hash\r\n");
    EXPECT_EQ(run(store, {"TYPE", "l"}), "+list\r\n");
    EXPECT_EQ(run(store, {"TYPE", "missing"}), "+none\r\n");
    EXPECT_EQ(run(store, {"DBSIZE"}), ":3\r\n");
    EXPECT_EQ(run(store, {"FLUSHALL", "FOO"}), "-ERR syntax error\r\n");
    EXPECT_EQ(run(store, {"FLUSHALL", "async"}), "+OK\r\n");
    EXPECT_EQ(run(store, {"DBSIZE"}), ":0\r\n");
}

// Arity comes from the command table: exact for GET, "at least" for DEL.
TEST(Commands, ArityFromTable) {
    Store store;
    EXPECT_EQ(run(store, {"GET", "a", "b"}),
              "-ERR wrong number of arguments for 'get' command\r\n");
    EXPECT_EQ(run(store, {"del"}), "-ERR wrong number of arguments for 'del' command\r\n");
    EXPECT_EQ(run(store, {"DBSIZE", "x"}),
              "-ERR wrong number of arguments for 'dbsize' command\r\n");
}

TEST(Commands, OnlyWritesAreLogged) {
    EXPECT_TRUE(tachyon::is_write_command({"set", "k", "v"}));
    EXPECT_TRUE(tachyon::is_write_command({"LPOP", "l"}));
    EXPECT_TRUE(tachyon::is_write_command({"EXPIRE", "k", "1"}));
    EXPECT_FALSE(tachyon::is_write_command({"GET", "k"}));
    EXPECT_FALSE(tachyon::is_write_command({"TTL", "k"}));
    EXPECT_FALSE(tachyon::is_write_command({"NOPE"}));
}
