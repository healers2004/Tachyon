// POSIX (Linux) implementation of platform/socket.h.
#include "platform/socket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace tachyon {

namespace {

int to_native(SocketHandle socket) { return static_cast<int>(socket); }

// POSIX calls report errors through errno; strerror turns it into text.
std::string last_error(const std::string& call) {
    return call + " failed (" + std::strerror(errno) + ")";
}

}  // namespace

// Nothing to start on POSIX; sockets are always available.
SocketLibrary::SocketLibrary() { ok_ = true; }

SocketLibrary::~SocketLibrary() {}

SocketHandle listen_tcp(const std::string& ip, unsigned short port, std::string& error) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        error = last_error("socket()");
        return kInvalidSocket;
    }

    // Lets tachyon restart straight away instead of failing with "address in use" while the
    // old connection's TIME_WAIT lingers.
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};  // {} zero-fills every field, like memset(&address, 0, ...)
    address.sin_family = AF_INET;
    address.sin_port = htons(port);  // network byte order (big-endian)
    // inet_pton turns "127.0.0.1" into binary; returns 1 on success.
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        error = "invalid IP address '" + ip + "'";
        close(s);
        return kInvalidSocket;
    }

    // reinterpret_cast: treat the sockaddr_in as the generic sockaddr the API expects.
    if (bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        error =
            last_error("bind()") + "; is another program using port " + std::to_string(port) + "?";
        close(s);
        return kInvalidSocket;
    }
    if (listen(s, SOMAXCONN) < 0) {
        error = last_error("listen()");
        close(s);
        return kInvalidSocket;
    }
    return s;
}

int wait_readable(SocketHandle socket, int timeout_ms) {
    // shortcut: select() watches one socket at a time and only fds below 1024; Phase 2
    // replaces it with poll().
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(to_native(socket), &readable);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    int ready = select(to_native(socket) + 1, &readable, nullptr, nullptr, &timeout);
    if (ready < 0) {
        // EINTR: a signal (e.g. Ctrl+C) interrupted the wait. Report a timeout so the caller
        // checks its stop flag.
        return errno == EINTR ? 0 : -1;
    }
    return ready > 0 ? 1 : 0;
}

SocketHandle accept_client(SocketHandle listener) {
    int client = accept(to_native(listener), nullptr, nullptr);
    return client < 0 ? kInvalidSocket : client;
}

long long receive(SocketHandle socket, char* buffer, std::size_t size) {
    ssize_t n;
    do {
        n = recv(to_native(socket), buffer, size, 0);
    } while (n < 0 && errno == EINTR);  // retry if a signal interrupted the call
    return n;
}

bool send_all(SocketHandle socket, std::string_view data) {
    while (!data.empty()) {
        // MSG_NOSIGNAL: if the client has gone, return an error instead of raising SIGPIPE,
        // which would kill tachyon.
        ssize_t sent = send(to_native(socket), data.data(), data.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));  // drop what was sent
    }
    return true;
}

void close_socket(SocketHandle socket) { close(to_native(socket)); }

}  // namespace tachyon
