#include "server/server.h"

#include <csignal>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

#include "commands/command_log.h"
#include "commands/store.h"
#include "platform/socket.h"
#include "protocol/resp_writer.h"
#include "server/connection.h"

namespace tachyon {

namespace {

// Set by request_stop() from the signal handler. sig_atomic_t is the one type a handler may
// safely write; volatile makes every loop re-read it instead of caching it.
volatile std::sig_atomic_t g_stop_requested = 0;

// How often poll wakes up to check g_stop_requested when nothing else happens.
const int kPollIntervalMs = 100;
const std::size_t kReadChunkSize = 16 * 1024;
// Stop reading from a client while this many reply bytes are queued for it. A client that
// never reads (a slow reader) then fills its own TCP window and stalls, not the server.
const std::size_t kMaxPendingReplyBytes = 1024 * 1024;
// Like Redis's maxclients: refuse more, rather than running out of file descriptors.
const std::size_t kMaxClients = 1000;

// unique_ptr: each Connection lives on the heap and is deleted (closing its socket) when its
// unique_ptr is reset or destroyed. Its address never changes as the vector grows, so later
// code (pub/sub, Phase 5) can safely point at a connection.
using ConnectionList = std::vector<std::unique_ptr<Connection>>;

// Accepts every waiting client. The listener is non-blocking, so this stops as soon as no
// one is left waiting.
void accept_new_clients(SocketHandle listener, ConnectionList& connections) {
    while (true) {
        Socket client = accept_client(listener);
        if (!client.valid()) return;
        if (connections.size() >= kMaxClients) {
            // Best effort; `client` goes out of scope at `continue` and its destructor closes it.
            send_some(client.get(), error_reply("ERR max number of clients reached"));
            continue;
        }
        // Linux doesn't pass non-blocking mode from the listener to accepted sockets.
        if (!set_non_blocking(client.get())) continue;
        connections.push_back(std::make_unique<Connection>(std::move(client)));
    }
}

// Reads what has arrived and runs it. Returns false if the connection should be closed.
bool read_from(Connection& connection, Store& store, CommandLog* log) {
    char buffer[kReadChunkSize];
    long long n = receive(connection.socket.get(), buffer, sizeof(buffer));
    if (n == kWouldBlock) return true;  // woken up, but nothing to read after all
    if (n <= 0) return false;           // 0: the client closed the connection; -1: error
    handle_input(connection, store, log, std::string_view(buffer, static_cast<std::size_t>(n)));
    return true;
}

// Sends queued replies until they're all out or the OS send buffer is full; poll then says
// when there's room again. Returns false if the connection should be closed.
bool write_to(Connection& connection) {
    while (!connection.pending_reply.empty()) {
        long long sent = send_some(connection.socket.get(), connection.pending_reply);
        if (sent == kWouldBlock) return true;
        if (sent < 0) return false;
        // shortcut: erasing from the front moves the rest of the buffer; an offset would avoid
        // that if profiling (Phase 7) shows it matters.
        connection.pending_reply.erase(0, static_cast<std::size_t>(sent));
    }
    return !connection.closing;  // after a protocol error, close once the error reply is out
}

// Deletes the connections that were reset this round, keeping the order of the rest.
void remove_closed(ConnectionList& connections) {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < connections.size(); ++i) {
        if (connections[i]) {  // an empty unique_ptr tests as false
            connections[kept] = std::move(connections[i]);
            ++kept;
        }
    }
    connections.resize(kept);
}

}  // namespace

void request_stop() { g_stop_requested = 1; }

int run_server(const ServerOptions& options) {
    std::string error;

    // One store for the server's lifetime, so every client sees the same data.
    Store store;
    // `log` points at command_log when persistence is on, and stays nullptr with --no-persist.
    CommandLog command_log;
    CommandLog* log = nullptr;
    if (options.persist) {
        // "/" works as a path separator on Windows too.
        const std::string path = options.dir + "/" + kLogFileName;
        if (!command_log.open(path, options.flush, store, error)) {
            std::cerr << "tachyon: " << error << std::endl;
            return 1;
        }
        log = &command_log;
        std::cout << "tachyon: replayed " << command_log.replayed_commands() << " command(s) from "
                  << path << std::endl;
        if (command_log.dropped_bytes() > 0) {
            std::cout << "tachyon: dropped a half-written last record ("
                      << command_log.dropped_bytes() << " bytes) left by a crash" << std::endl;
        }
    }

    // Listening only after the replay, so no client sees a half-loaded store.
    Socket listener = listen_tcp(options.ip, options.port, error);
    if (!listener.valid()) {
        std::cerr << "tachyon: " << error << std::endl;
        return 1;
    }
    std::cout << "tachyon: listening on " << options.ip << ":" << options.port
              << ", press Ctrl+C to stop" << std::endl;

    ConnectionList connections;
    std::vector<PollEntry> entries;
    int exit_code = 0;

    // The event loop: one thread waits on every socket at once and handles whichever are
    // ready, so a slow or idle client never holds up the others.
    while (!g_stop_requested) {
        // entries[0] is the listener; entries[i + 1] is connections[i].
        entries.clear();
        PollEntry listen_entry;
        listen_entry.socket = listener.get();
        listen_entry.want_read = true;
        entries.push_back(listen_entry);
        for (std::size_t i = 0; i < connections.size(); ++i) {
            const Connection& connection = *connections[i];
            PollEntry entry;
            entry.socket = connection.socket.get();
            entry.want_read =
                !connection.closing && connection.pending_reply.size() < kMaxPendingReplyBytes;
            entry.want_write = !connection.pending_reply.empty();
            entries.push_back(entry);
        }

        // On a timeout every entry reports nothing, so the passes below do almost nothing,
        // but the commit still runs the once-a-second flush.
        int ready = poll_sockets(entries, kPollIntervalMs);
        if (ready < 0) {
            std::cerr << "tachyon: waiting for sockets failed" << std::endl;
            exit_code = 1;
            break;
        }

        // 1. Read: run every complete command that arrived. Replies are only queued.
        for (std::size_t i = 0; i < connections.size(); ++i) {
            const PollEntry& entry = entries[i + 1];
            bool keep = true;
            if (entry.want_read && (entry.readable || entry.failed)) {
                keep = read_from(*connections[i], store, log);  // a hang-up shows up as a 0 read
            } else if (entry.failed) {
                keep = false;
            }
            if (!keep) connections[i].reset();  // deletes the Connection, closing its socket
        }

        // 2. Commit: one disk flush covers every write from this pass (group commit), and no
        // reply is sent before it, so an acknowledged write is always in the log.
        if (log != nullptr && !log->commit(error)) {
            std::cerr << "tachyon: " << error << "; stopping without sending unsaved replies"
                      << std::endl;
            exit_code = 1;
            break;
        }

        // 3. Write: send queued replies. Most fit in the OS buffer straight away.
        for (std::size_t i = 0; i < connections.size(); ++i) {
            if (connections[i] && !write_to(*connections[i])) connections[i].reset();
        }
        remove_closed(connections);

        // After the loop above, so new clients don't shift the entry-to-connection mapping.
        if (entries[0].readable) accept_new_clients(listener.get(), connections);
    }

    // With kEverySecond the last second of writes may not be on disk yet.
    if (exit_code == 0 && log != nullptr && !log->sync(error)) {
        std::cerr << "tachyon: " << error << std::endl;
        exit_code = 1;
    }
    // Returning destroys `connections` and `listener`; their destructors close every socket.
    if (exit_code == 0) {
        std::cout << "tachyon: stopped cleanly, closing " << connections.size()
                  << " client connection(s)" << std::endl;
    }
    return exit_code;
}

}  // namespace tachyon
