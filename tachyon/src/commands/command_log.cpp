#include "commands/command_log.h"

#include <filesystem>
#include <system_error>

#include "commands/commands.h"
#include "protocol/resp_parser.h"
#include "protocol/resp_writer.h"

namespace tachyon {

namespace {

// Chunk limits for big hashes and lists, comfortably inside the parser's kMaxArgs (1024) and
// kMaxCommandBytes (1 MB). A single element bigger than the byte budget still gets a chunk of
// its own; it arrived in a command that fit, so it fits again.
const std::size_t kChunkElements = 500;
const std::size_t kChunkBytes = 512 * 1024;

// Starts a new "<name> <key>" command in `commands` when the current one is full.
void add_to_chunk(std::vector<std::vector<std::string>>& commands, const std::string& name,
                  const std::string& key, std::size_t& chunk_bytes, std::size_t& chunk_elements,
                  const std::string& element) {
    if (commands.empty() || chunk_elements >= kChunkElements ||
        (chunk_elements > 0 && chunk_bytes + element.size() > kChunkBytes)) {
        commands.push_back({name, key});
        chunk_bytes = 0;
        chunk_elements = 0;
    }
    commands.back().push_back(element);
    chunk_bytes += element.size();
    ++chunk_elements;
}

}  // namespace

std::vector<std::vector<std::string>> rebuild_commands(const std::string& key, const Value& value,
                                                       std::int64_t expiry_ms) {
    std::vector<std::vector<std::string>> commands;
    if (const std::string* text = std::get_if<std::string>(&value)) {
        commands.push_back({"SET", key, *text});
    } else if (const Hash* hash = std::get_if<Hash>(&value)) {
        std::size_t bytes = 0;
        std::size_t elements = 0;
        for (const std::pair<const std::string, std::string>& field : *hash) {
            // A field and its value must land in the same HSET, so count them as one element.
            add_to_chunk(commands, "HSET", key, bytes, elements, field.first);
            commands.back().push_back(field.second);
            bytes += field.second.size();
        }
    } else if (const List* list = std::get_if<List>(&value)) {
        std::size_t bytes = 0;
        std::size_t elements = 0;
        for (const std::string& element : *list) {
            add_to_chunk(commands, "RPUSH", key, bytes, elements, element);  // keeps the order
        }
    }
    if (expiry_ms != kNoExpiry) commands.push_back({"PEXPIREAT", key, std::to_string(expiry_ms)});
    return commands;
}

bool CommandLog::open(const std::string& dir, horizon::FlushPolicy policy, Store& store,
                      std::string& error) {
    // Before 1.0 the whole log was one file, tachyon.wal. Its records have the same format as a
    // first-generation log, so if nothing newer exists it simply becomes tachyon.log.1.
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string legacy = dir + "/tachyon.wal";
    const std::string first_log = dir + "/" + kDataFileName + ".log.1";
    if (fs::exists(legacy, ec) && !fs::exists(dir + "/" + kDataFileName + ".snapshot", ec) &&
        !fs::exists(first_log, ec)) {
        fs::rename(legacy, first_log, ec);
        if (ec) {
            error = "could not rename " + legacy + " to " + first_log + " (" + ec.message() + ")";
            return false;
        }
        migrated_legacy_log_ = true;
    }

    if (!engine_.open(dir, kDataFileName, policy, error)) return false;

    std::string payload;
    horizon::RecoveryStatus status;
    while ((status = engine_.next(payload, error)) == horizon::RecoveryStatus::kRecord) {
        // Each record holds one command, encoded exactly as a client would send it.
        RespParser parser;
        parser.feed(payload);
        ParseResult command = parser.next();
        if (command.status != ParseStatus::kCommand) {
            error = "a stored record in " + dir + " isn't a command";
            return false;
        }
        // Expiry times are stored as absolute times, so keys whose time has passed are dropped.
        execute_command(store, command.args, current_time_ms());  // the reply is thrown away
        ++replayed_commands_;
    }
    if (status == horizon::RecoveryStatus::kError) return false;
    snapshot_commands_ = engine_.snapshot_records();
    replayed_commands_ -= snapshot_commands_;  // count only the log's commands as "replayed"
    return engine_.start(error);
}

bool CommandLog::record(const std::vector<std::string>& args) {
    if (!is_write_command(args)) return true;
    // Logged before it runs, even if it will fail (e.g. a syntax error): replaying it then
    // fails the same way, so the store ends up identical.
    return engine_.append(encode_command(args));
}

horizon::SnapshotResult CommandLog::save(const Store& store, std::int64_t now_ms,
                                         std::string& error) {
    if (!engine_.begin_snapshot(error)) return horizon::SnapshotResult::kFailed;
    for (const std::pair<const std::string, Value>& entry : store.entries()) {
        const std::int64_t expiry = store.expiry(entry.first);
        if (expiry != kNoExpiry && expiry <= now_ms) continue;  // already expired
        const std::vector<std::vector<std::string>> commands =
            rebuild_commands(entry.first, entry.second, expiry);
        for (const std::vector<std::string>& command : commands) {
            if (!engine_.add_to_snapshot(encode_command(command), error)) {
                engine_
                    .abandon_snapshot();  // an incomplete snapshot must never replace the old one
                return horizon::SnapshotResult::kFailed;
            }
        }
    }
    return engine_.finish_snapshot(error);
}

}  // namespace tachyon
