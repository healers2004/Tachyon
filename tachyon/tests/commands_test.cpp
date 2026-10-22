#include "commands/commands.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using tachyon::execute_command;
using tachyon::Store;

namespace {

// Runs one command against `store`; the braces build the argument vector.
std::string run(Store& store, const std::vector<std::string>& args) {
    return execute_command(store, args);
}

}  // namespace

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

TEST(Commands, SetRejectsOptionsForNow) {
    Store store;
    EXPECT_EQ(run(store, {"SET", "k", "v", "EX", "10"}), "-ERR syntax error\r\n");
    EXPECT_EQ(run(store, {"SET", "k"}), "-ERR wrong number of arguments for 'set' command\r\n");
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
    EXPECT_EQ(run(store, {"FLY", "away"}), "-ERR unknown command 'FLY'\r\n");
}

// The command name is echoed in the error; a \r\n in it must not start a second reply.
TEST(Commands, UnknownCommandCannotInjectReply) {
    Store store;
    EXPECT_EQ(run(store, {"X\r\n+OK"}), "-ERR unknown command 'X  +OK'\r\n");
}

TEST(Commands, EmptyArgsIsAnError) {
    Store store;
    EXPECT_EQ(run(store, {}), "-ERR empty command\r\n");
}
