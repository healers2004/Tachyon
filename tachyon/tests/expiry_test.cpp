// SET options, EXPIRE, PEXPIREAT, TTL, PERSIST and the log rewrite, at made-up times so every
// timing check is exact.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "command_test_util.h"

using tachyon::Store;
using test::kNow;
using test::run;

TEST(Expiry, SetExAndPx) {
    Store store;
    EXPECT_EQ(run(store, {"SET", "k", "v", "EX", "10"}), "+OK\r\n");
    EXPECT_EQ(run(store, {"TTL", "k"}), ":10\r\n");
    EXPECT_EQ(run(store, {"GET", "k"}, kNow + 9'999), "$1\r\nv\r\n");
    EXPECT_EQ(run(store, {"GET", "k"}, kNow + 10'000), "$-1\r\n");

    EXPECT_EQ(run(store, {"SET", "p", "v", "px", "1500"}), "+OK\r\n");
    EXPECT_EQ(run(store, {"TTL", "p"}), ":2\r\n");  // 1.5 s rounds to 2, like Redis
    EXPECT_EQ(run(store, {"TTL", "p"}, kNow + 1'000), ":1\r\n");
}

TEST(Expiry, SetAbsoluteTimes) {
    Store store;
    run(store, {"SET", "k", "v", "PXAT", std::to_string(kNow + 5'000)});
    EXPECT_EQ(run(store, {"TTL", "k"}), ":5\r\n");
    run(store, {"SET", "k", "v", "EXAT", std::to_string(kNow / 1000 + 7)});
    EXPECT_EQ(run(store, {"TTL", "k"}), ":7\r\n");
    run(store, {"SET", "k", "v", "PXAT", "1"});  // already passed: allowed, then gone
    EXPECT_EQ(run(store, {"GET", "k"}), "$-1\r\n");
}

TEST(Expiry, SetOptionErrors) {
    Store store;
    const std::string invalid = "-ERR invalid expire time in 'set' command\r\n";
    EXPECT_EQ(run(store, {"SET", "k", "v", "EX", "0"}), invalid);
    EXPECT_EQ(run(store, {"SET", "k", "v", "EX", "-5"}), invalid);
    EXPECT_EQ(run(store, {"SET", "k", "v", "EX", "9223372036854775807"}), invalid);
    EXPECT_EQ(run(store, {"SET", "k", "v", "EX", "abc"}),
              "-ERR value is not an integer or out of range\r\n");
    EXPECT_EQ(run(store, {"SET", "k", "v", "EX", "10", "PX", "100"}), "-ERR syntax error\r\n");
    EXPECT_EQ(run(store, {"SET", "k", "v", "NX", "XX"}), "-ERR syntax error\r\n");
    EXPECT_EQ(run(store, {"SET", "k", "v", "EX"}), "-ERR syntax error\r\n");
    EXPECT_EQ(run(store, {"SET", "k", "v", "KEEPTTL", "EX", "10"}), "-ERR syntax error\r\n");
    EXPECT_EQ(run(store, {"SET", "k", "v", "FOO"}), "-ERR syntax error\r\n");
    EXPECT_EQ(run(store, {"GET", "k"}), "$-1\r\n");  // none of those set anything
}

TEST(Expiry, SetNxXx) {
    Store store;
    EXPECT_EQ(run(store, {"SET", "k", "v", "XX"}), "$-1\r\n");
    EXPECT_EQ(run(store, {"SET", "k", "v", "NX"}), "+OK\r\n");
    EXPECT_EQ(run(store, {"SET", "k", "w", "NX"}), "$-1\r\n");
    EXPECT_EQ(run(store, {"SET", "k", "x", "XX"}), "+OK\r\n");
    EXPECT_EQ(run(store, {"GET", "k"}), "$1\r\nx\r\n");
}

TEST(Expiry, SetClearsTtlUnlessKeepTtl) {
    Store store;
    run(store, {"SET", "k", "v", "EX", "100"});
    run(store, {"SET", "k", "w", "KEEPTTL"});
    EXPECT_EQ(run(store, {"TTL", "k"}), ":100\r\n");
    run(store, {"SET", "k", "x"});
    EXPECT_EQ(run(store, {"TTL", "k"}), ":-1\r\n");
}

TEST(Expiry, ExpireTtlPersist) {
    Store store;
    EXPECT_EQ(run(store, {"EXPIRE", "missing", "10"}), ":0\r\n");
    EXPECT_EQ(run(store, {"TTL", "missing"}), ":-2\r\n");
    run(store, {"SET", "k", "v"});
    EXPECT_EQ(run(store, {"TTL", "k"}), ":-1\r\n");
    EXPECT_EQ(run(store, {"PERSIST", "k"}), ":0\r\n");
    EXPECT_EQ(run(store, {"EXPIRE", "k", "100"}), ":1\r\n");
    EXPECT_EQ(run(store, {"TTL", "k"}, kNow + 30'000), ":70\r\n");
    EXPECT_EQ(run(store, {"PERSIST", "k"}), ":1\r\n");
    EXPECT_EQ(run(store, {"TTL", "k"}), ":-1\r\n");
}

TEST(Expiry, ExpireInThePastDeletes) {
    Store store;
    run(store, {"SET", "k", "v"});
    EXPECT_EQ(run(store, {"EXPIRE", "k", "-5"}), ":1\r\n");
    EXPECT_EQ(run(store, {"EXISTS", "k"}), ":0\r\n");
    run(store, {"SET", "k", "v"});
    EXPECT_EQ(run(store, {"PEXPIREAT", "k", "1"}), ":1\r\n");
    EXPECT_EQ(run(store, {"EXISTS", "k"}), ":0\r\n");
}

TEST(Expiry, ExpireErrors) {
    Store store;
    run(store, {"SET", "k", "v"});
    EXPECT_EQ(run(store, {"EXPIRE", "k", "abc"}),
              "-ERR value is not an integer or out of range\r\n");
    EXPECT_EQ(run(store, {"EXPIRE", "k", "9223372036854775807"}),
              "-ERR invalid expire time in 'expire' command\r\n");
    // The number is checked before the key, like Redis.
    EXPECT_EQ(run(store, {"EXPIRE", "missing", "abc"}),
              "-ERR value is not an integer or out of range\r\n");
}

TEST(Expiry, ExpiryWorksOnEveryType) {
    Store store;
    run(store, {"HSET", "h", "f", "v"});
    run(store, {"RPUSH", "l", "a"});
    run(store, {"EXPIRE", "h", "1"});
    run(store, {"EXPIRE", "l", "1"});
    EXPECT_EQ(run(store, {"HGET", "h", "f"}, kNow + 1'000), "$-1\r\n");
    EXPECT_EQ(run(store, {"LLEN", "l"}, kNow + 1'000), ":0\r\n");
}

// The log stores absolute deadlines, so replaying later sets the same deadline.
TEST(Expiry, RelativeTimesAreRewrittenForTheLog) {
    using Args = std::vector<std::string>;
    const std::string deadline = std::to_string(kNow + 10'000);
    EXPECT_EQ(test::absolute({"expire", "k", "10"}, kNow), (Args{"PEXPIREAT", "k", deadline}));
    EXPECT_EQ(test::absolute({"SET", "k", "v", "NX", "ex", "10"}, kNow),
              (Args{"SET", "k", "v", "NX", "PXAT", deadline}));
    EXPECT_EQ(test::absolute({"SET", "k", "v", "PX", "10000"}, kNow),
              (Args{"SET", "k", "v", "PXAT", deadline}));
    // Errors are left alone, so they fail the same way when replayed.
    EXPECT_EQ(test::absolute({"SET", "k", "v", "EX", "0"}, kNow),
              (Args{"SET", "k", "v", "EX", "0"}));
    EXPECT_EQ(test::absolute({"EXPIRE", "k", "abc"}, kNow), (Args{"EXPIRE", "k", "abc"}));
    // "EX" as SET's value isn't an option.
    EXPECT_EQ(test::absolute({"SET", "k", "EX", "5"}, kNow), (Args{"SET", "k", "EX", "5"}));
    EXPECT_EQ(test::absolute({"GET", "k"}, kNow), (Args{"GET", "k"}));
}

// The rewritten command gives the same reply and the same result as the original.
TEST(Expiry, RewrittenCommandBehavesTheSame) {
    Store original;
    Store rewritten;
    const std::vector<std::string> commands[] = {
        {"SET", "a", "1", "EX", "10"}, {"SET", "b", "2"}, {"EXPIRE", "b", "20"}};
    for (const std::vector<std::string>& command : commands) {
        EXPECT_EQ(run(original, command), run(rewritten, test::absolute(command, kNow)));
    }
    EXPECT_EQ(run(original, {"TTL", "a"}), run(rewritten, {"TTL", "a"}));
    EXPECT_EQ(run(original, {"TTL", "b"}), run(rewritten, {"TTL", "b"}));
}
