// Winsock implementation of platform/socket.h.
#include "platform/socket.h"

// Keep windows.h small and stop it defining min/max macros.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

namespace tachyon {

namespace {

SOCKET to_native(SocketHandle socket) { return static_cast<SOCKET>(socket); }

// Winsock reports errors through WSAGetLastError(), not errno.
std::string last_error(const std::string& call) {
    return call + " failed (WSA error " + std::to_string(WSAGetLastError()) + ")";
}

// Winsock lengths are ints; cap one call so a huge buffer can't overflow the cast.
int capped_length(std::size_t size) {
    const std::size_t kMaxChunk = 1 << 30;
    return static_cast<int>(size < kMaxChunk ? size : kMaxChunk);
}

}  // namespace

// WSAStartup loads Winsock 2.2; it returns 0 on success or an error code.
SocketLibrary::SocketLibrary() {
    WSADATA data;
    ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

SocketLibrary::~SocketLibrary() {
    if (ok_) {
        WSACleanup();
    }
}

void close_socket(SocketHandle socket) { closesocket(to_native(socket)); }

Socket listen_tcp(const std::string& ip, unsigned short port, std::string& error) {
    // Owned from the start: every early return below closes it through the destructor.
    // INVALID_SOCKET converts to kInvalidSocket (-1), so valid() works here too.
    Socket s(static_cast<SocketHandle>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
    if (!s.valid()) {
        error = last_error("socket()");
        return Socket();
    }

    // Stop any other program binding the same port while tachyon holds it. (Windows'
    // SO_REUSEADDR would do the opposite and let another process steal the port.)
    BOOL exclusive = TRUE;
    setsockopt(to_native(s.get()), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in address{};  // {} zero-fills every field
    address.sin_family = AF_INET;
    address.sin_port = htons(port);  // network byte order (big-endian)
    // inet_pton turns "127.0.0.1" into binary; returns 1 on success.
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        error = "invalid IP address '" + ip + "'";
        return Socket();
    }

    // reinterpret_cast: treat the sockaddr_in as the generic sockaddr the API expects.
    if (bind(to_native(s.get()), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
        SOCKET_ERROR) {
        error =
            last_error("bind()") + "; is another program using port " + std::to_string(port) + "?";
        return Socket();
    }
    if (listen(to_native(s.get()), SOMAXCONN) == SOCKET_ERROR) {
        error = last_error("listen()");
        return Socket();
    }
    // Non-blocking, so accept() returns at once when the waiting client has already gone.
    if (!set_non_blocking(s.get())) {
        error = last_error("ioctlsocket()");
        return Socket();
    }
    return s;  // moved out to the caller, not copied
}

Socket accept_client(SocketHandle listener) {
    SOCKET client = accept(to_native(listener), nullptr, nullptr);
    // Fails with WSAEWOULDBLOCK when no client is waiting; the caller just stops accepting.
    if (client == INVALID_SOCKET) {
        return Socket();
    }
    return Socket(static_cast<SocketHandle>(client));
}

bool set_non_blocking(SocketHandle socket) {
    // FIONBIO with a non-zero value switches non-blocking mode on. Returns 0 on success.
    u_long enabled = 1;
    return ioctlsocket(to_native(socket), FIONBIO, &enabled) == 0;
}

int poll_sockets(std::vector<PollEntry>& entries, int timeout_ms) {
    // shortcut: rebuilds the array on every call; fine for hundreds of clients, IOCP would
    // scale further (see Future improvements).
    std::vector<WSAPOLLFD> fds(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        fds[i].fd = to_native(entries[i].socket);
        // WSAPoll rejects some POSIX flags (e.g. POLLPRI), so ask only for normal data.
        fds[i].events = static_cast<SHORT>((entries[i].want_read ? POLLRDNORM : 0) |
                                           (entries[i].want_write ? POLLWRNORM : 0));
    }
    // WSAPoll is Winsock's poll(). Ctrl+C doesn't interrupt it (the handler runs on another
    // thread), so the caller's timeout is what lets the stop flag be noticed.
    int ready = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
    if (ready == SOCKET_ERROR) {
        return -1;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        SHORT happened = fds[i].revents;
        entries[i].readable = (happened & POLLRDNORM) != 0;
        entries[i].writable = (happened & POLLWRNORM) != 0;
        // A peer that closes its side shows up as POLLHUP; a read then returns 0.
        entries[i].failed = (happened & (POLLERR | POLLHUP | POLLNVAL)) != 0;
    }
    return ready;
}

long long receive(SocketHandle socket, char* buffer, std::size_t size) {
    int n = recv(to_native(socket), buffer, capped_length(size), 0);
    if (n == SOCKET_ERROR) {
        return WSAGetLastError() == WSAEWOULDBLOCK ? kWouldBlock : -1;
    }
    return n;
}

long long send_some(SocketHandle socket, std::string_view data) {
    int sent = send(to_native(socket), data.data(), capped_length(data.size()), 0);
    if (sent == SOCKET_ERROR) {
        return WSAGetLastError() == WSAEWOULDBLOCK ? kWouldBlock : -1;
    }
    return sent;
}

}  // namespace tachyon
