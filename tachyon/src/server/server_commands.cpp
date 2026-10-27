#include "server/server_commands.h"

#include <algorithm>
#include <cstddef>

#include "commands/commands.h"
#include "protocol/resp_writer.h"
#include "server/logging.h"
#include "version.h"

namespace tachyon {

namespace {

bool wants(const std::vector<std::string>& sections, const std::string& name) {
    if (sections.empty()) return true;
    for (const std::string& section : sections) {
        const std::string lower = to_lower(section);
        if (lower == name || lower == "all" || lower == "everything" || lower == "default") {
            return true;
        }
    }
    return false;
}

// One "name:value" line, as INFO prints them.
std::string field(const std::string& name, const std::string& value) {
    return name + ":" + value + "\r\n";
}

std::string number(std::uint64_t value) { return std::to_string(value); }

// Starts an INFO section. Sections are separated by a blank line, as in Redis.
void start_section(std::string& text, const char* title) {
    if (!text.empty()) text += "\r\n";
    text += std::string("# ") + title + "\r\n";
}

}  // namespace

std::string save_snapshot(ServerContext& context) {
    if (context.log == nullptr) return "persistence is disabled (--no-persist)";
    std::string error;
    const std::int64_t now_ms = current_time_ms();
    const horizon::SnapshotResult result = context.log->save(context.store, now_ms, error);
    context.stats.last_save_ok = result == horizon::SnapshotResult::kSaved;
    if (result == horizon::SnapshotResult::kSaved) {
        context.stats.last_save_ms = now_ms;
        context.stats.changes_since_save = 0;
        ++context.stats.saves;
        context.next_snapshot_at =
            std::max(context.options.snapshot_after_bytes, context.log->snapshot_bytes());
        log_line(LogLevel::kNotice, "snapshot saved: " + number(context.store.size()) +
                                        " keys, log generation " +
                                        number(context.log->generation()));
        return "";
    }
    if (result == horizon::SnapshotResult::kFatal) context.fatal = true;
    log_line(LogLevel::kWarning,
             "snapshot failed: " + error +
                 (context.fatal ? "; stopping, since new writes have nowhere safe to go"
                                : "; still running on the previous snapshot and log"));
    return error;
}

std::string info_text(const ServerContext& context, const std::vector<std::string>& sections) {
    const ServerStats& stats = context.stats;
    const std::int64_t now_ms = current_time_ms();
    std::string text;

    if (wants(sections, "server")) {
        start_section(text, "Server");
        text += field("tachyon_version", kVersion);
        // Some clients read redis_version to decide which commands they may use.
        text += field("redis_version", "7.0.0");
        text += field("tcp_port", number(context.options.port));
        text += field("uptime_in_seconds", number((now_ms - stats.start_ms) / 1000));
    }
    if (wants(sections, "clients")) {
        start_section(text, "Clients");
        text += field("connected_clients", number(stats.connected_clients));
        text += field("pubsub_channels", number(context.pubsub.channel_count()));
    }
    if (wants(sections, "persistence")) {
        start_section(text, "Persistence");
        const bool on = context.log != nullptr;
        text += field("loading", "0");
        text += field("persistence_enabled", on ? "1" : "0");
        if (on) {
            text += field("fsync_policy", context.options.flush == horizon::FlushPolicy::kEveryWrite
                                              ? "always"
                                              : "everysec");
            text += field("log_size_bytes", number(context.log->log_size()));
            text += field("log_generation", number(context.log->generation()));
            text += field("snapshot_after_bytes", number(context.options.snapshot_after_bytes));
        }
        text += field("rdb_changes_since_last_save", number(stats.changes_since_save));
        text += field("rdb_saves", number(stats.saves));
        text += field("rdb_last_save_time", number(stats.last_save_ms / 1000));
        text += field("rdb_last_bgsave_status", stats.last_save_ok ? "ok" : "err");
    }
    if (wants(sections, "stats")) {
        start_section(text, "Stats");
        text += field("total_connections_received", number(stats.connections_received));
        text += field("total_commands_processed", number(stats.commands_processed));
    }
    if (wants(sections, "keyspace")) {
        start_section(text, "Keyspace");
        if (context.store.size() > 0) {
            text += field("db0", "keys=" + number(context.store.size()) + ",expires=" +
                                     number(context.store.expiring_count()) + ",avg_ttl=0");
        }
    }
    return text;
}

bool run_server_command(ServerContext& context, const std::vector<std::string>& args,
                        std::string& reply) {
    if (args.empty()) return false;
    if (is_command(args[0], "save")) {
        if (args.size() != 1) {
            reply += error_reply("ERR wrong number of arguments for 'save' command");
            return true;
        }
        const std::string error = save_snapshot(context);
        reply += error.empty() ? simple_string_reply("OK") : error_reply("ERR " + error);
        return true;
    }
    if (is_command(args[0], "info")) {
        const std::vector<std::string> sections(args.begin() + 1, args.end());
        reply += bulk_reply(info_text(context, sections));
        return true;
    }
    return false;
}

}  // namespace tachyon
