#include "horizon/crc32.h"

#include <gtest/gtest.h>

#include <string>

// "123456789" is the standard check value every CRC32 implementation must match.
TEST(Crc32, MatchesStandardCheckValue) {
    EXPECT_EQ(horizon::crc32(std::string("123456789")), 0xCBF43926u);
}

TEST(Crc32, EmptyInputIsZero) { EXPECT_EQ(horizon::crc32(std::string()), 0u); }

TEST(Crc32, OneFlippedBitChangesTheChecksum) {
    std::string data = "SET key value";
    std::uint32_t before = horizon::crc32(data);
    data[3] ^= 0x01;
    EXPECT_NE(horizon::crc32(data), before);
}

// Bytes above 127 are negative as char; they must still give the standard result.
TEST(Crc32, HighBytes) {
    std::string data = "\xFF\x80";
    EXPECT_EQ(horizon::crc32(data), 0x3F456CADu);
}
