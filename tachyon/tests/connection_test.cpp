#include "server/connection.h"

#include <gtest/gtest.h>

#include <string>

using tachyon::Connection;
using tachyon::handle_input;
using tachyon::Socket;
using tachyon::Store;

namespace {

const std::string kPing = "*1\r\n$4\r\nPING\r\n";

}  // namespace

// The event loop's per-client logic, without a network: bytes in, queued reply bytes out.
// Braces in `connection{Socket()}`: with () the line would declare a function instead.

TEST(Connection, RepliesToEveryPipelinedCommand) {
    Store store;
    Connection connection{Socket()};
    handle_input(connection, store, kPing + "*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n");
    EXPECT_EQ(connection.pending_reply, "+PONG\r\n$2\r\nhi\r\n");
    EXPECT_FALSE(connection.closing);
}

TEST(Connection, HalfACommandWaitsForTheRest) {
    Store store;
    Connection connection{Socket()};
    handle_input(connection, store, "*1\r\n$4\r\nPI");
    EXPECT_EQ(connection.pending_reply, "");
    handle_input(connection, store, "NG\r\n");
    EXPECT_EQ(connection.pending_reply, "+PONG\r\n");
}

TEST(Connection, ClientsShareOneStore) {
    Store store;
    Connection writer{Socket()};
    Connection reader{Socket()};
    handle_input(writer, store, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
    handle_input(reader, store, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    EXPECT_EQ(reader.pending_reply, "$1\r\nv\r\n");
}

TEST(Connection, ProtocolErrorRepliesThenCloses) {
    Store store;
    Connection connection{Socket()};
    handle_input(connection, store, kPing + "GET / HTTP/1.1\r\n" + kPing);
    // The command before the bad input runs; nothing after it does.
    EXPECT_EQ(connection.pending_reply.rfind("+PONG\r\n-ERR Protocol error", 0), 0u)
        << connection.pending_reply;
    EXPECT_TRUE(connection.closing);
}
