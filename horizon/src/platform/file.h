// Write-side file access with an explicit flush to the disk, which the C++ standard library
// can't do. Implemented twice: platform/posix/file.cpp (Linux) and platform/win32/file.cpp;
// CMake compiles only the one for the current OS. Private to Horizon.
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace horizon {

// A POSIX file descriptor or a Windows HANDLE, both fit in a pointer-sized integer.
using FileHandle = std::intptr_t;
const FileHandle kInvalidFile = -1;

// Closes a raw handle. Use File instead, which calls this for you exactly once.
void close_file(FileHandle file);

// Owns one open file and closes it in its destructor. Move-only, like tachyon's Socket.
class File {
public:
    File() = default;
    explicit File(FileHandle handle) : handle_(handle) {}
    ~File() { reset(); }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    File(File&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalidFile; }
    File& operator=(File&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = kInvalidFile;
        }
        return *this;
    }

    bool valid() const { return handle_ != kInvalidFile; }
    FileHandle get() const { return handle_; }

    void reset() {
        if (valid()) {
            close_file(handle_);
            handle_ = kInvalidFile;
        }
    }

private:
    FileHandle handle_ = kInvalidFile;
};

// Opens `path` for writing (creating it if missing), cuts it to `size` bytes and positions at
// the end, so the next write appends. `size` must not exceed the current file size.
// Returns an empty File and sets `error` on failure.
File open_for_append(const std::string& path, std::uint64_t size, std::string& error);

// Writes all of `data` at the current position, looping over partial writes. false on error.
bool write_all(FileHandle file, std::span<const char> data);

// Returns once everything written so far is on the storage device, not just in the OS cache:
// fsync() on Linux, FlushFileBuffers() on Windows. Slow (milliseconds); false on error.
bool sync_file(FileHandle file);

// Text for the last failed call above, e.g. "No such file or directory".
std::string last_file_error();

}  // namespace horizon
