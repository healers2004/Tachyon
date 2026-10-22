#include <csignal>
#include <iostream>

#include "platform/socket.h"
#include "server/server.h"

namespace {  // anonymous namespace: visible only in this file, like `static` in C

// Runs on Ctrl+C (SIGINT) or `kill` (SIGTERM). Only sets a flag: a handler must not do I/O.
// On Windows the C runtime calls this on a separate thread; the flag is still enough here.
void on_stop_signal(int) { tachyon::request_stop(); }

}  // namespace

int main() {
    // std::signal returns SIG_ERR if the handler can't be installed.
    if (std::signal(SIGINT, on_stop_signal) == SIG_ERR ||
        std::signal(SIGTERM, on_stop_signal) == SIG_ERR) {
        std::cerr << "tachyon: failed to install signal handlers" << std::endl;
        return 1;
    }

    // RAII: the constructor starts the socket library and the destructor stops it when main
    // returns, on every path out of main.
    tachyon::SocketLibrary sockets;
    if (!sockets.ok()) {
        std::cerr << "tachyon: could not start the socket library" << std::endl;
        return 1;
    }

    // Localhost only: nothing else on the network can reach tachyon.
    return tachyon::run_server("127.0.0.1", tachyon::kDefaultPort);
}
