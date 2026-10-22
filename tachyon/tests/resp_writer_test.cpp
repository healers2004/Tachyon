#include "protocol/resp_writer.h"

#include <gtest/gtest.h>

#include <string>

TEST(RespWriter, FormatsEachReplyType) {
    EXPECT_EQ(tachyon::simple_string_reply("OK"), "+OK\r\n");
    EXPECT_EQ(tachyon::error_reply("ERR boom"), "-ERR boom\r\n");
    EXPECT_EQ(tachyon::integer_reply(-3), ":-3\r\n");
    EXPECT_EQ(tachyon::bulk_reply("bar"), "$3\r\nbar\r\n");
    EXPECT_EQ(tachyon::bulk_reply(""), "$0\r\n\r\n");
    EXPECT_EQ(tachyon::null_reply(), "$-1\r\n");
}

TEST(RespWriter, BulkReplyKeepsCrlfInside) {
    EXPECT_EQ(tachyon::bulk_reply("a\r\nb"), "$4\r\na\r\nb\r\n");
}

// An error that quotes user input must not let that input inject a second reply.
TEST(RespWriter, SingleLineRepliesStripCrlf) {
    EXPECT_EQ(tachyon::error_reply("ERR bad 'x\r\n+OK'"), "-ERR bad 'x  +OK'\r\n");
    EXPECT_EQ(tachyon::simple_string_reply("a\nb"), "+a b\r\n");
}

// The log stores commands in the same form clients send them.
TEST(RespWriter, EncodeCommandMatchesClientFormat) {
    EXPECT_EQ(tachyon::encode_command({"SET", "foo", "a\r\nb"}),
              "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$4\r\na\r\nb\r\n");
}
