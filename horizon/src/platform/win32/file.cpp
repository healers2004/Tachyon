// Windows implementation of platform/file.h.
#include "platform/file.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace horizon {

namespace {

// A HANDLE is a pointer; INVALID_HANDLE_VALUE is (HANDLE)-1, which maps to kInvalidFile.
HANDLE to_native(FileHandle file) { return reinterpret_cast<HANDLE>(file); }

}  // namespace

void close_file(FileHandle file) { CloseHandle(to_native(file)); }

File open_for_append(const std::string& path, std::uint64_t size, std::string& error) {
    // OPEN_ALWAYS opens the file or creates it. The share flags let readers (and tests) open
    // it while it's held here.
    // shortcut: the "A" version takes the path in the local code page, so non-ASCII paths may
    // fail; CreateFileW with a UTF-16 path would fix that.
    HANDLE handle = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    File file(reinterpret_cast<FileHandle>(handle));
    if (!file.valid()) {
        error = "could not open " + path + " (" + last_file_error() + ")";
        return File();
    }
    // Move to `size`, then SetEndOfFile cuts the file there. The position stays at the end.
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) || !SetEndOfFile(handle)) {
        error = "could not prepare " + path + " (" + last_file_error() + ")";
        return File();
    }
    return file;
}

bool write_all(FileHandle file, std::span<const char> data) {
    while (!data.empty()) {
        // WriteFile takes a DWORD (32-bit) length, so write at most 1 GB per call.
        const std::size_t kMaxChunk = 1 << 30;
        DWORD chunk = static_cast<DWORD>(data.size() < kMaxChunk ? data.size() : kMaxChunk);
        DWORD written = 0;
        if (!WriteFile(to_native(file), data.data(), chunk, &written, nullptr)) {
            return false;
        }
        data = data.subspan(written);  // drop what was written
    }
    return true;
}

bool sync_file(FileHandle file) { return FlushFileBuffers(to_native(file)) != 0; }

// MOVEFILE_REPLACE_EXISTING: overwrite `to` in one step. MOVEFILE_WRITE_THROUGH: don't return
// until the rename is on disk, which is what the directory fsync does on Linux.
// shortcut: the "A" version, like CreateFileA above; MoveFileExW would accept any path.
bool replace_file(const std::string& from, const std::string& to) {
    return MoveFileExA(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool sync_directory(const std::string&) { return true; }

std::string last_file_error() { return "Windows error " + std::to_string(GetLastError()); }

}  // namespace horizon
