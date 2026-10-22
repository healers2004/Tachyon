#include "commands/command_log.h"

#include "commands/commands.h"
#include "protocol/resp_parser.h"
#include "protocol/resp_writer.h"

namespace tachyon {

bool CommandLog::open(const std::string& path, horizon::FlushPolicy policy, Store& store,
                      std::string& error) {
    std::uint64_t valid_bytes = 0;
    if (!replay(path, store, valid_bytes, error)) return false;
    return log_.open(path, valid_bytes, policy, error);
}

bool CommandLog::replay(const std::string& path, Store& store, std::uint64_t& valid_bytes,
                        std::string& error) {
    // The reader is local, so its file is closed again before the log opens it for writing.
    horizon::LogReader reader;
    if (!reader.open(path, error)) return false;

    std::string payload;
    while (true) {
        horizon::ReadStatus status = reader.next(payload);
        if (status == horizon::ReadStatus::kEnd) break;
        if (status == horizon::ReadStatus::kTornTail) {
            // The crash interrupted this write, so it was never acknowledged: safe to drop.
            dropped_bytes_ = reader.file_size() - reader.valid_bytes();
            break;
        }
        if (status == horizon::ReadStatus::kCorrupt) {
            error = path + " is damaged at byte " + std::to_string(reader.valid_bytes()) +
                    "; refusing to start rather than lose the records after it";
            return false;
        }
        if (status == horizon::ReadStatus::kError) {
            error = "could not read " + path;
            return false;
        }

        // Each record holds one command, encoded exactly as a client would send it.
        RespParser parser;
        parser.feed(payload);
        ParseResult command = parser.next();
        if (command.status != ParseStatus::kCommand) {
            error = path + " has a record that isn't a command at byte " +
                    std::to_string(reader.valid_bytes());
            return false;
        }
        execute_command(store, command.args);  // the reply is thrown away during replay
        ++replayed_commands_;
    }
    valid_bytes = reader.valid_bytes();
    return true;
}

bool CommandLog::record(const std::vector<std::string>& args) {
    if (!is_write_command(args)) return true;
    // Logged before it runs, even if it will fail (e.g. a syntax error): replaying it then
    // fails the same way, so the store ends up identical.
    return log_.append(encode_command(args));
}

}  // namespace tachyon
