#include "platform/socket.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using tachyon::listen_tcp;
using tachyon::poll_sockets;
using tachyon::PollEntry;
using tachyon::Socket;
using tachyon::SocketHandle;
using tachyon::SocketLibrary;

namespace {

// Real sockets on localhost. A port only one listener can hold shows whether a socket is
// still open: binding it again fails until the first one is closed.
const char* kIp = "127.0.0.1";
const unsigned short kTestPort = 6391;

}  // namespace

TEST(Socket, DefaultIsEmpty) {
    Socket socket;
    EXPECT_FALSE(socket.valid());
}

TEST(Socket, DestructorClosesTheSocket) {
    SocketLibrary library;
    std::string error;
    {
        Socket first = listen_tcp(kIp, kTestPort, error);
        ASSERT_TRUE(first.valid()) << error;
        Socket second = listen_tcp(kIp, kTestPort, error);
        EXPECT_FALSE(second.valid());  // the port is still held by `first`
    }  // `first` is destroyed here
    Socket again = listen_tcp(kIp, kTestPort, error);
    EXPECT_TRUE(again.valid()) << error;
}

TEST(Socket, MoveTransfersOwnership) {
    SocketLibrary library;
    std::string error;
    Socket a = listen_tcp(kIp, kTestPort, error);
    ASSERT_TRUE(a.valid()) << error;
    SocketHandle handle = a.get();

    Socket b = std::move(a);  // move constructor
    EXPECT_FALSE(a.valid());
    EXPECT_EQ(b.get(), handle);

    Socket c;
    c = std::move(b);  // move assignment
    EXPECT_FALSE(b.valid());
    EXPECT_EQ(c.get(), handle);
}

TEST(Socket, ResetClosesEarly) {
    SocketLibrary library;
    std::string error;
    Socket socket = listen_tcp(kIp, kTestPort, error);
    ASSERT_TRUE(socket.valid()) << error;
    socket.reset();
    EXPECT_FALSE(socket.valid());
    EXPECT_TRUE(listen_tcp(kIp, kTestPort, error).valid()) << error;
}

TEST(PollSockets, IdleListenerTimesOut) {
    SocketLibrary library;
    std::string error;
    Socket listener = listen_tcp(kIp, kTestPort, error);
    ASSERT_TRUE(listener.valid()) << error;

    std::vector<PollEntry> entries(1);
    entries[0].socket = listener.get();
    entries[0].want_read = true;
    EXPECT_EQ(poll_sockets(entries, 10), 0);  // no client is connecting
    EXPECT_FALSE(entries[0].readable);
    EXPECT_FALSE(entries[0].failed);
}
