#include "horizon/crc32.h"

#include <vector>

namespace horizon {

namespace {

// 0xEDB88320 is the IEEE polynomial with its bits reversed, because this CRC processes the
// lowest bit of each byte first.
const std::uint32_t kPolynomial = 0xEDB88320u;

// table[b] is the effect of one byte b on the CRC, so the main loop handles a byte per step
// instead of a bit per step.
std::vector<std::uint32_t> build_table() {
    std::vector<std::uint32_t> table(256);
    for (std::uint32_t byte = 0; byte < 256; ++byte) {
        std::uint32_t crc = byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? (crc >> 1) ^ kPolynomial : crc >> 1;
        }
        table[byte] = crc;
    }
    return table;
}

}  // namespace

std::uint32_t crc32(std::span<const char> data) {
    // A function-local static is built once, on the first call (thread-safe since C++11).
    static const std::vector<std::uint32_t> table = build_table();

    std::uint32_t crc = 0xFFFFFFFFu;  // the standard starts with all bits set...
    for (std::size_t i = 0; i < data.size(); ++i) {
        // unsigned char: a char above 127 would otherwise be negative and index out of range.
        unsigned char byte = static_cast<unsigned char>(data[i]);
        crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;  // ...and flips them at the end
}

}  // namespace horizon
