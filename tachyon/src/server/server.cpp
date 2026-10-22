#include "server/server.h"

#include <csignal>
#include <cstddef>
#include <iostream>
#include <string_view>

#include "commands/commands.h"
#include "commands/store.h"
#include "platform/socket.h"
#include "protocol/resp_parser.h"
#include "protocol/resp_writer.h"

namespace tachyon {

namespace {

// Set by request_stop() from the signal handler. sig_atomic_t is the one type a handler may
// safely write; volatile makes every loop re-read it instead of caching it.
volatile std::sig_atomic_t g_stop_requested = 0;

// How often a blocked wait wakes up to check g_stop_requested.
const int kPollIntervalMs = 100;
const std::size_t kReadBufferSize = 4096;

// Reads commands from one client and replies, until the client disconnects, sends bad
// input, or a stop is requested.
void serve_client(SocketHandle client, Store& store) {
    RespParser parser;
    char buffer[kReadBufferSize];

    while (!g_stop_requested) {
        int ready = wait_readable(client, kPollIntervalMs);
        if (ready == 0) continue;
        if (ready < 0) return;

        long long n = receive(client, buffer, sizeof(buffer));
        if (n <= 0) return;  // 0: the client closed the connection; -1: error
        parser.feed(std::string_view(buffer, static_cast<std::size_t>(n)));

        // One read can hold several commands (clients may pipeline them), so handle them all.
        while (true) {
            ParseResult result = parser.next();
            if (result.status == ParseStatus::kNeedMore) break;
            if (result.status == ParseStatus::kError) {
                // Like Redis: say why, then hang up, since the rest of the stream can't be trusted.
                send_all(client, error_reply("ERR " + result.error));
                std::cout << "tachyon: closing client: " << result.error << std::endl;
                return;
            }
            if (!send_all(client, execute_command(store, result.args))) return;
        }
    }
}

}  // namespace

void request_stop() { g_stop_requested = 1; }

int run_server(const std::string& ip, unsigned short port) {
    std::string error;
    SocketHandle listener = listen_tcp(ip, port, error);
    if (listener == kInvalidSocket) {
        std::cerr << "tachyon: " << error << std::endl;
        return 1;
    }
    std::cout << "tachyon: listening on " << ip << ":" << port << ", press Ctrl+C to stop"
              << std::endl;

    // One store for the server's lifetime, so every client sees the same data.
    Store store;
    int exit_code = 0;

    // shortcut: one client at a time; a second client waits until the first disconnects.
    // Phase 2 replaces this with an event loop that serves many clients.
    while (!g_stop_requested) {
        int ready = wait_readable(listener, kPollIntervalMs);
        if (ready == 0) continue;
        if (ready < 0) {
            std::cerr << "tachyon: waiting for clients failed" << std::endl;
            exit_code = 1;
            break;
        }

        SocketHandle client = accept_client(listener);
        if (client == kInvalidSocket) continue;  // e.g. the client gave up before we accepted
        std::cout << "tachyon: client connected" << std::endl;
        serve_client(client, store);
        close_socket(client);
        std::cout << "tachyon: client disconnected" << std::endl;
    }

    close_socket(listener);
    if (exit_code == 0) {
        std::cout << "tachyon: stopped cleanly" << std::endl;
    }
    return exit_code;
}

}  // namespace tachyon
