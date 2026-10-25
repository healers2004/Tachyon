#include <gtest/gtest.h>

#include <string>

#include "command_test_util.h"

using tachyon::Store;
using test::run;

namespace {

const std::string kWrongType =
    "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

}  // namespace

TEST(Hash, SetCountsNewFieldsOnly) {
    Store store;
    EXPECT_EQ(run(store, {"HSET", "h", "a", "1", "b", "2"}), ":2\r\n");
    EXPECT_EQ(run(store, {"HSET", "h", "c", "3", "a", "9"}), ":1\r\n");  // a is updated
    EXPECT_EQ(run(store, {"HGET", "h", "a"}), "$1\r\n9\r\n");
    EXPECT_EQ(run(store, {"HLEN", "h"}), ":3\r\n");
}

TEST(Hash, SetNeedsPairs) {
    Store store;
    EXPECT_EQ(run(store, {"HSET", "h", "a", "1", "c"}),
              "-ERR wrong number of arguments for 'hset' command\r\n");
    EXPECT_EQ(run(store, {"HSET", "h", "a"}),
              "-ERR wrong number of arguments for 'hset' command\r\n");
}

TEST(Hash, MissingKeyOrField) {
    Store store;
    EXPECT_EQ(run(store, {"HGET", "h", "a"}), "$-1\r\n");
    EXPECT_EQ(run(store, {"HLEN", "h"}), ":0\r\n");
    EXPECT_EQ(run(store, {"HGETALL", "h"}), "*0\r\n");
    EXPECT_EQ(run(store, {"HDEL", "h", "a"}), ":0\r\n");
    run(store, {"HSET", "h", "a", "1"});
    EXPECT_EQ(run(store, {"HGET", "h", "zz"}), "$-1\r\n");
}

TEST(Hash, GetAllReturnsFieldValuePairs) {
    Store store;
    run(store, {"HSET", "h", "only", "one"});
    EXPECT_EQ(run(store, {"HGETALL", "h"}), "*2\r\n$4\r\nonly\r\n$3\r\none\r\n");
}

TEST(Hash, DeletingTheLastFieldDeletesTheKey) {
    Store store;
    run(store, {"HSET", "h", "a", "1", "b", "2"});
    EXPECT_EQ(run(store, {"HDEL", "h", "a", "missing"}), ":1\r\n");
    EXPECT_EQ(run(store, {"EXISTS", "h"}), ":1\r\n");
    EXPECT_EQ(run(store, {"HDEL", "h", "b"}), ":1\r\n");
    EXPECT_EQ(run(store, {"EXISTS", "h"}), ":0\r\n");
    EXPECT_EQ(run(store, {"TYPE", "h"}), "+none\r\n");
}

TEST(Hash, WrongType) {
    Store store;
    run(store, {"SET", "s", "v"});
    EXPECT_EQ(run(store, {"HSET", "s", "a", "1"}), kWrongType);
    EXPECT_EQ(run(store, {"HGET", "s", "a"}), kWrongType);
    EXPECT_EQ(run(store, {"HGETALL", "s"}), kWrongType);
    EXPECT_EQ(run(store, {"HDEL", "s", "a"}), kWrongType);
    EXPECT_EQ(run(store, {"HLEN", "s"}), kWrongType);
    run(store, {"HSET", "h", "a", "1"});
    EXPECT_EQ(run(store, {"GET", "h"}), kWrongType);
    EXPECT_EQ(run(store, {"SET", "h", "now a string"}), "+OK\r\n");  // SET replaces any type
}
