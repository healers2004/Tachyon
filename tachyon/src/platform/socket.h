// Thin, portable wrapper over TCP sockets. The same declarations are implemented twice:
// platform/win32/socket.cpp (Winsock) and platform/posix/socket.cpp (Linux). CMake compiles
// only the one for the current OS, so no #ifdef is needed here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon {

// One type for both OSes: Windows SOCKET is a pointer-sized integer, POSIX uses an int.
// `using X = Y;` is C++'s typedef.
using SocketHandle = std::intptr_t;
const SocketHandle kInvalidSocket = -1;

// Returned by receive() and send_some() when a non-blocking socket isn't ready yet.
const long long kWouldBlock = -2;

// Starts the socket library for as long as the object lives. On Windows the constructor calls
// WSAStartup and the destructor calls WSACleanup; on Linux both do nothing.
// Create exactly one, in main.
class SocketLibrary {
public:
    SocketLibrary();
    ~SocketLibrary();
    // `= delete`: copying would call WSACleanup twice, so the compiler rejects any copy.
    SocketLibrary(const SocketLibrary&) = delete;
    SocketLibrary& operator=(const SocketLibrary&) = delete;
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

// Closes a raw handle. Use Socket instead, which calls this for you exactly once.
void close_socket(SocketHandle socket);

// Owns one OS socket and closes it in its destructor (RAII), so no path can leak it.
// Move-only: a copy would close the same socket twice, but ownership can be handed over.
class Socket {
public:
    Socket() = default;  // empty: owns nothing
    explicit Socket(SocketHandle handle) : handle_(handle) {}
    ~Socket() { reset(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Move: `Socket&&` binds to a temporary or to std::move(x). Take the handle and leave
    // `other` empty, so only one object ever closes it. noexcept: moves can't fail, which
    // lets std::vector move (not copy) elements when it grows.
    Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalidSocket; }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {  // `s = std::move(s)` must not close s
            reset();
            handle_ = other.handle_;
            other.handle_ = kInvalidSocket;
        }
        return *this;
    }

    bool valid() const { return handle_ != kInvalidSocket; }
    SocketHandle get() const { return handle_; }  // for OS calls; the Socket keeps ownership

    // Closes the socket now (if any) and leaves this object empty.
    void reset() {
        if (valid()) {
            close_socket(handle_);
            handle_ = kInvalidSocket;
        }
    }

private:
    SocketHandle handle_ = kInvalidSocket;
};

// Creates a non-blocking TCP socket listening on ip:port (e.g. "127.0.0.1", 6380).
// Returns an empty Socket and sets `error` on failure, e.g. when the port is already in use.
Socket listen_tcp(const std::string& ip, unsigned short port, std::string& error);

// Takes the next waiting client off a listening socket. An empty Socket when none is waiting.
Socket accept_client(SocketHandle listener);

// Makes calls on the socket return kWouldBlock instead of waiting. false on failure.
bool set_non_blocking(SocketHandle socket);

// One socket to watch in poll_sockets(): fill in the first three fields, read the rest.
struct PollEntry {
    SocketHandle socket = kInvalidSocket;
    bool want_read = false;
    bool want_write = false;

    bool readable = false;  // data (or a waiting client, or end of stream) to read
    bool writable = false;  // room in the OS send buffer
    bool failed = false;    // error or hang-up; a read reports which
};

// Waits until at least one entry is ready or timeout_ms passes, then fills in the results.
// Returns the number of ready entries, 0 on timeout or a signal, -1 on error.
int poll_sockets(std::vector<PollEntry>& entries, int timeout_ms);

// Reads up to `size` bytes into `buffer`. Returns the byte count, 0 if the peer closed the
// connection, kWouldBlock if nothing has arrived, or -1 on error.
long long receive(SocketHandle socket, char* buffer, std::size_t size);

// Sends as much of `data` as the OS accepts right now. Returns the byte count (possibly less
// than data.size()), kWouldBlock if the send buffer is full, or -1 on error.
long long send_some(SocketHandle socket, std::string_view data);

}  // namespace tachyon
