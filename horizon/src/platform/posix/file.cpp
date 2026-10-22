// POSIX (Linux) implementation of platform/file.h.
#include "platform/file.h"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace horizon {

namespace {

int to_native(FileHandle file) { return static_cast<int>(file); }

}  // namespace

void close_file(FileHandle file) { close(to_native(file)); }

File open_for_append(const std::string& path, std::uint64_t size, std::string& error) {
    // O_CREAT creates a missing file with permissions 0644 (owner read/write, others read).
    // O_CLOEXEC: don't leak the descriptor into child processes.
    File file(open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644));
    if (!file.valid()) {
        error = "could not open " + path + " (" + last_file_error() + ")";
        return File();
    }
    // ftruncate cuts the file to `size`; lseek moves the write position to the new end.
    if (ftruncate(to_native(file.get()), static_cast<off_t>(size)) != 0 ||
        lseek(to_native(file.get()), 0, SEEK_END) < 0) {
        error = "could not prepare " + path + " (" + last_file_error() + ")";
        return File();
    }
    return file;
}

bool write_all(FileHandle file, std::span<const char> data) {
    while (!data.empty()) {
        ssize_t written = write(to_native(file), data.data(), data.size());
        if (written < 0) {
            if (errno == EINTR) continue;  // a signal interrupted the call; nothing was written
            return false;
        }
        data = data.subspan(static_cast<std::size_t>(written));  // drop what was written
    }
    return true;
}

// shortcut: fsync also flushes metadata such as the modification time; fdatasync would skip
// that and be a little faster.
bool sync_file(FileHandle file) { return fsync(to_native(file)) == 0; }

std::string last_file_error() { return std::strerror(errno); }

}  // namespace horizon
