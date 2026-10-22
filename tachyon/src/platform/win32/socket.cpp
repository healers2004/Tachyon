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

SocketHandle listen_tcp(const std::string& ip, unsigned short port, std::string& error) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = last_error("socket()");
        return kInvalidSocket;
    }

    // Stop any other program binding the same port while tachyon holds it. (Windows'
    // SO_REUSEADDR would do the opposite and let another process steal the port.)
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));

    sockaddr_in address{};  // {} zero-fills every field, like memset(&address, 0, ...)
    address.sin_family = AF_INET;
    address.sin_port = htons(port);  // network byte order (big-endian)
    // inet_pton turns "127.0.0.1" into binary; returns 1 on success.
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        error = "invalid IP address '" + ip + "'";
        closesocket(s);
        return kInvalidSocket;
    }

    // reinterpret_cast: treat the sockaddr_in as the generic sockaddr the API expects.
    if (bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        error =
            last_error("bind()") + "; is another program using port " + std::to_string(port) + "?";
        closesocket(s);
        return kInvalidSocket;
    }
    if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
        error = last_error("listen()");
        closesocket(s);
        return kInvalidSocket;
    }
    return static_cast<SocketHandle>(s);
}

int wait_readable(SocketHandle socket, int timeout_ms) {
    // shortcut: select() watches one socket at a time; Phase 2 replaces it with WSAPoll.
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(to_native(socket), &readable);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    // Returns the number of ready sockets, 0 on timeout, SOCKET_ERROR on failure.
    // The first argument is ignored on Windows.
    int ready = select(0, &readable, nullptr, nullptr, &timeout);
    if (ready == SOCKET_ERROR) {
        return -1;
    }
    return ready > 0 ? 1 : 0;
}

SocketHandle accept_client(SocketHandle listener) {
    SOCKET client = accept(to_native(listener), nullptr, nullptr);
    if (client == INVALID_SOCKET) {
        return kInvalidSocket;
    }
    return static_cast<SocketHandle>(client);
}

long long receive(SocketHandle socket, char* buffer, std::size_t size) {
    // recv takes an int length; callers pass small buffers, so the cast is safe.
    int n = recv(to_native(socket), buffer, static_cast<int>(size), 0);
    return n == SOCKET_ERROR ? -1 : n;
}

bool send_all(SocketHandle socket, std::string_view data) {
    while (!data.empty()) {
        int sent = send(to_native(socket), data.data(), static_cast<int>(data.size()), 0);
        if (sent == SOCKET_ERROR) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));  // drop what was sent
    }
    return true;
}

void close_socket(SocketHandle socket) { closesocket(to_native(socket)); }

}  // namespace tachyon
