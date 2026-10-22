#include "protocol/resp_parser.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using tachyon::ParseResult;
using tachyon::ParseStatus;
using tachyon::RespParser;

namespace {

const std::string kSetFooBar = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
const std::vector<std::string> kSetFooBarArgs = {"SET", "foo", "bar"};

// Feeds `input` in one piece and returns the first result.
ParseResult parse_once(const std::string& input) {
    RespParser parser;
    parser.feed(input);
    return parser.next();
}

}  // namespace

TEST(RespParser, ParsesCompleteCommand) {
    ParseResult result = parse_once(kSetFooBar);
    EXPECT_EQ(result.status, ParseStatus::kCommand);
    EXPECT_EQ(result.args, kSetFooBarArgs);
}

TEST(RespParser, EmptyInputNeedsMore) { EXPECT_EQ(parse_once("").status, ParseStatus::kNeedMore); }

// The core partial-read case: TCP splits the command at any byte.
TEST(RespParser, CommandSplitAtEveryBytePosition) {
    for (std::size_t split = 1; split < kSetFooBar.size(); ++split) {
        RespParser parser;
        parser.feed(kSetFooBar.substr(0, split));
        // << adds context to the failure message.
        EXPECT_EQ(parser.next().status, ParseStatus::kNeedMore) << "split at " << split;

        parser.feed(kSetFooBar.substr(split));
        ParseResult result = parser.next();
        EXPECT_EQ(result.status, ParseStatus::kCommand) << "split at " << split;
        EXPECT_EQ(result.args, kSetFooBarArgs) << "split at " << split;
    }
}

TEST(RespParser, CommandFedOneByteAtATime) {
    RespParser parser;
    for (std::size_t i = 0; i + 1 < kSetFooBar.size(); ++i) {
        parser.feed(kSetFooBar.substr(i, 1));
        EXPECT_EQ(parser.next().status, ParseStatus::kNeedMore) << "after byte " << i;
    }
    parser.feed(kSetFooBar.substr(kSetFooBar.size() - 1));
    EXPECT_EQ(parser.next().args, kSetFooBarArgs);
}

TEST(RespParser, TwoCommandsInOneFeed) {
    RespParser parser;
    parser.feed(kSetFooBar + "*1\r\n$4\r\nPING\r\n");
    EXPECT_EQ(parser.next().args, kSetFooBarArgs);
    EXPECT_EQ(parser.next().args, std::vector<std::string>{"PING"});
    EXPECT_EQ(parser.next().status, ParseStatus::kNeedMore);
}

TEST(RespParser, BulkStringMayContainCrlf) {
    ParseResult result = parse_once("*2\r\n$4\r\nECHO\r\n$4\r\na\r\nb\r\n");
    EXPECT_EQ(result.args, (std::vector<std::string>{"ECHO", "a\r\nb"}));
}

TEST(RespParser, EmptyBulkStringIsValid) {
    ParseResult result = parse_once("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$0\r\n\r\n");
    EXPECT_EQ(result.args, (std::vector<std::string>{"SET", "k", ""}));
}

TEST(RespParser, LargeArgumentUnderLimitIsAccepted) {
    std::string value(1000000, 'a');
    ParseResult result = parse_once("*1\r\n$1000000\r\n" + value + "\r\n");
    ASSERT_EQ(result.status, ParseStatus::kCommand);
    EXPECT_EQ(result.args[0], value);
}

// Cross-protocol attack: a web page POSTing to localhost:6380 must not run commands.
TEST(RespParser, RejectsHttpRequests) {
    EXPECT_EQ(parse_once("POST / HTTP/1.1\r\nHost: localhost\r\n\r\n").status, ParseStatus::kError);
    EXPECT_EQ(parse_once("GET / HTTP/1.1\r\n\r\n").status, ParseStatus::kError);
    EXPECT_EQ(parse_once("P").status, ParseStatus::kError);  // fails on the first byte
}

TEST(RespParser, RejectsMalformedInput) {
    const std::vector<std::string> inputs = {
        "*x\r\n",                     // count isn't a number
        "*-1\r\n",                    // null array
        "*0\r\n",                     // empty array
        "*1025\r\n",                  // more than kMaxArgs
        "*1\r\n:3\r\n",               // integer instead of bulk string
        "*1\r\n$\r\n",                // missing length
        "*1\r\n$abc\r\n",             // length isn't a number
        "*1\r\n$-1\r\n",              // null bulk string
        "*1\r\n$2000000\r\n",         // length over kMaxCommandBytes
        "*1\r\n$3\r\nfooXY",          // string not followed by \r\n
        "*" + std::string(100, '1'),  // length line that never ends
    };
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        EXPECT_EQ(parse_once(inputs[i]).status, ParseStatus::kError) << "input #" << i;
    }
}

TEST(RespParser, RejectsUnfinishedCommandOverLimit) {
    // Each length is allowed, but together they pass kMaxCommandBytes before finishing.
    std::string input = "*2\r\n$1000000\r\n" + std::string(1000000, 'a') + "\r\n$100000\r\n" +
                        std::string(60000, 'b');
    EXPECT_EQ(parse_once(input).status, ParseStatus::kError);
}
