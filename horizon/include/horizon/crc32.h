// CRC32 checksum (the IEEE polynomial used by zip, PNG and Ethernet). Each log record stores
// the CRC32 of its payload, so a half-written or damaged record is detected on replay.
#pragma once

#include <cstdint>
#include <span>

namespace horizon {

// std::span<const char>: a pointer + length view of bytes, like string_view but for any
// contiguous bytes; a std::string or a char array converts to it automatically.
std::uint32_t crc32(std::span<const char> data);

}  // namespace horizon
