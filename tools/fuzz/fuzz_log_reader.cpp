// libFuzzer target for Horizon's log reader: every input is written to a file and read back
// as a log. Besides "doesn't crash", it checks that the reader accounts for every byte, and
// that the records it accepted, written out again, reproduce the file's valid part exactly.
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "horizon/log.h"

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string bytes;
    char c;
    while (in.get(c)) bytes.push_back(c);
    return bytes;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // One file per process, so parallel fuzzing jobs don't share it.
    const std::string path = "/tmp/horizon_fuzz_" + std::to_string(getpid()) + ".log";
    const std::string copy = path + ".copy";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    horizon::LogReader reader;
    std::string error;
    if (!reader.open(path, error)) __builtin_trap();  // the file exists and is readable
    std::vector<std::string> records;
    std::string payload;
    horizon::ReadStatus status;
    std::uint64_t counted = 0;
    while ((status = reader.next(payload)) == horizon::ReadStatus::kRecord) {
        records.push_back(payload);
        counted += 8 + payload.size();
    }

    // Every byte is either part of a valid record or reported as a bad tail.
    if (reader.file_size() != size || counted != reader.valid_bytes() ||
        reader.valid_bytes() > size) {
        __builtin_trap();
    }
    if (status == horizon::ReadStatus::kEnd && reader.valid_bytes() != size) __builtin_trap();
    if (status != horizon::ReadStatus::kEnd && reader.valid_bytes() == size) __builtin_trap();

    // Writing the accepted records again must give back exactly the valid prefix.
    {
        horizon::Log log;
        if (!log.open(copy, 0, horizon::FlushPolicy::kEverySecond, error)) __builtin_trap();
        for (const std::string& record : records) log.append(record);
        if (!log.commit(error)) __builtin_trap();
    }
    const std::string rewritten = read_file(copy);
    if (rewritten != std::string(reinterpret_cast<const char*>(data), reader.valid_bytes())) {
        __builtin_trap();
    }
    std::remove(path.c_str());
    std::remove(copy.c_str());
    return 0;
}
