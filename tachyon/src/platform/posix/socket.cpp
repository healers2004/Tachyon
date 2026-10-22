// POSIX (Linux) implementation of platform/socket.h.
#include "platform/socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
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

// A non-blocking call that has nothing to do yet fails with EAGAIN (EWOULDBLOCK on some systems).
bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }

}  // namespace

// Nothing to start on POSIX; sockets are always available.
SocketLibrary::SocketLibrary() { ok_ = true; }

SocketLibrary::~SocketLibrary() {}

void close_socket(SocketHandle socket) { close(to_native(socket)); }

Socket listen_tcp(const std::string& ip, unsigned short port, std::string& error) {
    // Owned from the start: every early return below closes it through the destructor.
    Socket s(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!s.valid()) {
        error = last_error("socket()");
        return Socket();
    }

    // Lets tachyon restart straight away instead of failing with "address in use" while the
    // old connection's TIME_WAIT lingers.
    int reuse = 1;
    setsockopt(to_native(s.get()), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};  // {} zero-fills every field
    address.sin_family = AF_INET;
    address.sin_port = htons(port);  // network byte order (big-endian)
    // inet_pton turns "127.0.0.1" into binary; returns 1 on success.
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        error = "invalid IP address '" + ip + "'";
        return Socket();
    }

    // reinterpret_cast: treat the sockaddr_in as the generic sockaddr the API expects.
    if (bind(to_native(s.get()), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) <
        0) {
        error =
            last_error("bind()") + "; is another program using port " + std::to_string(port) + "?";
        return Socket();
    }
    if (listen(to_native(s.get()), SOMAXCONN) < 0) {
        error = last_error("listen()");
        return Socket();
    }
    // Non-blocking, so accept() returns at once when the waiting client has already gone.
    if (!set_non_blocking(s.get())) {
        error = last_error("fcntl()");
        return Socket();
    }
    return s;  // moved out to the caller, not copied
}

Socket accept_client(SocketHandle listener) {
    int client = accept(to_native(listener), nullptr, nullptr);
    // Fails with EAGAIN when no client is waiting; the caller just stops accepting.
    return client < 0 ? Socket() : Socket(client);
}

bool set_non_blocking(SocketHandle socket) {
    // fcntl reads the file status flags, then writes them back with O_NONBLOCK added.
    int flags = fcntl(to_native(socket), F_GETFL, 0);
    return flags >= 0 && fcntl(to_native(socket), F_SETFL, flags | O_NONBLOCK) == 0;
}

int poll_sockets(std::vector<PollEntry>& entries, int timeout_ms) {
    // shortcut: rebuilds the pollfd array on every call; fine for hundreds of clients, epoll
    // would scale further (see Future improvements).
    std::vector<pollfd> fds(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        fds[i].fd = to_native(entries[i].socket);
        fds[i].events = static_cast<short>((entries[i].want_read ? POLLIN : 0) |
                                           (entries[i].want_write ? POLLOUT : 0));
    }
    int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
    if (ready < 0) {
        // EINTR: a signal (e.g. Ctrl+C) interrupted the wait. Report a timeout so the caller
        // checks its stop flag.
        return errno == EINTR ? 0 : -1;
    }
    // revents: what actually happened. POLLERR, POLLHUP and POLLNVAL are reported even when
    // not asked for.
    for (std::size_t i = 0; i < entries.size(); ++i) {
        short happened = fds[i].revents;
        entries[i].readable = (happened & POLLIN) != 0;
        entries[i].writable = (happened & POLLOUT) != 0;
        entries[i].failed = (happened & (POLLERR | POLLHUP | POLLNVAL)) != 0;
    }
    return ready;
}

long long receive(SocketHandle socket, char* buffer, std::size_t size) {
    ssize_t n;
    do {
        n = recv(to_native(socket), buffer, size, 0);
    } while (n < 0 && errno == EINTR);  // retry if a signal interrupted the call
    if (n < 0 && would_block()) return kWouldBlock;
    return n;
}

long long send_some(SocketHandle socket, std::string_view data) {
    ssize_t sent;
    do {
        // MSG_NOSIGNAL: if the client has gone, return an error instead of raising SIGPIPE,
        // which would kill tachyon.
        sent = send(to_native(socket), data.data(), data.size(), MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0 && would_block()) return kWouldBlock;
    return sent;
}

}  // namespace tachyon
