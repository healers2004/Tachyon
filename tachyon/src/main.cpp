#include <charconv>
#include <csignal>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

#include "platform/socket.h"
#include "server/server.h"

namespace {  // anonymous namespace: visible only in this file

// Runs on Ctrl+C (SIGINT) or `kill` (SIGTERM). Only sets a flag: a handler must not do I/O.
// On Windows the C runtime calls this on a separate thread; the flag is still enough here.
void on_stop_signal(int) { tachyon::request_stop(); }

const char* const kUsage =
    "usage: tachyon [--port N] [--dir PATH] [--fsync always|everysec] [--no-persist]";

// Parses a port number. from_chars parses without exceptions; `ptr` is where it stopped, so a
// value like "80x" is caught by checking that it reached the end.
bool parse_port(std::string_view value, unsigned short& port) {
    int number = 0;
    std::from_chars_result result =
        std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size() || number < 1 ||
        number > 65535) {
        std::cerr << "tachyon: invalid port '" << value << "', expected 1-65535" << std::endl;
        return false;
    }
    port = static_cast<unsigned short>(number);
    return true;
}

// Reads the command line into `options`. Prints why and returns false on anything unknown.
bool parse_args(int argc, char* argv[], tachyon::ServerOptions& options) {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--no-persist") {
            options.persist = false;
            continue;
        }
        // Every other option takes a value.
        if (i + 1 == argc) {
            std::cerr << kUsage << std::endl;
            return false;
        }
        std::string_view value = argv[++i];
        if (arg == "--port") {
            if (!parse_port(value, options.port)) return false;
        } else if (arg == "--dir") {
            options.dir = std::string(value);
        } else if (arg == "--fsync" && value == "always") {
            options.flush = horizon::FlushPolicy::kEveryWrite;
        } else if (arg == "--fsync" && value == "everysec") {
            options.flush = horizon::FlushPolicy::kEverySecond;
        } else {
            std::cerr << kUsage << std::endl;
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    tachyon::ServerOptions options;
    if (!parse_args(argc, argv, options)) {
        return 1;
    }

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

    return tachyon::run_server(options);
}
