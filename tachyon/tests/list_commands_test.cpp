#include <gtest/gtest.h>

#include <string>

#include "command_test_util.h"

using tachyon::Store;
using test::run;

namespace {

const std::string kWrongType =
    "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

// Builds a list of "a", "b", "c", "d", "e" under key "l".
void make_list(Store& store) { run(store, {"RPUSH", "l", "a", "b", "c", "d", "e"}); }

}  // namespace

TEST(List, PushOrder) {
    Store store;
    EXPECT_EQ(run(store, {"RPUSH", "l", "a", "b", "c"}), ":3\r\n");
    EXPECT_EQ(run(store, {"LPUSH", "l", "x", "y"}), ":5\r\n");  // y x a b c
    EXPECT_EQ(run(store, {"LRANGE", "l", "0", "-1"}),
              "*5\r\n$1\r\ny\r\n$1\r\nx\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n");
    EXPECT_EQ(run(store, {"LLEN", "l"}), ":5\r\n");
}

TEST(List, RangeIndexes) {
    Store store;
    make_list(store);
    EXPECT_EQ(run(store, {"LRANGE", "l", "-2", "-1"}), "*2\r\n$1\r\nd\r\n$1\r\ne\r\n");
    EXPECT_EQ(run(store, {"LRANGE", "l", "1", "1"}), "*1\r\n$1\r\nb\r\n");
    EXPECT_EQ(run(store, {"LRANGE", "l", "-100", "0"}), "*1\r\n$1\r\na\r\n");  // clamped
    EXPECT_EQ(run(store, {"LRANGE", "l", "3", "100"}), "*2\r\n$1\r\nd\r\n$1\r\ne\r\n");
    EXPECT_EQ(run(store, {"LRANGE", "l", "5", "10"}), "*0\r\n");
    EXPECT_EQ(run(store, {"LRANGE", "l", "3", "1"}), "*0\r\n");
    EXPECT_EQ(run(store, {"LRANGE", "missing", "0", "-1"}), "*0\r\n");
    EXPECT_EQ(run(store, {"LRANGE", "l", "0", "abc"}),
              "-ERR value is not an integer or out of range\r\n");
}

TEST(List, PopWithoutCount) {
    Store store;
    make_list(store);
    EXPECT_EQ(run(store, {"LPOP", "l"}), "$1\r\na\r\n");
    EXPECT_EQ(run(store, {"RPOP", "l"}), "$1\r\ne\r\n");
    EXPECT_EQ(run(store, {"LPOP", "missing"}), "$-1\r\n");
}

TEST(List, PopWithCount) {
    Store store;
    make_list(store);
    EXPECT_EQ(run(store, {"LPOP", "l", "0"}), "*0\r\n");
    EXPECT_EQ(run(store, {"LPOP", "l", "2"}), "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
    EXPECT_EQ(run(store, {"RPOP", "l", "10"}), "*3\r\n$1\r\ne\r\n$1\r\nd\r\n$1\r\nc\r\n");
    EXPECT_EQ(run(store, {"EXISTS", "l"}), ":0\r\n");  // emptied lists are deleted
    EXPECT_EQ(run(store, {"LPOP", "missing", "2"}), "*-1\r\n");
    EXPECT_EQ(run(store, {"LPOP", "missing", "-1"}),
              "-ERR value is out of range, must be positive\r\n");
    EXPECT_EQ(run(store, {"LPOP", "l", "1", "2"}),
              "-ERR wrong number of arguments for 'lpop' command\r\n");
}

TEST(List, Trim) {
    Store store;
    make_list(store);
    EXPECT_EQ(run(store, {"LTRIM", "l", "1", "-2"}), "+OK\r\n");
    EXPECT_EQ(run(store, {"LRANGE", "l", "0", "-1"}), "*3\r\n$1\r\nb\r\n$1\r\nc\r\n$1\r\nd\r\n");
    EXPECT_EQ(run(store, {"LTRIM", "l", "5", "1"}), "+OK\r\n");  // empty range
    EXPECT_EQ(run(store, {"EXISTS", "l"}), ":0\r\n");
    EXPECT_EQ(run(store, {"LTRIM", "missing", "0", "1"}), "+OK\r\n");
}

TEST(List, WrongType) {
    Store store;
    run(store, {"SET", "s", "v"});
    EXPECT_EQ(run(store, {"LPUSH", "s", "a"}), kWrongType);
    EXPECT_EQ(run(store, {"RPOP", "s"}), kWrongType);
    EXPECT_EQ(run(store, {"LRANGE", "s", "0", "1"}), kWrongType);
    EXPECT_EQ(run(store, {"LTRIM", "s", "0", "1"}), kWrongType);
    EXPECT_EQ(run(store, {"LLEN", "s"}), kWrongType);
    EXPECT_EQ(run(store, {"GET", "s"}), "$1\r\nv\r\n");  // untouched
}
