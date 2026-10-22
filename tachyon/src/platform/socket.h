// Thin, portable wrapper over TCP sockets. The same declarations are implemented twice:
// platform/win32/socket.cpp (Winsock) and platform/posix/socket.cpp (Linux). CMake compiles
// only the one for the current OS, so no #ifdef is needed here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tachyon {

// One type for both OSes: Windows SOCKET is a pointer-sized integer, POSIX uses an int.
// `using X = Y;` is C++'s typedef.
using SocketHandle = std::intptr_t;
const SocketHandle kInvalidSocket = -1;

// Starts the socket library for as long as the object lives. On Windows the constructor calls
// WSAStartup and the destructor calls WSACleanup; on Linux both do nothing.
// Create exactly one, in main, and don't copy it.
class SocketLibrary {
public:
    SocketLibrary();
    ~SocketLibrary();
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

// Creates a TCP socket listening on ip:port (e.g. "127.0.0.1", 6380).
// Returns kInvalidSocket and sets `error` on failure, e.g. when the port is already in use.
SocketHandle listen_tcp(const std::string& ip, unsigned short port, std::string& error);

// Waits until `socket` has data (or, for a listening socket, a waiting client).
// Returns 1 if ready, 0 on timeout or interruption by a signal, -1 on error.
int wait_readable(SocketHandle socket, int timeout_ms);

// Takes the next waiting client off a listening socket. kInvalidSocket on failure.
SocketHandle accept_client(SocketHandle listener);

// Reads up to `size` bytes into `buffer`. Returns the byte count, 0 if the peer closed the
// connection, or -1 on error.
long long receive(SocketHandle socket, char* buffer, std::size_t size);

// Sends all of `data`, looping because one send call may send only part. false on error.
bool send_all(SocketHandle socket, std::string_view data);

void close_socket(SocketHandle socket);

}  // namespace tachyon
