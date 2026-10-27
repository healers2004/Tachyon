#include "server/server.h"

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "commands/command_log.h"
#include "platform/socket.h"
#include "protocol/resp_writer.h"
#include "server/connection.h"
#include "server/logging.h"
#include "server/server_commands.h"
#include "version.h"

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
// How often the event loop deletes expired keys that nobody has read (active expiry).
const std::int64_t kExpirySweepIntervalMs = 1000;

// unique_ptr: each Connection lives on the heap and is deleted (closing its socket) when its
// unique_ptr is reset or destroyed. Its address never changes as the vector grows, so later
// code (pub/sub, Phase 5) can safely point at a connection.
using ConnectionList = std::vector<std::unique_ptr<Connection>>;

// Accepts every waiting client. The listener is non-blocking, so this stops as soon as no
// one is left waiting.
void accept_new_clients(SocketHandle listener, ConnectionList& connections, ServerStats& stats) {
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
        ++stats.connections_received;
    }
}

// Reads what has arrived and runs it. Returns false if the connection should be closed.
bool read_from(Connection& connection, ServerContext& context) {
    char buffer[kReadChunkSize];
    long long n = receive(connection.socket.get(), buffer, sizeof(buffer));
    if (n == kWouldBlock) return true;  // woken up, but nothing to read after all
    if (n <= 0) return false;           // 0: the client closed the connection; -1: error
    handle_input(connection, context, std::string_view(buffer, static_cast<std::size_t>(n)));
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

// One line at startup that says how the server is set up.
std::string config_summary(const ServerOptions& options) {
    std::string text = std::string("starting version ") + kVersion + ": port " +
                       std::to_string(options.port) + ", ";
    if (!options.persist) return text + "persistence off (--no-persist)";
    text += "persistence on (dir " + options.dir + ", fsync " +
            (options.flush == horizon::FlushPolicy::kEveryWrite ? "always" : "everysec") + ", ";
    if (options.snapshot_after_bytes == 0) return text + "no automatic snapshots)";
    return text + "snapshot after " + std::to_string(options.snapshot_after_bytes) +
           " bytes of log)";
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
    log_line(LogLevel::kNotice, config_summary(options));

    // Everything the commands share. Declared before `connections`, so it's destroyed after
    // them: objects in a function are destroyed in reverse order, and every Connection's
    // destructor still needs the pub/sub registry inside it.
    ServerContext context;
    context.options = options;
    context.stats.start_ms = current_time_ms();
    CommandLog command_log;
    if (options.persist) {
        command_log.set_crash_point(options.crash_point);
        if (!command_log.open(options.dir, options.flush, context.store, error)) {
            log_line(LogLevel::kWarning, error);
            return 1;
        }
        context.log = &command_log;
        context.next_snapshot_at =
            std::max(options.snapshot_after_bytes, command_log.snapshot_bytes());
        if (command_log.migrated_legacy_log()) {
            log_line(LogLevel::kNotice, "renamed the pre-1.0 tachyon.wal to tachyon.log.1");
        }
        log_line(LogLevel::kNotice, "loaded " + std::to_string(command_log.snapshot_commands()) +
                                        " command(s) from the snapshot and replayed " +
                                        std::to_string(command_log.replayed_commands()) +
                                        " from log generation " +
                                        std::to_string(command_log.generation()));
        if (command_log.dropped_bytes() > 0) {
            log_line(LogLevel::kNotice, "dropped a half-written last record (" +
                                            std::to_string(command_log.dropped_bytes()) +
                                            " bytes) left by a crash");
        }
    }

    // Listening only after loading, so no client sees a half-loaded store.
    Socket listener = listen_tcp(options.ip, options.port, error);
    if (!listener.valid()) {
        log_line(LogLevel::kWarning, error);
        return 1;
    }
    log_line(LogLevel::kNotice, "listening on " + options.ip + ":" + std::to_string(options.port) +
                                    ", press Ctrl+C to stop");

    ConnectionList connections;
    std::vector<PollEntry> entries;
    std::int64_t last_sweep_ms = current_time_ms();
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
            log_line(LogLevel::kWarning, "waiting for sockets failed");
            exit_code = 1;
            break;
        }

        // 1. Read: run every complete command that arrived. Replies are only queued.
        for (std::size_t i = 0; i < connections.size(); ++i) {
            const PollEntry& entry = entries[i + 1];
            bool keep = true;
            if (entry.want_read && (entry.readable || entry.failed)) {
                keep = read_from(*connections[i], context);  // a hang-up shows up as a 0 read
            } else if (entry.failed) {
                keep = false;
            }
            if (!keep) connections[i].reset();  // deletes the Connection, closing its socket
        }
        if (context.fatal) {  // a SAVE that couldn't start its new log: send nothing more
            exit_code = 1;
            break;
        }

        // 2. Commit: one disk flush covers every write from this pass (group commit), and no
        // reply is sent before it, so an acknowledged write is always in the log. If the disk
        // is full, stop rather than acknowledge writes that aren't saved.
        if (context.log != nullptr && !context.log->commit(error)) {
            log_line(LogLevel::kWarning,
                     error +
                         ". Stopping without acknowledging this round of writes; every "
                         "write acknowledged before it is safe on disk. Free some space "
                         "and restart.");
            exit_code = 1;
            break;
        }

        // 3. Snapshot when the log has grown enough. Right after the commit, so the store and
        // the log agree. If it fails (e.g. a full disk), keep going on the old snapshot and
        // log, and wait for another threshold's worth of log before trying again.
        if (context.log != nullptr && options.snapshot_after_bytes > 0 &&
            context.log->log_size() >= context.next_snapshot_at) {
            // On success save_snapshot sets the next point; on failure, back off.
            if (!save_snapshot(context).empty()) {
                context.next_snapshot_at = context.log->log_size() + options.snapshot_after_bytes;
            }
            if (context.fatal) {
                exit_code = 1;
                break;
            }
        }

        // 4. Write: send queued replies. Most fit in the OS buffer straight away.
        for (std::size_t i = 0; i < connections.size(); ++i) {
            if (!connections[i]) continue;
            bool keep = write_to(*connections[i]);
            if (keep && over_output_limit(*connections[i])) {
                log_line(LogLevel::kNotice, "closing subscriber: too far behind on messages");
                keep = false;
            }
            if (!keep) connections[i].reset();  // also takes it out of every channel
        }
        remove_closed(connections);

        // poll wakes at least every 100 ms, so this runs about once a second. Deleting an
        // expired key needs no log record: replay sees the same absolute deadline and drops it.
        const std::int64_t now_ms = current_time_ms();
        if (now_ms - last_sweep_ms >= kExpirySweepIntervalMs) {
            context.store.remove_expired(now_ms);
            last_sweep_ms = now_ms;
        }

        // After the loops above, so new clients don't shift the entry-to-connection mapping.
        if (entries[0].readable) accept_new_clients(listener.get(), connections, context.stats);
        context.stats.connected_clients = connections.size();
    }

    // With kEverySecond the last second of writes may not be on disk yet.
    if (exit_code == 0 && context.log != nullptr && !context.log->sync(error)) {
        log_line(LogLevel::kWarning, error);
        exit_code = 1;
    }
    // Returning destroys `connections`, then `command_log` and `context`, then `listener`;
    // their destructors close every socket and file.
    if (exit_code == 0) {
        log_line(LogLevel::kNotice, "stopped cleanly, closing " +
                                        std::to_string(connections.size()) +
                                        " client connection(s)");
    }
    return exit_code;
}

}  // namespace tachyon
